# Firmware Binaries

Precompiled firmware for the one-click web installer.

## What goes here

Four files, exactly as Arduino IDE produces them:

```
firmware/
├── SwarmDashboard.ino.bootloader.bin   (boot loader, offset 0x1000)
├── SwarmDashboard.ino.partitions.bin   (partition table, offset 0x8000)
├── boot_app0.bin                       (OTA selector, offset 0xE000)
└── SwarmDashboard.ino.bin              (your sketch, offset 0x10000)
```

ESP Web Tools combines them on the fly during install — no merging required.

## How to build a new release

### 1. Compile in Arduino IDE

Open `SwarmDashboard.ino`. Set the board to **ESP32 Dev Module** with:
- Flash Size: **4MB (32Mb)**
- Partition Scheme: **Default 4MB with spiffs**
- PSRAM: **Disabled**

Click **Sketch → Export Compiled Binary** (Alt+Ctrl+S). Wait for compile to finish.

### 2. Locate the build output

**Sketch → Show Sketch Folder** opens the source folder. Inside is `build/esp32.esp32.esp32/` containing:

- `SwarmDashboard.ino.bootloader.bin`
- `SwarmDashboard.ino.partitions.bin`
- `SwarmDashboard.ino.bin`

### 3. Find boot_app0.bin (one-time setup)

You also need `boot_app0.bin`. It's part of the ESP32 board package and lives in your Arduino installation:

**Windows:**
```
C:\Users\<YOU>\AppData\Local\Arduino15\packages\esp32\hardware\esp32\<VERSION>\tools\partitions\boot_app0.bin
```

**macOS / Linux:**
```
~/Library/Arduino15/packages/esp32/hardware/esp32/<VERSION>/tools/partitions/boot_app0.bin
```

Copy this file into the `firmware/` folder. You only need to do this once — it doesn't change between builds.

### 4. Copy all 4 .bin files into firmware/

Drop these into this folder, overwriting any old versions:

- `SwarmDashboard.ino.bootloader.bin`
- `SwarmDashboard.ino.partitions.bin`
- `SwarmDashboard.ino.bin`
- `boot_app0.bin` (if not already here)

### 5. Update the version in manifest.json

Edit [`docs/install/manifest.json`](../docs/install/manifest.json) and bump the `"version"` field. Filenames stay the same — versioning is tracked through git history.

### 6. Commit and push

GitHub Pages auto-redeploys within 1–2 minutes after push. The install page at https://apathec.github.io/Home-Swarm-Dashboard/install/ will then flash the new firmware.

## Why four files instead of one?

ESP32 boots in stages — bootloader → partition table → OTA selector → sketch. Each lives at a specific flash offset. Most flashing tools handle this internally; ESP Web Tools just exposes the offsets in the manifest so anyone can see exactly what gets flashed where.
