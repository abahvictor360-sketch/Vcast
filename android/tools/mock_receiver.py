#!/usr/bin/env python3
"""Mock Vcast receiver: the OBS side of the Vcast sender protocol (docs/protocol.md).

Answers UDP discovery, accepts the TCP stream, validates the framing and writes what it
receives to files so a real phone running the Vcast app can be tested without OBS:

    python3 android/tools/mock_receiver.py --name "Test PC" --out /tmp/vcast
    ffplay /tmp/vcast/stream-1.h264      # video (H.264 Annex-B elementary stream)
    ffplay /tmp/vcast/stream-1.aac       # audio (AAC with ADTS headers), if shared

Only the Python 3 standard library is used. Remember to allow UDP 48689 and TCP 48690 in
the PC's firewall.
"""

import argparse
import json
import os
import socket
import struct
import sys
import threading
import time

DISCOVERY_PORT = 48689
DEFAULT_PORT = 48690
QUERY = b"VCAST?\n"
HANDSHAKE = b"VCAST/1\n"
HEADER = struct.Struct(">BIq")  # type u8, length u32, pts_us i64 (big-endian, 13 bytes)
MAX_PAYLOAD = 16 * 1024 * 1024
IDLE_TIMEOUT = 10.0

HELLO, VIDEO_CONFIG, VIDEO_FRAME, VIDEO_KEYFRAME = 0x01, 0x10, 0x11, 0x12
AUDIO_CONFIG, AUDIO_FRAME, PING, BYE = 0x20, 0x21, 0x30, 0x31
NAMES = {
    HELLO: "HELLO", VIDEO_CONFIG: "VIDEO_CONFIG", VIDEO_FRAME: "VIDEO_FRAME",
    VIDEO_KEYFRAME: "VIDEO_KEYFRAME", AUDIO_CONFIG: "AUDIO_CONFIG",
    AUDIO_FRAME: "AUDIO_FRAME", PING: "PING", BYE: "BYE",
}
AAC_RATES = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350]


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


class ProtocolError(Exception):
    pass


def read_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            if not buf:
                return None
            raise ProtocolError("connection closed in the middle of a frame (%d of %d bytes)" % (len(buf), n))
        buf += chunk
    return bytes(buf)


def nal_types(annexb):
    """Returns the NAL unit types (H.264) found after start codes."""
    types, i, n = [], 0, len(annexb)
    while True:
        j = annexb.find(b"\x00\x00\x01", i)
        if j < 0 or j + 3 >= n:
            return types
        types.append(annexb[j + 3] & 0x1F)
        i = j + 3


def starts_with_start_code(b):
    return b.startswith(b"\x00\x00\x00\x01") or b.startswith(b"\x00\x00\x01")


class Bits:
    def __init__(self, data):
        # strip emulation prevention bytes
        out, zeros = bytearray(), 0
        for byte in data:
            if zeros >= 2 and byte == 3:
                zeros = 0
                continue
            out.append(byte)
            zeros = zeros + 1 if byte == 0 else 0
        self.d, self.pos = bytes(out), 0

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.d[self.pos >> 3] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def ue(self):
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
        return (1 << zeros) - 1 + self.u(zeros)

    def se(self):
        v = self.ue()
        return (v + 1) // 2 if v & 1 else -(v // 2)


def h264_sps_size(params):
    """Best-effort width x height from the first SPS in an Annex-B buffer."""
    i = params.find(b"\x00\x00\x01")
    while i >= 0:
        if params[i + 3] & 0x1F == 7:
            end = params.find(b"\x00\x00\x01", i + 3)
            sps = params[i + 4:end if end > 0 else len(params)]
            try:
                b = Bits(sps)
                profile = b.u(8)
                b.u(16)  # constraint flags + level
                b.ue()  # sps id
                chroma = 1
                if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
                    chroma = b.ue()
                    if chroma == 3:
                        b.u(1)
                    b.ue()
                    b.ue()
                    b.u(1)
                    if b.u(1):
                        for k in range(8 if chroma != 3 else 12):
                            if b.u(1):
                                last, nxt = 8, 8
                                for _ in range(16 if k < 6 else 64):
                                    if nxt:
                                        nxt = (last + b.se() + 256) % 256
                                    last = nxt or last
                b.ue()  # log2_max_frame_num
                poc = b.ue()
                if poc == 0:
                    b.ue()
                elif poc == 1:
                    b.u(1)
                    b.se()
                    b.se()
                    for _ in range(b.ue()):
                        b.se()
                b.ue()
                b.u(1)
                w_mbs, h_map = b.ue() + 1, b.ue() + 1
                frame_mbs_only = b.u(1)
                if not frame_mbs_only:
                    b.u(1)
                b.u(1)
                w, h = w_mbs * 16, h_map * 16 * (2 - frame_mbs_only)
                if b.u(1):  # cropping
                    cl, cr, ct, cb = b.ue(), b.ue(), b.ue(), b.ue()
                    sx = 2 if chroma in (1, 2) else 1
                    sy = (2 if chroma == 1 else 1) * (2 - frame_mbs_only)
                    w -= (cl + cr) * sx
                    h -= (ct + cb) * sy
                return "%dx%d (profile %d)" % (w, h, profile)
            except IndexError:
                return "?"
        i = params.find(b"\x00\x00\x01", i + 3)
    return "?"


def adts_header(asc, payload_len):
    """Builds a 7-byte ADTS header from a 2-byte AAC AudioSpecificConfig."""
    obj = asc[0] >> 3
    freq_idx = ((asc[0] & 0x07) << 1) | (asc[1] >> 7)
    chan = (asc[1] >> 3) & 0x0F
    n = payload_len + 7
    return bytes([
        0xFF, 0xF1,
        ((obj - 1) << 6) | (freq_idx << 2) | (chan >> 2),
        ((chan & 3) << 6) | (n >> 11),
        (n >> 3) & 0xFF,
        ((n & 7) << 5) | 0x1F,
        0xFC,
    ])


class Session:
    def __init__(self, sock, addr, index, args):
        self.sock, self.addr, self.index, self.args = sock, addr, index, args
        self.stop = threading.Event()
        self.video_file = self.audio_file = None
        self.asc = None
        self.have_video_config = False
        self.need_keyframe = True
        self.last_pts = {}
        self.counts = {}
        self.bytes = 0
        self.errors = 0
        self.warnings = 0
        self.frames_since_stats = 0
        self.bytes_since_stats = 0
        self.stats_time = time.time()

    def close(self):
        self.stop.set()
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def warn(self, msg):
        self.warnings += 1
        log("[%d] WARNING: %s" % (self.index, msg))

    def open_outputs(self):
        os.makedirs(self.args.out, exist_ok=True)
        base = os.path.join(self.args.out, "stream-%d" % self.index)
        self.video_path, self.audio_path = base + ".h264", base + ".aac"
        self.video_file = open(self.video_path, "wb")

    def run(self):
        try:
            self.sock.settimeout(5.0)
            hs = read_exact(self.sock, len(HANDSHAKE))
            if hs != HANDSHAKE:
                self.sock.sendall(b"VCAST/1 ERR bad handshake\n")
                raise ProtocolError("bad handshake %r" % hs)
            if self.args.refuse:
                self.sock.sendall(("VCAST/1 ERR %s\n" % self.args.refuse).encode())
                log("[%d] refused (%s)" % (self.index, self.args.refuse))
                return
            self.sock.sendall(("VCAST/1 OK %s\n" % self.args.name).encode("utf-8"))
            log("[%d] handshake OK from %s:%d" % (self.index, *self.addr[:2]))
            self.sock.settimeout(IDLE_TIMEOUT)
            self.open_outputs()
            while not self.stop.is_set():
                try:
                    hdr = read_exact(self.sock, HEADER.size)
                except socket.timeout:
                    raise ProtocolError("no frame for %.0f s (sender must PING every 2 s)" % IDLE_TIMEOUT)
                if hdr is None:
                    log("[%d] sender closed the connection without BYE" % self.index)
                    break
                ftype, length, pts = HEADER.unpack(hdr)
                if length > MAX_PAYLOAD:
                    raise ProtocolError("frame length %d exceeds 16 MiB" % length)
                payload = read_exact(self.sock, length) if length else b""
                if payload is None:
                    raise ProtocolError("connection closed before payload")
                if self.handle(ftype, pts, payload):
                    break
                self.maybe_stats()
        except (ProtocolError, OSError) as e:
            if not self.stop.is_set():
                self.errors += 1
                log("[%d] ERROR: %s" % (self.index, e))
        finally:
            self.summary()
            for f in (self.video_file, self.audio_file):
                if f:
                    f.close()
            self.close()

    def check_pts(self, stream, pts):
        last = self.last_pts.get(stream)
        if last is not None and pts < last:
            self.warn("%s pts went backwards: %d < %d" % (stream, pts, last))
        self.last_pts[stream] = pts

    def handle(self, ftype, pts, payload):
        """Returns True when the stream ended (BYE)."""
        name = NAMES.get(ftype)
        if name is None:
            self.warn("unknown frame type 0x%02x (%d bytes) ignored" % (ftype, len(payload)))
            return False
        self.counts[name] = self.counts.get(name, 0) + 1
        self.bytes += HEADER.size + len(payload)
        self.bytes_since_stats += HEADER.size + len(payload)
        if self.args.verbose:
            log("[%d] %-14s %8d bytes pts=%d" % (self.index, name, len(payload), pts))

        if ftype == HELLO:
            try:
                hello = json.loads(payload.decode("utf-8"))
                log("[%d] HELLO %s" % (self.index, hello))
            except ValueError as e:
                self.warn("HELLO is not JSON: %s" % e)
        elif ftype == VIDEO_CONFIG:
            if len(payload) < 5:
                raise ProtocolError("VIDEO_CONFIG too short")
            codec, params = payload[0], payload[1:]
            if codec not in (1, 2):
                self.warn("unknown video codec %d" % codec)
            if not starts_with_start_code(params):
                self.warn("VIDEO_CONFIG parameter sets do not start with an Annex-B start code")
            types = nal_types(params)
            if codec == 1 and not (7 in types and 8 in types):
                self.warn("VIDEO_CONFIG lacks SPS+PPS (NAL types %s)" % types)
            log("[%d] VIDEO_CONFIG codec=%s nal=%s size=%s" % (
                self.index, "H.264" if codec == 1 else "H.265" if codec == 2 else codec, types,
                h264_sps_size(params) if codec == 1 else "?"))
            self.video_file.write(params)
            self.have_video_config = True
            self.need_keyframe = True
        elif ftype in (VIDEO_FRAME, VIDEO_KEYFRAME):
            if not self.have_video_config:
                raise ProtocolError("%s before VIDEO_CONFIG" % name)
            if not starts_with_start_code(payload):
                self.warn("%s does not start with a start code" % name)
            if ftype == VIDEO_KEYFRAME:
                if self.need_keyframe:
                    self.need_keyframe = False
                if 5 not in nal_types(payload):
                    self.warn("VIDEO_KEYFRAME without an IDR slice (NAL types %s)" % nal_types(payload))
            elif self.need_keyframe:
                self.warn("VIDEO_FRAME before the keyframe that must follow VIDEO_CONFIG")
            self.check_pts("video", pts)
            self.video_file.write(payload)
            self.frames_since_stats += 1
        elif ftype == AUDIO_CONFIG:
            if len(payload) < 8:
                raise ProtocolError("AUDIO_CONFIG too short")
            codec, rate, channels = payload[0], struct.unpack(">I", payload[1:5])[0], payload[5]
            asc = payload[6:]
            log("[%d] AUDIO_CONFIG codec=%d rate=%d channels=%d asc=%s" % (
                self.index, codec, rate, channels, asc.hex()))
            if codec != 1:
                self.warn("unknown audio codec %d" % codec)
            else:
                freq_idx = ((asc[0] & 0x07) << 1) | (asc[1] >> 7)
                if freq_idx < len(AAC_RATES) and AAC_RATES[freq_idx] != rate:
                    self.warn("AudioSpecificConfig rate %d != %d" % (AAC_RATES[freq_idx], rate))
            self.asc = asc
            if self.audio_file is None:
                self.audio_file = open(self.audio_path, "wb")
        elif ftype == AUDIO_FRAME:
            if self.asc is None:
                raise ProtocolError("AUDIO_FRAME before AUDIO_CONFIG")
            if payload[:2] == b"\xff\xf1" or payload[:2] == b"\xff\xf9":
                self.warn("AUDIO_FRAME looks like it has an ADTS header (must be raw AAC)")
            self.check_pts("audio", pts)
            self.audio_file.write(adts_header(self.asc, len(payload)) + payload)
        elif ftype in (PING, BYE):
            if payload:
                self.warn("%s with a payload" % name)
            if ftype == BYE:
                log("[%d] BYE" % self.index)
                return True
        return False

    def maybe_stats(self):
        now = time.time()
        dt = now - self.stats_time
        if dt >= 2.0:
            log("[%d] %.1f fps video, %.2f Mbit/s, totals: %s" % (
                self.index, self.frames_since_stats / dt, self.bytes_since_stats * 8 / dt / 1e6,
                ", ".join("%s=%d" % kv for kv in sorted(self.counts.items()))))
            self.stats_time, self.frames_since_stats, self.bytes_since_stats = now, 0, 0

    def summary(self):
        log("[%d] session ended: %d bytes, %s, %d warnings, %d errors" % (
            self.index, self.bytes, self.counts, self.warnings, self.errors))
        if self.video_file:
            log("[%d] video written to %s" % (self.index, self.video_path))
        if self.audio_file:
            log("[%d] audio written to %s" % (self.index, self.audio_path))


def discovery_loop(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", args.discovery_port))
    reply = b"VCAST!\n" + json.dumps({"name": args.name, "port": args.port, "version": 1}).encode("utf-8")
    log("discovery: listening on UDP %d" % args.discovery_port)
    while True:
        data, addr = s.recvfrom(2048)
        if data == QUERY:
            s.sendto(reply, addr)
            if args.verbose:
                log("discovery: answered %s:%d" % addr[:2])
        elif args.verbose:
            log("discovery: ignored %r from %s:%d" % (data[:32], *addr[:2]))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--name", default=socket.gethostname() + " (mock)", help="receiver name shown on the phone")
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help="TCP stream port (default 48690)")
    p.add_argument("--discovery-port", type=int, default=DISCOVERY_PORT, help="UDP discovery port (default 48689)")
    p.add_argument("--no-discovery", action="store_true", help="do not answer UDP discovery")
    p.add_argument("--bind", default="0.0.0.0", help="address to listen on")
    p.add_argument("--out", default="vcast-out", help="directory for the received streams")
    p.add_argument("--refuse", metavar="REASON", help="answer VCAST/1 ERR REASON to every sender")
    p.add_argument("--once", action="store_true", help="exit after the first session")
    p.add_argument("-v", "--verbose", action="store_true", help="log every frame")
    args = p.parse_args()

    if not args.no_discovery:
        threading.Thread(target=discovery_loop, args=(args,), daemon=True).start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(4)
    log("stream: listening on TCP %s:%d as %r, writing to %s/" % (args.bind, args.port, args.name, args.out))

    current, index = None, 0
    try:
        while True:
            conn, addr = srv.accept()
            index += 1
            if current is not None:
                log("[%d] replaced by a new sender" % current.index)
                current.close()
            current = Session(conn, addr, index, args)
            t = threading.Thread(target=current.run, daemon=True)
            t.start()
            if args.once:
                t.join()
                return 1 if current.errors else 0
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
