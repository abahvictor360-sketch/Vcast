#!/usr/bin/env python3
"""A fake DLNA MediaRenderer ("smart TV") for testing Vcast.

It answers SSDP searches, serves a device description, accepts AVTransport
SOAP calls, and on Play pulls the stream URL with ffmpeg into an output file,
just like a real TV would fetch it.

Usage: fake_renderer.py <output.ts> <log.txt>
"""
import html
import http.server
import re
import socket
import struct
import subprocess
import sys
import threading

HTTP_PORT = 49152
OUT, LOG = sys.argv[1], sys.argv[2]
log_lock = threading.Lock()
player = None


def log(msg):
    with log_lock:
        with open(LOG, "a") as f:
            f.write(msg + "\n")
        print("[fake-tv]", msg, flush=True)


DESCRIPTION = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>
    <friendlyName>Vcast Fake TV</friendlyName>
    <manufacturer>Test &amp; Co</manufacturer>
    <modelName>FakeTV 9000</modelName>
    <UDN>uuid:11111111-2222-3333-4444-555555555555</UDN>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>
        <controlURL>/rc/control</controlURL>
      </service>
      <service>
        <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>
        <controlURL>avt/control</controlURL>
      </service>
    </serviceList>
  </device>
</root>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/desc.xml":
            body = DESCRIPTION.encode()
            self.send_response(200)
            self.send_header("Content-Type", 'text/xml; charset="utf-8"')
            # Exercise the chunked decoder of the plugin's HTTP client.
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(body), 100):
                part = body[i:i + 100]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        global player
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode()
        action = self.headers.get("SOAPAction", "")
        log("SOAP %s %s" % (self.path, action))
        if self.path != "/avt/control":
            self.send_response(404)
            self.end_headers()
            return
        if action.endswith('#SetAVTransportURI"'):
            uri = html.unescape(re.search(r"<CurrentURI>(.*?)</CurrentURI>", body).group(1))
            meta = html.unescape(re.search(r"<CurrentURIMetaData>(.*?)</CurrentURIMetaData>", body).group(1))
            title = html.unescape(re.search(r"<dc:title>(.*?)</dc:title>", meta).group(1))
            log("URI %s" % uri)
            log("TITLE %s" % title)
            self.server.uri = uri
        elif action.endswith('#Play"'):
            # Behave like a TV: ask for DLNA content features, then play.
            player = subprocess.Popen(
                ["ffmpeg", "-v", "error", "-y", "-headers", "getcontentFeatures.dlna.org: 1\r\n",
                 "-i", self.server.uri, "-c", "copy", "-f", "mpegts", OUT],
                stdout=subprocess.DEVNULL, stderr=open(LOG + ".ffmpeg", "w"))
            log("PLAY")
        elif action.endswith('#Stop"'):
            log("STOP")
            if player:
                player.terminate()
        resp = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
                "<s:Body><u:Response/></s:Body></s:Envelope>").encode()
        self.send_response(200)
        self.send_header("Content-Type", 'text/xml; charset="utf-8"')
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)


def ssdp():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", 1900))
    try:
        mreq = struct.pack("4sl", socket.inet_aton("239.255.255.250"), socket.INADDR_ANY)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    except OSError as e:
        log("multicast join failed: %s" % e)
    while True:
        data, addr = sock.recvfrom(4096)
        text = data.decode(errors="replace")
        if not text.startswith("M-SEARCH"):
            continue
        st = re.search(r"(?im)^ST:\s*(.+?)\s*$", text).group(1)
        log("M-SEARCH from %s st=%s" % (addr[0], st))
        resp = ("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
                "LOCATION: http://127.0.0.1:%d/desc.xml\r\nSERVER: Fake/1.0 UPnP/1.0\r\n"
                "ST: %s\r\nUSN: uuid:11111111-2222-3333-4444-555555555555::%s\r\n\r\n"
                % (HTTP_PORT, st, st))
        sock.sendto(resp.encode(), addr)


threading.Thread(target=ssdp, daemon=True).start()
server = http.server.ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), Handler)
server.uri = None
log("ready")
server.serve_forever()
