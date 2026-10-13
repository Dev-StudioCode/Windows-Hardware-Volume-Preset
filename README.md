# Audio Hardware Volume Preset (Windhawk Mod)

A [Windhawk](https://windhawk.net/) mod for Windows that automatically sets your audio output hardware (Bluetooth headphones, 3.5mm jack, USB DACs, speakers, etc.) to a specified volume percentage whenever they connect.

---

## 🎧 The Problem It Solves
Have you ever plugged in your headphones or reconnected Bluetooth earbuds only for Windows to blast your eardrums at 100% volume? Or do you always prefer your external speakers at 50% and your headphones at 25%?

This mod solves that problem by listening directly to Windows audio endpoint connection events. Whenever any playback device connects or switches to active:
1. It matches the device against your custom device rules (e.g., "AirPods", "Sony", "Speakers").
2. Sets the master volume level automatically.
3. Optionally ensures the device is unmuted.
4. Leaves you free to adjust the volume manually afterward without interfering.

---

## 🚀 How to Use in Windhawk

### Step 1: Enable Developer Mode in Windhawk
1. Open the **Windhawk** application on your PC.
2. Click the **Settings** (gear icon) on the top right or left navigation.
3. Check the box for **"Developer mode"** (or switch to the **Developer** tab).

### Step 2: Add and Compile the Mod
1. In the **Developer** tab, click **"New mod"**.
2. Replace the template editor content with the entire code from [`audio-hardware-volume-preset.wh.cpp`](./audio-hardware-volume-preset.wh.cpp).
3. Click the **"Compile and inject"** (or **"Compile and test"**) button in the top right of the editor.
4. Windhawk will compile the mod using Clang and run it in a dedicated standalone tool process (`windhawk.exe`).

### Step 3: Configure Your Settings
Once compiled, go to the **Settings** tab inside the mod's Windhawk page:
- **Default Volume (%)**: Fallback volume (e.g., `30%`) applied to any connected playback device.
- **Max Volume Safety Limit (%)**: Maximum ceiling cap (default `100%`). No device will exceed this limit.
- **Apply to all devices by default**: Enabled by default. If disabled, only devices matching custom rules will have their volume preset.
- **Unmute on connect**: Automatically unmutes the newly connected device.
- **Apply on mod startup**: If enabled, sets the current audio device's volume as soon as the mod initializes.
- **Custom Device Presets**: Define custom rules using simple keywords:
  - `name`: `AirPods` $\rightarrow$ `volume`: `30`
  - `name`: `Sony` $\rightarrow$ `volume`: `25`
  - `name`: `Speakers` $\rightarrow$ `volume`: `50`
  - `name`: `USB DAC` $\rightarrow$ `volume`: `40`
  *(Note: You only need to enter any keyword from your device name; you do NOT need to type full prefixes like "Speakers" or "Headphones".)*

---

## 🌐 Working with GitHub

### Option A: Hosting Your Own GitHub Repository
1. Initialize Git and commit the files locally:
   ```bash
   git init
   git add .
   git commit -m "Initial commit of Audio Hardware Volume Preset Windhawk mod"
   ```
2. Create a new repository on [GitHub](https://github.com/new) (e.g., `audio-hardware-volume-preset`).
3. Connect and push your repository:
   ```bash
   git branch -M main
   git remote add origin https://github.com/<your-username>/audio-hardware-volume-preset.git
   git push -u origin main
   ```

### Option B: Contributing to the Official Windhawk Catalog
To make your mod available to all Windhawk users directly through the Windhawk in-app mod store:
1. Fork the official Windhawk mods repository: [ramensoftware/windhawk-mods](https://github.com/ramensoftware/windhawk-mods) on GitHub.
2. Clone your fork:
   ```bash
   git clone https://github.com/<your-username>/windhawk-mods.git
   cd windhawk-mods
   ```
3. Copy [`audio-hardware-volume-preset.wh.cpp`](./audio-hardware-volume-preset.wh.cpp) into the `mods/` directory of the cloned repo:
   ```bash
   cp "d:\#Anti Gravity\Windows Hardware Volume Set\audio-hardware-volume-preset.wh.cpp" mods/
   ```
4. Commit and push your branch:
   ```bash
   git checkout -b add-audio-volume-preset
   git add mods/audio-hardware-volume-preset.wh.cpp
   git commit -m "Add Audio Hardware Volume Preset mod"
   git push origin add-audio-volume-preset
   ```
5. Open a **Pull Request (PR)** to `ramensoftware/windhawk-mods`. Once reviewed and merged by the maintainer, your mod will appear inside the Windhawk app for anyone to install with one click!

---

## 🛠️ Technical Details
- **Target Process**: `windhawk.exe` (dedicated standalone tool process)
- **Compiler**: Clang / LLVM (MinGW-w64) with `-lole32 -lshell32`
- **Windows APIs**: Core Audio APIs (`IMMDeviceEnumerator`, `IMMNotificationClient`, `IAudioEndpointVolume`, `IPropertyStore`)
- **Resource Usage**: Event-driven notification system; zero polling loops and zero background CPU usage.
