#!/usr/bin/env python3
"""End-to-end test of the Vcast AirPlay receiver without an Apple device.

Starts tests/airplay/vcast-airplay-smoke, then acts as an AirPlay sender:
  1. mDNS: the _airplay._tcp and _raop._tcp services are found (python-zeroconf)
     with sane TXT records; two receivers with different names have different
     device ids.
  2. GET /info returns a binary plist with the expected keys.
  3. A full mirroring + audio session like iOS does it: FairPlay fp-setup
     (UxPlay's playfair code computes the AES key; the test asks the smoke
     binary for the same derivation), optionally legacy pair-setup/pair-verify
     (Ed25519 + X25519 with python-cryptography, i.e. an independent
     implementation), SETUP (timing, mirroring stream 110, AAC-ELD audio stream
     96), an encrypted H.264 mirroring stream over TCP and encrypted RTP audio
     over UDP, a volume change, TEARDOWN.
  The receiver's callbacks must deliver the original H.264 Annex-B units and
  audio frames, the audio format, the volume and connect/disconnect.

Usage: airplay-e2e.py <path to vcast-airplay-smoke>
Needs: pip install zeroconf cryptography
"""
import os
import plistlib
import socket
import struct
import subprocess
import sys
import threading
import time
import hashlib

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives import serialization

SMOKE = sys.argv[1] if len(sys.argv) > 1 else "build_x86_64/tests/airplay/vcast-airplay-smoke"
NTP_EPOCH = 2208988800

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


class Receiver:
    def __init__(self, name, port_base, extra=()):
        self.lines = []
        self.cv = threading.Condition()
        self.p = subprocess.Popen([SMOKE, "-n", name, "-p", str(port_base), *extra], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)
        threading.Thread(target=self._read, daemon=True).start()
        ready = self.wait(lambda l: l.startswith("READY") or l.startswith("ERROR"), 10)
        if not ready or ready.startswith("ERROR"):
            raise RuntimeError("receiver failed to start: %s" % ready)
        fields = dict(f.split("=", 1) for f in ready.split()[1:])
        self.port = int(fields["port"])
        self.device_id = fields["deviceid"]

    def _read(self):
        for line in self.p.stdout:
            with self.cv:
                self.lines.append(line.rstrip("\n"))
                self.cv.notify_all()

    def wait(self, pred, timeout):
        end = time.time() + timeout
        with self.cv:
            i = 0
            while True:
                while i < len(self.lines):
                    if pred(self.lines[i]):
                        return self.lines[i]
                    i += 1
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def matching(self, prefix):
        with self.cv:
            return [l for l in self.lines if l.startswith(prefix)]

    def stop(self):
        self.p.stdin.close()
        self.p.wait(15)
        return self.wait(lambda l: l.startswith("DESTROYED"), 1)


class Rtsp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.cseq = 0
        self.buf = b""

    def request(self, method, url, body=b"", ctype=None, headers=None):
        self.cseq += 1
        h = ["%s %s RTSP/1.0" % (method, url), "CSeq: %d" % self.cseq, "User-Agent: AirPlay/690.7.1",
             "X-Apple-ProtocolVersion: 1"]
        if body:
            h.append("Content-Length: %d" % len(body))
        if ctype:
            h.append("Content-Type: " + ctype)
        for k, v in (headers or {}).items():
            h.append("%s: %s" % (k, v))
        self.s.sendall(("\r\n".join(h) + "\r\n\r\n").encode() + body)
        while b"\r\n\r\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise RuntimeError("connection closed")
            self.buf += d
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode().split("\r\n")
        status = int(lines[0].split()[1])
        hdrs = {}
        for l in lines[1:]:
            k, v = l.split(":", 1)
            hdrs[k.strip().lower()] = v.strip()
        n = int(hdrs.get("content-length", "0"))
        while len(self.buf) < n:
            self.buf += self.s.recv(65536)
        data, self.buf = self.buf[:n], self.buf[n:]
        return status, hdrs, data

    def plist(self, method, url, obj):
        st, h, data = self.request(method, url, plistlib.dumps(obj, fmt=plistlib.FMT_BINARY),
                                   "application/x-apple-binary-plist")
        return st, (plistlib.loads(data) if data else None)


def q32(t):
    return int(t * (1 << 32)) & 0xFFFFFFFFFFFFFFFF


def ntp_server(sock, stop):
    """Answer the receiver's NTP-style timing requests."""
    sock.settimeout(0.2)
    while not stop.is_set():
        try:
            req, addr = sock.recvfrom(128)
        except socket.timeout:
            continue
        recv = q32(time.time() + NTP_EPOCH)
        resp = bytearray(32)
        resp[0], resp[1] = 0x80, 0xd3
        resp[2:4] = req[2:4]
        resp[8:16] = req[24:32]
        resp[16:24] = struct.pack(">Q", recv)
        resp[24:32] = struct.pack(">Q", q32(time.time() + NTP_EPOCH))
        sock.sendto(bytes(resp), addr)


def check_mdns(rx_name, port, device_id, other_device_id):
    try:
        from zeroconf import Zeroconf, ServiceBrowser
    except ImportError:
        check(False, "python zeroconf installed")
        return
    zc = Zeroconf()
    found = {}

    class L:
        def add_service(self, z, t, n):
            found[n] = t

        def update_service(self, z, t, n):
            found[n] = t

        def remove_service(self, z, t, n):
            pass

    ServiceBrowser(zc, ["_airplay._tcp.local.", "_raop._tcp.local."], L())
    end = time.time() + 8
    ap_name = "%s._airplay._tcp.local." % rx_name
    raop_name = "%s@%s._raop._tcp.local." % (device_id.replace(":", "").upper(), rx_name)
    while time.time() < end and not (ap_name in found and raop_name in found):
        time.sleep(0.2)
    check(ap_name in found, "mDNS: %s browsed" % ap_name)
    check(raop_name in found, "mDNS: %s browsed" % raop_name)
    info = zc.get_service_info("_airplay._tcp.local.", ap_name, 3000)
    if info:
        txt = {k.decode(): (v or b"").decode() for k, v in info.properties.items()}
        print("     airplay TXT:", txt, "port", info.port, "addresses", info.parsed_addresses(), "server", info.server)
        check(info.port == port, "mDNS: _airplay port %d" % port)
        check(txt.get("deviceid") == device_id, "mDNS: deviceid matches")
        check(txt.get("deviceid") != other_device_id, "mDNS: different names give different device ids")
        feats = int(txt.get("features", "0x0,0x0").split(",")[0], 16)
        check(feats & (1 << 7) != 0, "mDNS: features has mirroring (bit 7)")
        check(feats & (1 << 27) == 0, "mDNS: legacy pairing off by default (bit 27)")
        check(len(txt.get("pk", "")) == 64, "mDNS: pk is a 32-byte hex key")
        check(txt.get("model") == "AppleTV3,2", "mDNS: model")
        check(len(info.parsed_addresses()) > 0, "mDNS: host address resolved")
    else:
        check(False, "mDNS: _airplay service info resolved")
    info = zc.get_service_info("_raop._tcp.local.", raop_name, 3000)
    if info:
        txt = {k.decode(): (v or b"").decode() for k, v in info.properties.items()}
        print("     raop TXT:", txt)
        check(info.port == port and txt.get("cn") == "0,1,2,3" and txt.get("et") == "0,3,5" and "pk" in txt,
              "mDNS: _raop TXT sane")
    else:
        check(False, "mDNS: _raop service info resolved")
    zc.close()


def check_info(port, device_id):
    c = Rtsp(port)
    st, h, data = c.request("GET", "/info")
    check(st == 200 and h.get("content-type") == "application/x-apple-binary-plist" and data.startswith(b"bplist00"),
          "GET /info returns a binary plist")
    info = plistlib.loads(data)
    print("     /info keys:", sorted(info.keys()))
    for k in ("deviceID", "features", "name", "pk", "model", "sourceVersion", "displays", "audioFormats",
              "audioLatencies", "initialVolume", "macAddress", "statusFlags"):
        check(k in info, "/info has %s" % k)
    check(info.get("deviceID") == device_id, "/info deviceID matches mDNS")
    d = info.get("displays", [{}])[0]
    check(d.get("width") == 1920 and d.get("height") == 1080 and d.get("maxFPS") == 60, "/info display 1920x1080@60")
    c.s.close()


def aes_ctr(key, iv):
    return Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()


def session(rx, use_pairing, teardown=True):
    tag = " (pairing)" if use_pairing else ""
    c = Rtsp(rx.port)
    ecdh_secret = None
    if use_pairing:
        # legacy pairing as iOS does it when features bit 27 is set
        ced = ed25519.Ed25519PrivateKey.generate()
        ced_pub = ced.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        st, h, server_ed = c.request("POST", "/pair-setup", ced_pub, "application/octet-stream")
        check(st == 200 and len(server_ed) == 32, "pair-setup" + tag)
        cx = x25519.X25519PrivateKey.generate()
        cx_pub = cx.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        st, h, data = c.request("POST", "/pair-verify", b"\x01\x00\x00\x00" + cx_pub + ced_pub,
                                "application/octet-stream")
        check(st == 200 and len(data) == 96, "pair-verify 1" + tag)
        sx_pub, enc_sig = data[:32], data[32:]
        ecdh_secret = cx.exchange(x25519.X25519PublicKey.from_public_bytes(sx_pub))
        key = hashlib.sha512(b"Pair-Verify-AES-Key" + ecdh_secret).digest()[:16]
        iv = hashlib.sha512(b"Pair-Verify-AES-IV" + ecdh_secret).digest()[:16]
        ctr = aes_ctr(key, iv)
        sig = ctr.update(enc_sig)
        try:
            ed25519.Ed25519PublicKey.from_public_bytes(server_ed).verify(sig, sx_pub + cx_pub)
            ok = True
        except Exception:
            ok = False
        check(ok, "server pair-verify signature valid (python-cryptography)" + tag)
        my_sig = ctr.update(ced.sign(cx_pub + sx_pub))
        st, h, data = c.request("POST", "/pair-verify", b"\x00\x00\x00\x00" + my_sig, "application/octet-stream")
        check(st == 200, "pair-verify 2 accepted" + tag)

    # FairPlay
    st, h, r1 = c.request("POST", "/fp-setup", bytes.fromhex("46504c590301010000000004020000bb"),
                          "application/octet-stream")
    check(st == 200 and len(r1) == 142, "fp-setup 1" + tag)
    # byte 12 is the FairPlay mode (0..3), as in a real client's message
    keymsg = bytes.fromhex("46504c59030103000000009800") + os.urandom(151)
    st, h, r2 = c.request("POST", "/fp-setup", keymsg, "application/octet-stream")
    check(st == 200 and len(r2) == 32 and r2[12:] == keymsg[144:], "fp-setup 2" + tag)
    ekey = bytes.fromhex("46504c59010201000000003c00000000") + os.urandom(56)
    eiv = os.urandom(16)
    out = subprocess.run([SMOKE, "--fpdecrypt", keymsg.hex(), ekey.hex()], capture_output=True, text=True)
    aeskey = bytes.fromhex(out.stdout.strip())
    if ecdh_secret:
        aeskey = hashlib.sha512(aeskey + ecdh_secret).digest()[:16]

    # timing
    tsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tsock.bind(("127.0.0.1", 0))
    stop = threading.Event()
    threading.Thread(target=ntp_server, args=(tsock, stop), daemon=True).start()

    st, r = c.plist("SETUP", "rtsp://127.0.0.1/1234", {
        "ekey": ekey, "eiv": eiv, "deviceID": "11:22:33:44:55:66", "name": "E2E iPhone", "model": "iPhone15,2",
        "timingProtocol": "NTP", "timingPort": tsock.getsockname()[1], "isScreenMirroringSession": True,
        "sessionUUID": "F7A4C1B2-0000-4000-8000-000000000001", "macAddress": "11:22:33:44:55:66"})
    check(st == 200 and r and r.get("timingPort"), "SETUP 1 (keys, timing)" + tag)
    check(rx.wait(lambda l: l.startswith("CONNECT E2E iPhone"), 3) is not None, "on_connect with device name" + tag)

    stream_id = 0x1234567890abcdef
    st, r = c.plist("SETUP", "rtsp://127.0.0.1/1234", {"streams": [{"type": 110, "streamConnectionID": stream_id}]})
    vport = r["streams"][0]["dataPort"] if r else 0
    check(st == 200 and vport, "SETUP mirroring stream" + tag)

    csock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    csock.bind(("127.0.0.1", 0))
    st, r = c.plist("SETUP", "rtsp://127.0.0.1/1234", {"streams": [{
        "type": 96, "ct": 8, "spf": 480, "audioFormat": 0x1000000, "controlPort": csock.getsockname()[1],
        "usingScreen": True, "isMedia": False}]})
    adport = r["streams"][0]["dataPort"] if r else 0
    acport = r["streams"][0]["controlPort"] if r else 0
    check(st == 200 and adport and acport, "SETUP audio stream (AAC-ELD)" + tag)
    st, h, _ = c.request("RECORD", "rtsp://127.0.0.1/1234")
    check(st == 200, "RECORD" + tag)
    st, h, _ = c.request("SET_PARAMETER", "rtsp://127.0.0.1/1234", b"volume: -11.500000\r\n", "text/parameters")
    check(st == 200 and rx.wait(lambda l: l.startswith("VOLUME -11.50"), 3) is not None, "on_volume" + tag)

    # --- mirroring stream ---
    vkey = hashlib.sha512(b"AirPlayStreamKey%d" % stream_id + aeskey).digest()[:16]
    viv = hashlib.sha512(b"AirPlayStreamIV%d" % stream_id + aeskey).digest()[:16]
    vctr = aes_ctr(vkey, viv)
    m = socket.create_connection(("127.0.0.1", vport))
    sps = bytes.fromhex("6764001facd9405005bb011000000300100000030320f18319a0")
    pps = bytes.fromhex("68ebe3cb22c0")
    now = time.time()

    def header(size, ptype, opt, ts, extra=b""):
        h = bytearray(128)
        struct.pack_into("<I", h, 0, size)
        h[4], h[6], h[7] = ptype, opt & 0xff, opt >> 8
        struct.pack_into("<Q", h, 8, q32(ts))
        h[16:16 + len(extra)] = extra
        return bytes(h)

    geom = struct.pack("<ff", 1280.0, 720.0) + bytes(16) + struct.pack("<ffffff", 1280.0, 720.0, 0, 0, 1280.0, 720.0)
    avcc = bytes([1, 0x64, 0, 0x1f, 0xff, 0xe1]) + struct.pack(">H", len(sps)) + sps + bytes([1]) + \
        struct.pack(">H", len(pps)) + pps
    frames = []
    for i in range(6):
        ts = now + i / 60.0
        if i == 0:
            m.sendall(header(len(avcc), 0x01, 0x0116, ts, geom) + avcc)
        nal = (bytes([0x65, 0x88, 0x84]) if i == 0 else bytes([0x41, 0x9a, 0x02])) + os.urandom(37 + 101 * i)
        payload = struct.pack(">I", len(nal)) + nal
        m.sendall(header(len(payload), 0x00, 0x1000 if i == 0 else 0, ts) + vctr.update(payload))
        frames.append(nal)
    expect0 = b"\0\0\0\1" + sps + b"\0\0\0\1" + pps + b"\0\0\0\1" + frames[0]
    vlines = []
    end = time.time() + 5
    while time.time() < end and len(vlines) < 6:
        vlines = rx.matching("VIDEO ")
        time.sleep(0.05)
    check(len(vlines) == 6, "6 video units delivered (got %d)%s" % (len(vlines), tag))
    if vlines:
        f = dict(x.split("=", 1) for x in vlines[0].split()[1:])
        check(f["codec"] == "1" and f["key"] == "1" and int(f["size"]) == len(expect0) and
              f["head"] == expect0[:48].hex(), "first unit = SPS+PPS+IDR in Annex-B, keyframe" + tag)
        ts, nowr = int(f["ts"]), int(f["now"])
        check(ts != 0 and abs(nowr - ts) < 2_000_000_000, "video timestamp on the monotonic clock" + tag)
    if len(vlines) == 6:
        ok = True
        prev = 0
        for i, l in enumerate(vlines[1:], 1):
            f = dict(x.split("=", 1) for x in l.split()[1:])
            exp = b"\0\0\0\1" + frames[i]
            ok &= f["key"] == "0" and int(f["size"]) == len(exp) and f["head"] == exp[:48].hex()
            ok &= int(f["ts"]) > prev
            prev = int(f["ts"])
        check(ok, "later units decrypted correctly across odd sizes (CTR continuity), increasing ts" + tag)

    # --- audio: sync on the control port, then encrypted RTP ---
    rtp0 = 123456
    sync = bytes([0x90, 0xd4, 0x00, 0x04]) + struct.pack(">IQI", rtp0, q32(time.time() + NTP_EPOCH), rtp0 + 7497)
    csock.sendto(sync, ("127.0.0.1", acport))
    asock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    aframes = []
    for i in range(12):
        frame = bytes([0x8c]) + os.urandom(150 + i)
        n = len(frame) // 16 * 16
        enc = Cipher(algorithms.AES(aeskey), modes.CBC(eiv)).encryptor().update(frame[:n]) + frame[n:]
        pkt = struct.pack(">BBHII", 0x80, 0x60, 1000 + i, rtp0 + 480 * i, 0x11223344) + enc
        asock.sendto(pkt, ("127.0.0.1", adport))
        aframes.append(frame)
        time.sleep(0.005)
    fmt = rx.wait(lambda l: l.startswith("AUDIOFMT"), 5)
    check(fmt is not None and "codec=1" in fmt and "rate=44100" in fmt and "config=f8e85000" in fmt,
          "on_audio_format AAC-ELD f8e85000" + tag)
    end = time.time() + 5
    alines = []
    while time.time() < end and len(alines) < 11:
        alines = rx.matching("AUDIO ")
        time.sleep(0.05)
    check(len(alines) >= 11, "audio frames delivered (%d of 12)%s" % (len(alines), tag))
    if alines:
        ok = True
        for l in alines:
            f = dict(x.split("=", 1) for x in l.split()[1:])
            size = int(f["size"])
            match = [a for a in aframes if len(a) == size]
            ok &= bool(match) and f["head"] == match[0][:16].hex() and int(f["ts"]) != 0
        check(ok, "audio frames decrypted (AES-CBC) correctly" + tag)
        fmt_idx = rx.lines.index(fmt) if fmt in rx.lines else -1
        first_audio = rx.lines.index(alines[0])
        check(0 <= fmt_idx < first_audio, "on_audio_format before the first frame" + tag)

    # --- teardown ---
    if not teardown:
        # keep everything open and streaming: the caller destroys the receiver
        threading.Thread(target=lambda: [m.sendall(header(8, 0x00, 0, time.time()) + vctr.update(
            struct.pack(">I", 4) + b"\x41abc")) or time.sleep(0.02) for _ in range(100)], daemon=True).start()
        return c, m, stop
    c.plist("TEARDOWN", "rtsp://127.0.0.1/1234", {"streams": [{"type": 110}]})
    c.plist("TEARDOWN", "rtsp://127.0.0.1/1234", {"streams": [{"type": 96}]})
    c.plist("TEARDOWN", "rtsp://127.0.0.1/1234", {})
    m.close()
    c.s.close()
    check(rx.wait(lambda l: l.startswith("DISCONNECT"), 5) is not None, "on_disconnect" + tag)
    stop.set()


def main():
    rx2 = Receiver("Vcast Test 2", 47100)
    rx = Receiver("Vcast Test", 47000, ["-v"] if os.environ.get("E2E_VERBOSE") else [])
    print("receiver port", rx.port, "device id", rx.device_id, "| second:", rx2.device_id)
    check(rx.port == 47000, "RTSP port = port_base")
    check(rx.device_id != rx2.device_id, "device ids differ between names")
    rx3 = Receiver("Vcast Test", 47200)
    check(rx3.device_id == rx.device_id, "device id is stable for a name")
    rx3.stop()

    check_mdns("Vcast Test", rx.port, rx.device_id, rx2.device_id)
    check_info(rx.port, rx.device_id)
    try:
        session(rx, False)
    except Exception as e:
        check(False, "session raised %r" % e)
        if os.environ.get("E2E_VERBOSE"):
            print("\n".join(rx.lines[-60:]))
    print("destroy:", rx.stop())
    rx2.stop()

    rxp = Receiver("Vcast Pairing", 47300, ["-pairing"])
    keep = session(rxp, True, teardown=False)
    # destroy while the client is connected and streaming
    time.sleep(0.5)
    d = rxp.stop()
    keep[2].set()
    print("destroy with a client connected:", d)
    check(d is not None and rxp.p.returncode == 0, "destroy while a client is connected")

    print("\n%d failures" % len(failures))
    for f in failures:
        print("  FAILED:", f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
