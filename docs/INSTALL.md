# Installing Vcast in OBS Studio

Vcast works with **OBS Studio 32.2 or newer** (Help → About shows your version; update OBS if it is older) on Windows 10/11, macOS 13+ (Apple
Silicon and Intel) and Ubuntu 24.04.

## 1. Download

Open the [Actions page](https://github.com/abahvictor360-sketch/Vcast/actions)
of the Vcast repository and click the newest green (✅) run. Under
**Artifacts**, download the file for your computer:

| Your computer | Download | What's inside |
| --- | --- | --- |
| Windows | `vcast-…-windows-x64-…` | `vcast-…-windows-x64-Installer.exe` (installer) and a `.zip` |
| Mac | `vcast-…-macos-universal-…` | `vcast-…-macos-universal.pkg` (installer) |
| Ubuntu | `vcast-…-ubuntu-24.04-x86_64-…` | `.deb` package |

You need to be signed in to GitHub to download Actions artifacts. They are
`.zip` files, so unzip them first. Tagged releases also show the same files on
the [Releases page](https://github.com/abahvictor360-sketch/Vcast/releases).

## 2. Install

**Close OBS Studio first.**

### Windows

1. Double-click **`vcast-…-windows-x64-Installer.exe`**.
2. If Windows SmartScreen says *"Windows protected your PC"*, click
   **More info → Run anyway**. The installer isn't code-signed yet.
3. Click **Next / Install**. Vcast installs into
   `C:\ProgramData\obs-studio\plugins\vcast`. It also allows OBS through
   Windows Firewall on private networks, so phones can reach it.
4. Start OBS.

<details><summary>Manual install from the .zip instead</summary>

Unzip it and copy the `vcast` folder into
`C:\ProgramData\obs-studio\plugins\`. You should end up with
`C:\ProgramData\obs-studio\plugins\vcast\bin\64bit\vcast.dll`. The first time
a phone connects, Windows asks whether to allow OBS on the network. Tick
**Private networks** and click **Allow**.
</details>

### macOS

1. Double-click **`vcast-…-macos-universal.pkg`**.
2. If macOS says the package *"can't be opened because it is from an
   unidentified developer"*, close the message. Then **right-click the
   .pkg → Open → Open**. You can also go to **System Settings → Privacy &
   Security** and click **Open Anyway**.
3. Follow the installer. It puts `vcast.plugin` in
   `~/Library/Application Support/obs-studio/plugins/`.
4. Start OBS. If macOS asks whether OBS may *"find devices on your local
   network"* or *"accept incoming connections"*, click **Allow**. AirPlay and
   phone casting need both.

<details><summary>Manual install</summary>

Copy `vcast.plugin` to `~/Library/Application Support/obs-studio/plugins/`
(press ⌘⇧G in Finder and paste that path). If OBS doesn't load it, run
`xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/vcast.plugin`
in Terminal.
</details>

### Ubuntu / Linux

```sh
sudo apt install ./vcast-*-x86_64-linux-gnu.deb
```

## 3. Check that it loaded

In OBS, open **Docks → Vcast**. A **Vcast** panel appears. If it doesn't, go
to **Help → Log Files → View Current Log** and search for `vcast`.

## 4. Show a phone or laptop screen in OBS

In the Vcast dock, click **Add screen receiver to scene**. You can also use
**+** in *Sources* → **Vcast Screen Receiver**. The device must be on the
**same Wi‑Fi / network** as the OBS computer.

| Device | How to cast |
| --- | --- |
| **iPhone / iPad** | Open **Control Center** → **Screen Mirroring** → pick **Vcast (your computer name)** |
| **Mac** | Menu bar **Control Center** → **Screen Mirroring** → **Vcast (…)** |
| **Android** | Install the **Vcast** app (see below), open it, tap your OBS computer, then tap **Start now** |
| **Windows laptop, Chromebook, Linux, Mac** | In Chrome, Edge, Firefox or Safari open the address shown in the dock, e.g. `https://192.168.1.20:8443`, and press **Share screen** |

**Laptop browsers warn about the certificate.** Vcast uses a private
certificate made on your computer, so the browser says *"Your connection is
not private"*. Click **Advanced → Proceed to 192.168.x.x (unsafe)**. In
Firefox click *Accept the Risk and Continue*. Your browser only needs this
once.

**Android app.** On the phone, open `https://<OBS address>:8443/android` (the
same address as above plus `/android`) and download `vcast-android.apk`. When
Android asks, allow your browser to *install unknown apps*. The app finds OBS
automatically. If it doesn't, type the IP address shown in the dock.

Only one device shows at a time. When another device starts casting, it
replaces the current one, like a TV. To show two phones at once, add a second
*Vcast Screen Receiver* source and give it a different name in its
properties.

**Android's built-in Cast / Smart View (Windows only).** Samsung Smart View,
Android "Cast" and the Windows **Win + K** "Cast" button use Miracast, which
plugins can't receive. Windows has its own Miracast receiver, though:

1. Open **Settings → System → Projecting to this PC**. Install the optional
   **Wireless Display** feature if asked, and set it to *Available everywhere
   on secure networks*.
2. On the phone or laptop, press Cast / Smart View (Win + K on Windows) and
   pick the OBS computer. A *Connect* window opens on the PC.
3. In OBS add a **Window Capture** of that Connect window.

## 5. Show OBS on a TV

In the **Vcast** dock, the TVs on your network appear in the list. Select one
and press **Cast**. You can also press **Start link** and open the link in
VLC.

## Troubleshooting

- **iPhone doesn't list "Vcast (…)" in Screen Mirroring.**
  - The iPhone and the OBS computer must be on the same Wi‑Fi network
    (2.4 GHz vs 5 GHz of the same router is fine). Turn off any VPN on the
    phone and the computer.
  - Many routers have "AP/client isolation" or "guest mode" that hides
    devices from each other. Turn it off, or use the main network.
  - The *Vcast Screen Receiver* source must exist in OBS (any scene) with
    *Accept AirPlay* ticked. The name appears within a few seconds.
  - Windows: allow OBS through the firewall (the installer does this). If you
    use the **Steam** version of OBS, Windows asks the first time; click
    **Allow**.
  - Mac: allow OBS under **System Settings → Privacy & Security → Local
    Network**, and accept *incoming connections* if the firewall asks.
- **Phone doesn't see "Vcast" in Screen Mirroring.**
  - Check that both devices are on the same Wi‑Fi. Guest networks usually
    block this.
  - Make sure *Accept AirPlay* is ticked in the source's properties.
  - On Windows, make sure the network is set to **Private**, not Public.
- **Picture appears, then freezes.** Move closer to the router or use 5 GHz
  Wi‑Fi. On the laptop page, choose 720p.
- **Port already in use.** Vcast tries the next free ports automatically. The
  address in the dock always shows the current one.
- **Log file.** Go to **Help → Log Files → View Current Log** and look for
  lines starting with `[vcast]`.
