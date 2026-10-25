# Vcast sender protocol (version 1)

The Vcast Android app (and any other native sender) uses this protocol to send
a live screen to a "Vcast Receiver" source in OBS. All integers are big-endian.

## Discovery (UDP)

1. The sender broadcasts the 7 bytes `VCAST?\n` to UDP port **48689**
   (to `255.255.255.255` and to the directed broadcast address of its Wi-Fi
   subnet, e.g. `192.168.1.255`).
2. Each receiver in every OBS instance on the network answers the sender's
   address and port with one datagram per receiver source:

   ```
   VCAST!\n{"name":"Living room OBS","port":48690,"version":1}
   ```

   `port` is the TCP port to connect to. The answer's source IP is the address.

Senders should also let the user type `ip` or `ip:port` (default port 48690).

## Stream (TCP)

1. The sender connects and writes the 8 bytes `VCAST/1\n`.
2. The receiver replies with one line: `VCAST/1 OK <receiver name>\n`, or
   `VCAST/1 ERR <reason>\n` followed by closing the connection.
3. The sender then writes frames. Each frame is a 13-byte header followed by
   `length` payload bytes:

   | Offset | Size | Field |
   | --- | --- | --- |
   | 0 | 1 | type |
   | 1 | 4 | length (payload bytes, max 16 MiB) |
   | 5 | 8 | pts in microseconds (sender clock, signed, monotonic per stream) |

| Type | Name | Payload |
| --- | --- | --- |
| `0x01` | HELLO | UTF-8 JSON, e.g. `{"device":"Pixel 8","os":"Android 15","app":"1.0"}` |
| `0x10` | VIDEO_CONFIG | 1 byte codec (`1` = H.264, `2` = H.265), then the Annex-B parameter sets (SPS+PPS, plus VPS for H.265) |
| `0x11` | VIDEO_FRAME | One Annex-B access unit (start codes included) |
| `0x12` | VIDEO_KEYFRAME | Like VIDEO_FRAME, for an IDR frame |
| `0x20` | AUDIO_CONFIG | 1 byte codec (`1` = AAC), 4 bytes sample rate, 1 byte channels, then the AudioSpecificConfig (Android `csd-0`) |
| `0x21` | AUDIO_FRAME | One raw AAC access unit (no ADTS header) |
| `0x30` | PING | Empty; send at least every 2 seconds when nothing else is sent |
| `0x31` | BYE | Empty; the sender is stopping |

Rules:

- A VIDEO_CONFIG must come before the first video frame and again whenever the
  encoder is reconfigured (rotation, resolution change), followed by a keyframe.
- The receiver drops the connection after 10 seconds without any frame.
- The receiver accepts one sender at a time per receiver source; a new sender
  replaces the current one.
