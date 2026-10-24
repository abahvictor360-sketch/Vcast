#!/usr/bin/env python3
"""Pretends to be the Vcast Android app: discovers the receiver over UDP and
streams H.264 + AAC (made with ffmpeg) using the protocol in docs/protocol.md.

Usage: fake_android_sender.py <workdir> <seconds>
"""
import json
import socket
import struct
import subprocess
import sys
import time

work, seconds = sys.argv[1], int(sys.argv[2])
fps = 30

# Test media: annex-B H.264 with access unit delimiters, ADTS AAC.
subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", f"testsrc2=size=1080x2340:rate={fps}",
                "-t", str(seconds), "-c:v", "libx264", "-preset", "ultrafast", "-bf", "0", "-g", "30",
                "-x264-params", "aud=1:repeat-headers=1", "-pix_fmt", "yuv420p", "-f", "h264",
                f"{work}/phone.h264"], check=True)
subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                "-ac", "2", "-t", str(seconds), "-c:a", "aac", "-b:a", "128k", "-f", "adts", f"{work}/phone.aac"],
               check=True)

# Split H.264 into access units at the AUD NALs (00 00 00 01 09).
data = open(f"{work}/phone.h264", "rb").read()
starts = []
i = 0
while True:
    i = data.find(b"\x00\x00\x00\x01\x09", i)
    if i < 0:
        break
    starts.append(i)
    i += 5
units = [data[a:b] for a, b in zip(starts, starts[1:] + [len(data)])]

def nal_types(au):
    out, j = [], 0
    while True:
        j = au.find(b"\x00\x00\x01", j)
        if j < 0:
            return out
        out.append(au[j + 3] & 0x1F)
        j += 3

def param_sets(au):
    # SPS + PPS NALs with start codes, for VIDEO_CONFIG
    out, j, idx = b"", 0, []
    while True:
        j = au.find(b"\x00\x00\x00\x01", j)
        if j < 0:
            break
        idx.append(j)
        j += 4
    for a, b in zip(idx, idx[1:] + [len(au)]):
        if au[a + 4] & 0x1F in (7, 8):
            out += au[a:b]
    return out

# Split ADTS into raw AAC frames and build the AudioSpecificConfig.
aac = open(f"{work}/phone.aac", "rb").read()
frames, k = [], 0
asc = None
while k + 7 <= len(aac):
    hdr = aac[k:k + 7]
    flen = ((hdr[3] & 3) << 11) | (hdr[4] << 3) | (hdr[5] >> 5)
    hlen = 7 if hdr[1] & 1 else 9
    if asc is None:
        obj = (hdr[2] >> 6) + 1
        freq = (hdr[2] >> 2) & 0xF
        ch = ((hdr[2] & 1) << 2) | (hdr[3] >> 6)
        asc = bytes([(obj << 3) | (freq >> 1), ((freq & 1) << 7) | (ch << 3)])
    frames.append(aac[k + hlen:k + flen])
    k += flen

# Discovery
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
udp.settimeout(1.0)
found = None
for attempt in range(5):
    udp.sendto(b"VCAST?\n", ("255.255.255.255", 48689))
    udp.sendto(b"VCAST?\n", ("127.0.0.1", 48689))
    try:
        reply, addr = udp.recvfrom(2048)
    except socket.timeout:
        continue
    assert reply.startswith(b"VCAST!\n"), reply
    info = json.loads(reply[7:])
    found = (addr[0], info["port"], info["name"])
    break
assert found, "receiver did not answer discovery"
print("DISCOVERED", found, flush=True)

sock = socket.create_connection((found[0], found[1]))
sock.sendall(b"VCAST/1\n")
line = b""
while not line.endswith(b"\n"):
    line += sock.recv(1)
print("HANDSHAKE", line.decode().strip(), flush=True)
assert line.startswith(b"VCAST/1 OK"), line

def frame(t, payload=b"", pts=0):
    sock.sendall(struct.pack(">BIq", t, len(payload), pts) + payload)

frame(0x01, json.dumps({"device": "Fake Pixel", "os": "Android 15", "app": "test"}).encode())
frame(0x10, b"\x01" + param_sets(units[0]))
frame(0x20, b"\x01" + struct.pack(">I", 48000) + b"\x02" + asc)

start = time.monotonic()
ai = 0
for n, au in enumerate(units):
    pts = int(n * 1e6 / fps)
    key = 5 in nal_types(au)
    frame(0x12 if key else 0x11, au, pts)
    # audio up to this video time (1024 samples per AAC frame)
    while ai < len(frames) and ai * 1024 * 1e6 / 48000 <= pts:
        frame(0x21, frames[ai], int(ai * 1024 * 1e6 / 48000))
        ai += 1
    delay = start + (n + 1) / fps - time.monotonic()
    if delay > 0:
        time.sleep(delay)
frame(0x31)
sock.close()
print("SENT", len(units), "video frames,", ai, "audio frames", flush=True)
