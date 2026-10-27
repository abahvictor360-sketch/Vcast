# Installing Vcast in OBS Studio

Vcast works with **OBS Studio 32.2 or newer** (Help → About shows your version; update OBS if it is older) on Windows 10/11, macOS 13+ (Apple
Silicon and Intel) and Ubuntu 24.04.

## 1. Download

Get the files from the [website](https://abahvictor360-sketch.github.io/Vcast/)
or the [latest release](https://github.com/abahvictor360-sketch/Vcast/releases/latest):

| Your device | File |
| --- | --- |
| Windows 10/11 | `vcast-…-windows-x64-Installer.exe` (or the `.zip` for a manual install) |
| Mac (macOS 13+, Apple Silicon and Intel) | `vcast-…-macos-universal.pkg` |
| Ubuntu 24.04 | `vcast-…-x86_64-linux-gnu.deb` |
| Android phone or tablet | `vcast-android.apk` (see [Android app](#android-app) below) |

iPhones, iPads and Macs need nothing extra: they cast with their built-in
Screen Mirroring.

### What you need

| Device | Requirement |
| --- | --- |
| Computer running OBS | OBS Studio 32.2 or newer, on the same Wi‑Fi / network as the phone |
| iPhone / iPad | Any model with **Screen Mirroring** in Control Center |
| Mac (as the device being cast) | Screen Mirroring in the menu bar's Control Center |
| Android | Android 8.0 or newer. Phone **sound** needs Android 10 or newer |
| Laptop / Chromebook | Chrome, Edge, Firefox or Safari |

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
| **Android** | Install the **Vcast** app (see below), open it, tap your OBS computer, then **Cast screen → Start now** |
| **Windows laptop, Chromebook, Linux, Mac** | In Chrome, Edge, Firefox or Safari open the address shown in the dock, e.g. `https://192.168.1.20:8443`, and press **Share screen** |

**Laptop browsers warn about the certificate.** Vcast uses a private
certificate made on your computer, so the browser says *"Your connection is
not private"*. Click **Advanced → Proceed to 192.168.x.x (unsafe)**. In
Firefox click *Accept the Risk and Continue*. Your browser only needs this
once.

### iPhone, iPad and Mac

1. Open **Control Center**. On iPhone X and newer, swipe down from the
   top-right corner; on a Mac, click the Control Center icon in the menu bar.
2. Tap **Screen Mirroring** and choose **Vcast (your computer name)**.
3. The screen appears in OBS within a few seconds, with the phone's sound.
   Turn the phone sideways for a landscape picture; Vcast refits it
   automatically.
4. To stop, open Control Center → **Screen Mirroring** → **Stop Mirroring**.

If the phone asks for an AirPlay code, it is looking at a different device:
pick the entry that starts with **Vcast**.

### Android app

**Install it (once).** The app isn't in the Play Store, so Android asks a
couple of extra questions:

1. Download `vcast-android.apk` on the phone. You can scan the QR code on the
   [website](https://abahvictor360-sketch.github.io/Vcast/#download), use the
   [latest release](https://github.com/abahvictor360-sketch/Vcast/releases/latest),
   or open `https://<OBS address>:8443/android` (the address in the Vcast
   dock plus `/android`; accept the certificate warning).
2. Open the downloaded file. When Android says your browser *isn't allowed
   to install unknown apps*, tap **Settings**, turn on **Allow from this
   source**, and go back.
3. If **Google Play Protect** warns about an unknown app, tap **More details →
   Install anyway**. It warns about every app that isn't from the Play Store.

**Cast.**

1. Open **Vcast**. Your OBS computer appears under *OBS receivers on this
   network* within a few seconds. If it doesn't, tap **Connect by address**
   and type the IP shown in the Vcast dock, e.g. `192.168.1.20`.
2. Optional: pick **720p** or **1080p**, and tick **Share phone audio** to send
   the phone's sound (Android 10+). Allow the audio permission when asked;
   Vcast only records what apps play, never the microphone.
3. Tap your computer, then **Cast screen**. Android 14 and newer ask whether
   to share **one app** or the **entire screen**: choose **Entire screen**. Tap
   **Start now**.
4. To stop, tap **Stop** in the app or in the Vcast notification.

**One-tap casting.** Add the **Cast to OBS** tile to Quick Settings (swipe down
twice, tap the pencil/edit icon, drag the tile into place, or use **Add tile to
Quick Settings** in the app). Tapping it casts to the OBS you used last; tap
it again to stop.

### Apps that show black or have no sound

Netflix, Disney+, Prime Video, banking apps and other protected apps block
screen recording on iPhone and Android, so they appear black in OBS. On
Android some apps also refuse to share their sound. That is the app's own
setting and no casting tool can get around it. YouTube, games, photos,
browsers and most other apps work.

### More than one device

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
- **The Android app doesn't find OBS.**
  - OBS must be open with a *Vcast Screen Receiver* source that has *Accept
    the Vcast Android app* ticked.
  - Same Wi‑Fi, not a guest network, no VPN. Then try **Connect by address**
    with the IP from the Vcast dock.
  - Windows: the network must be **Private**. Other firewalls must allow UDP
    48689 (finding OBS) and TCP 48690 (the picture).
- **Android: picture but no sound.** Tick *Share phone audio* before casting
  and allow the audio permission (Settings → Apps → Vcast → Permissions if you
  refused it). It needs Android 10+, and some apps block it (see above).
- **Android: casting stops when the screen turns off or after a while.**
  Some phones (Samsung, Xiaomi, Huawei, OnePlus) close background apps to save
  battery. Go to Settings → Apps → Vcast → Battery and choose **Unrestricted**
  (or turn off battery optimisation for Vcast).
- **Picture appears, then freezes.** Move closer to the router or use 5 GHz
  Wi‑Fi. On the laptop page, choose 720p.
- **Ports used** (allow these in any third-party firewall): TCP 7100 and up
  plus UDP 5353 (AirPlay and its discovery), UDP 48689 and TCP 48690 (Android
  app), TCP 8443 (laptop cast page), TCP 8090 (TV stream).
- **Port already in use.** Vcast tries the next free ports automatically. The
  address in the dock always shows the current one.
- **Log file.** Go to **Help → Log Files → View Current Log** and look for
  lines starting with `[vcast]`.
