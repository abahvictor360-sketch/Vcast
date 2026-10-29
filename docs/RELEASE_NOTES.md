## Download

| Your computer | File |
| --- | --- |
| **Windows 10/11** | `vcast-…-windows-x64-Installer.exe` — double-click to install (or the `.zip` for a manual install) |
| **macOS 13+ (Apple Silicon & Intel)** | `vcast-…-macos-universal.pkg` |
| **Ubuntu 24.04** | `vcast-…-x86_64-linux-gnu.deb` |
| **Android phone** | `vcast-android.apk` — the Vcast app for casting Android screens into OBS |

Requires **OBS Studio 32.2 or newer**. Close OBS before installing.
Step-by-step instructions: [docs/INSTALL.md](https://github.com/abahvictor360-sketch/Vcast/blob/HEAD/docs/INSTALL.md)

### Quick start
1. Install, open OBS, open **Docks → Vcast**, click **Add screen receiver to scene**.
2. **iPhone / iPad / Mac:** Control Center → **Screen Mirroring** → *Vcast (your PC)*.
3. **Android:** install the Vcast app, tap **Cast to OBS** (or add the *Cast to OBS* quick-settings tile).
4. **Laptop:** open the address shown in the dock in Chrome/Edge and press **Share screen**.
5. **Windows laptop with Win + K, Android Cast / Smart View (OBS on Windows):** click **Add Win+K receiver to scene** in the dock, then press **Win + K** on the laptop and pick the OBS computer.
Every cast is shown at **1920 × 1080** (scaled to fit, with black bars when the shape differs; change it in the source's properties). If OBS itself is not 1920 × 1080, the Vcast dock offers a one-click **Set OBS to 1920 x 1080** button. The picture is fitted to the screen automatically; use **Fit to screen** if you moved it.
