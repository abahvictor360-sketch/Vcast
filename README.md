# Vcast — cast screens into OBS, and OBS onto your TV

Vcast is an OBS Studio plugin for your local Wi‑Fi / network. It works in two
directions:

## ⬇️ Download

**[Get the latest release →](https://github.com/abahvictor360-sketch/Vcast/releases/latest)**

| Your computer | Download |
| --- | --- |
| Windows 10/11 | `vcast-…-windows-x64-Installer.exe` |
| macOS 13+ | `vcast-…-macos-universal.pkg` |
| Ubuntu 24.04 | `vcast-…-x86_64-linux-gnu.deb` |
| Android phone (sender app) | `vcast-android.apk` |

Requires OBS Studio 32.2 or newer. Then follow **[docs/INSTALL.md](docs/INSTALL.md)**.

**Receive: show a phone, tablet or laptop screen in OBS**

Add a **Vcast Screen Receiver** source. Devices then cast to OBS the way they
cast to a smart TV:

| Device | How it casts |
| --- | --- |
| iPhone, iPad, Mac | Built-in **Screen Mirroring** (AirPlay). Pick "Vcast (your PC)" |
| Android phone / tablet | The free **Vcast** Android app (in `android/`, served by OBS at `https://<pc>:8443/android`) |
| Windows laptop, Chromebook, Linux, Mac | Open `https://<pc>:8443` in Chrome / Edge / Firefox / Safari and press **Share screen** |
| Android "Cast/Smart View", Windows Win+K | Miracast. Use Windows' built-in *Wireless Display* receiver plus Window Capture (see [docs/INSTALL.md](docs/INSTALL.md)) |

Video and sound both come through. If a new device starts casting, it
replaces the current one, like a TV. Add more receiver sources to show
several devices at once.

**Send: show OBS on a smart TV**

The **Vcast** dock finds DLNA TVs on your network and casts the OBS output to
them. It can also give you a link that opens in VLC.

➡️ **Installing and using it: [docs/INSTALL.md](docs/INSTALL.md)**

## How sending to a TV works

```
 OBS canvas ──► H.264 + AAC encoders ──► Vcast output ──► MPEG-TS muxer
                                                              │
                         HTTP server (port 8090) ◄────────────┘
                            ▲                ▲
            TV pulls stream │                │ VLC / Kodi / phone
                            │
  Vcast ──SSDP search──► TV (DLNA MediaRenderer)
        ──SOAP SetAVTransportURI + Play──►
```

1. Vcast sends an SSDP `M-SEARCH` on every network interface. It reads each
   device description and keeps the devices that have an `AVTransport`
   service (TVs and renderers).
2. When you press **Cast**, Vcast starts its own OBS output and an HTTP
   server. It then sends the TV `SetAVTransportURI` with the stream URL,
   followed by `Play`.
3. The TV connects and pulls a live MPEG‑TS stream. Each new viewer starts at
   the next keyframe, which comes every second. A viewer that falls too far
   behind skips ahead instead of slowing the other viewers.

## Using it

1. Install the plugin (see below) and restart OBS.
2. Open the dock with **Docks → Vcast**.
3. Turn on the TV and make sure it is on the same network as the PC.
   Vcast searches automatically. You can also press **Search for TVs**.
4. Select your TV and press **Cast**. Many TVs take 2–5 seconds to buffer
   before the picture appears.
5. Press **Stop** to stop casting.

To watch on a device that is not a DLNA TV, press **Start link** and open the
link it shows in VLC or Kodi (*Open Network Stream*).

### Settings

| Setting | Default | Notes |
| --- | --- | --- |
| Video encoder | x264 | Use a GPU encoder to save CPU |
| Video bitrate | 6000 Kbps | Lower this if playback stutters on Wi‑Fi |
| Audio bitrate | 160 Kbps | AAC |
| Resolution | Same as OBS output | 1080p, 720p or 480p downscale (never upscales) |
| Network port | 8090 | If the port is busy, Vcast tries the next 9 ports |
| Title shown on TV | OBS Studio | Shown in the TV's player UI |

### Troubleshooting

- **No TVs found.**
  - Allow OBS through your firewall on private networks. On Windows, accept
    the firewall prompt.
  - Check that the TV's DLNA / "screen share" / "media renderer" feature is
    on.
  - Guest Wi‑Fi and "AP isolation" block devices from seeing each other.
  - You can also use **Add by IP...** with the TV's IP address.
- **The TV says the format is not supported.** Try 1080p or 720p and the x264
  encoder. Some older TVs can't handle 4K or very high bitrates.
- **Delay.** TVs buffer live streams, so expect 2–6 seconds of delay. Use
  the link in VLC or mpv for lower delay.

### Supported receivers

Vcast works with any **UPnP/DLNA MediaRenderer**. This includes most Samsung,
LG, Sony, Philips, Panasonic, Hisense and TCL smart TVs. It also works with
Android TV and Google TV when a DLNA receiver app is installed (for example
BubbleUPnP or Kodi), and with Kodi, Xbox and many AV receivers. Any player
that can open an HTTP MPEG‑TS URL works with the link.

Chromecast (Google Cast) and AirPlay use closed protocols and are not
supported yet. Android TVs with Kodi or a DLNA receiver work today.

## Building

Vcast uses the official
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) build
system, like [DistroAV](https://github.com/DistroAV/DistroAV). GitHub Actions
builds Windows, macOS and Ubuntu packages on every push.

### Ubuntu / Linux

```sh
sudo apt install cmake ninja-build pkg-config libobs-dev qt6-base-dev libavcodec-dev libavformat-dev libavutil-dev
cmake --preset ubuntu-x86_64
cmake --build build_x86_64
# Manual install for the current user:
mkdir -p ~/.config/obs-studio/plugins/vcast/bin/64bit ~/.config/obs-studio/plugins/vcast/data
cp build_x86_64/vcast.so ~/.config/obs-studio/plugins/vcast/bin/64bit/
cp -r data/* ~/.config/obs-studio/plugins/vcast/data/
```

### Windows / macOS

```sh
cmake --preset windows-x64   # or: cmake --preset macos
cmake --build --preset windows-x64
```

The presets download the matching OBS sources and dependencies. See the
[plugin template wiki](https://github.com/obsproject/obs-plugintemplate/wiki)
for details.

### End-to-end test (Linux)

The test runs libobs without a window, using the real x264 and AAC encoders.
A fake DLNA TV (`tests/fake_renderer.py`) answers the SSDP search and accepts
the cast. It then pulls the stream with ffmpeg the way a TV does, and the
script checks that the recording decodes without errors.

```sh
sudo apt install obs-studio ffmpeg xvfb
cmake --preset ubuntu-x86_64 -DVCAST_BUILD_TESTS=ON
cmake --build build_x86_64
tests/run-e2e.sh build_x86_64           # OBS -> fake TV
tests/run-receiver-e2e.sh build_x86_64  # fake Android app + real Chromium -> OBS (needs node + playwright)
```

## Source layout

| File | Purpose |
| --- | --- |
| `src/plugin-main.cpp` | Module entry point: registers the output and the dock |
| `src/vcast-output.*` | The `vcast_output` OBS output (encoded H.264 + AAC) |
| `src/ts-muxer.*` | Live MPEG‑TS muxer (PAT/PMT, PES, PCR, ADTS) |
| `src/http-server.*` | Multi-client live HTTP stream server |
| `src/dlna.*`, `src/http-client.*` | SSDP discovery and AVTransport control |
| `src/cast-manager.*` | Encoders, output lifecycle and current TV |
| `src/ui/*` | Qt dock and settings dialog |
| `src/receiver/*` | Screen receiver source, decoder, cast page server, Android app ingest |
| `data/web/index.html` | The laptop cast page |
| `android/` | Vcast Android sender app |

## How receiving works

```
iPhone/Mac ──AirPlay (mDNS + RTSP, FairPlay)──┐
Android app ──UDP discovery + TCP H.264/AAC───┼──► Vcast Screen Receiver ──► FFmpeg decode ──► OBS source
Laptop browser ──HTTPS page + WebSocket WebM──┘
```

- **AirPlay** comes from [UxPlay](https://github.com/FDH2/UxPlay) (GPL-3.0),
  vendored under `third_party/`. Its crypto is ported to mbedTLS and it uses a
  built-in mDNS responder, so Bonjour is not needed on Windows.
- The **laptop cast page** has to be served over HTTPS, because browsers only
  allow screen capture on secure pages. Vcast creates a private certificate on
  first use, stored in the OBS plugin config folder.
- The **Android app** protocol is documented in
  [docs/protocol.md](docs/protocol.md).

Ports used (TCP unless noted): 7100+ (AirPlay), 8443 (cast page), 48690
(Android app), UDP 48689 (app discovery), UDP 5353 (mDNS), 8090 (TV stream).

## License

GPL-3.0-or-later. The AirPlay support is based on UxPlay, which is GPL-3.0.
