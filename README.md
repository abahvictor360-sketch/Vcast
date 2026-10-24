# Vcast — cast OBS to your TV over the local network

Vcast is an OBS Studio plugin that sends what OBS is showing to a **smart TV,
streaming box, or media player on the same Wi‑Fi / network**, the same way
a phone "casts" or "plays to" a TV.

- **Find TVs automatically.** Vcast uses UPnP/DLNA (SSDP) discovery, the same
  protocol phones use for "Play To" and "Cast to TV".
- **One-click cast.** Pick the TV and press **Cast**. Vcast tells the TV to
  play the live stream from OBS.
- **Open the link on any device.** Every stream also has a link
  (`http://<your-pc>:8090/stream.ts`) that opens in VLC, Kodi, mpv, or most TV
  and phone media players.
- **Its own encoder.** You can cast while you also stream or record. It
  works with x264 or your GPU encoder (NVENC, AMF, QuickSync, Apple VT).
- **No extra dependencies.** The MPEG‑TS muxer, HTTP server, and DLNA client
  are built in.

## How it works

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
sudo apt install cmake ninja-build pkg-config libobs-dev qt6-base-dev
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
tests/run-e2e.sh build_x86_64
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

## License

GPL-2.0-or-later, the same as OBS Studio.
