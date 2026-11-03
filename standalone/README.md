# Audio Hardware Volume Preset (Standalone Mini App)

A lightweight (~160 KB), portable background utility for Windows that sets audio playback devices to configured volume percentages when they connect. Does not require Windhawk or any third-party frameworks.

---

## Features
- **Zero Dependencies**: Pure native Win32 & Core Audio APIs (`IMMNotificationClient`, `IAudioEndpointVolume`).
- **Tray Resident**: Runs silently in the system notification area with a speaker tray icon.
- **Auto Startup**: Option to run on Windows startup via the tray menu.
- **Easy Config**: Uses a simple `config.ini` file located next to the `.exe`.
- **Double-Click Edit**: Double-clicking the tray icon opens `config.ini` in your default text editor.
- **Instant Reload**: Click "Reload Settings" in the tray menu to apply config changes without restarting.
- **Two-Pass Matching**: Matches exact device names first, with generic token matching fallback.
- **Safety Ceiling**: Enforces maximum volume limits to protect hearing.

---

## Configuration (`config.ini`)

Edit `config.ini` in the same directory as the executable:

```ini
[Settings]
DefaultVolume = 30
MaxVolumeLimit = 100
ApplyToAllDevices = 1
AutoUnmute = 1
ApplyOnStartup = 1

[Rules]
AirPods = 30
Sony = 25
Speakers = 50
DAC = 40
```

---

## Building from Source

Run `build.bat` or compile manually with Clang / MinGW-w64:

```cmd
clang++ --target=x86_64-w64-windows-gnu -O2 -mwindows main.cpp -lole32 -lshell32 -lshlwapi -ladvapi32 -luser32 -lgdi32 -o AudioHardwareVolumePreset.exe
```

---

## License
MIT License. See [LICENSE](../LICENSE).
