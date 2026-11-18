# Resident Evil: Degeneration (N-Gage 2.0) — Windows Release 0.1.0

**Resident Evil Degeneration for Windows — Standalone N-Gage 2.0 gaming experience powered by EKA2L1, featuring controller support, custom launcher and enhanced presentation.**

---

### 🌟 Key Highlights & Features

- **Direct Game Boot**: Automatically initializes the Symbian OS runtime and launches *Resident Evil: Degeneration* (UID `0x2001AD82`) directly into 3D gameplay, bypassing emulator system menus.
- **In-Game Select Menu Overlay (`Select` / `Back` / `F9`)**:
  - Native Win32 overlay allowing players to pause emulation (`POST /v1/emulator/pause`) at any moment.
  - Offers **Resume Game** and **Quit Game** options with safe, graceful process termination and zero orphans.
- **Kernel Process Supervision**:
  - Managed by a restricted Windows Kernel **Job Object** (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`).
  - Child processes (`eka2l1_qt.exe`) are unconditionally terminated if the launcher closes, crashes, or is stopped via Task Manager.
- **Atomic Save Protection**:
  - Progress files (`appdata.i3d`, `userdata.i3d`, `DBMS\myprofile.db`, `2001AD82\as.dat`) are flushed (`FlushFileBuffers`) before exit and mirrored atomically in `app/saves_backup/`.
  - Self-healing recovery automatically restores previous valid states if a save file is corrupted by sudden power loss.
- **Non-Blocking Unified Importer**:
  - Background worker thread with real-time progress bar.
  - Import all required files at once via **Import Full Folder** or **Select Files (Ctrl+A)** without UI freezes.
  - Automatic zero-copy detection of pre-existing Nokia 5320 RM-409 firmware installations.
- **Dual Gamepad Profiles**:
  - **Default Validated (`resident_evil.yml`)**: Authentic classic tank controls with D-Pad/Left Stick and face-button aiming/firing.
  - **Full Dual-Stick (`resident_evil_rstick.yml`)**: 4-way Left Stick steering + 4-way Right Stick camera rotation and laser aiming, plus `R3` Zoom/Knife and `L3` Reload.
- **Enhanced Presentation**:
  - Custom 1080p illustrated bezel frame (`1672×941`, ratio 67% viewport = `960×720`).
  - Optional Scale2x GLSL shader filter (Pixel-sharp default). ReShade strictly disabled.

---

### 📦 Package Artifact & Checksums

| Package Property | Value |
|---|---|
| **Filename** | `ResidentEvilDegeneration-0.1.0-windows-x64.zip` |
| **File Size** | `50,319,087 bytes` (47.98 MB) |
| **SHA-256 Checksum** | `7CA6943538F129CCC9745FA5886E36A7C2D594DF572C8DA54B5556F53FDB6C91` |
| **Target OS** | Windows 10 / Windows 11 (64-bit) |

> 🛡️ **Clean Package Audit Verified**: Contains **NO** proprietary Symbian ROMs, **NO** Nokia firmware, **NO** game binaries, **NO** personal saves, and **NO** hardcoded computer paths.

---

### 🚀 Getting Started

1. Download **`ResidentEvilDegeneration-0.1.0-windows-x64.zip`** and extract it to any writable folder.
2. Run **`ResidentEvilLauncher.exe`** (or `jogar_resident_evil.bat`).
3. If files are missing, the launcher opens in the **System** tab. Import your legitimate copies of:
   - `SYM.ROM` & `SYM.RPKG` (Nokia 5320 RM-409 firmware)
   - `ngage_client.sis` (N-Gage 2.0 client installer)
   - `Resident Evil Degeneration.n-gage` (Game package)
4. Select your preferred resolution and filter in **Display**, then click **PLAY GAME**!

---

### ⚠️ Known Limitations

1. **Original Game Camera Constraints**: The original Symbian S3E engine uses classic tank controls and 4 directional input scancodes (`16, 17, 14, 15`). There is no independent 360° orbital camera in the original code. In the Dual-Stick profile, the Right Stick controls rotation and vertical reticle adjustment during aiming.
2. **Select Menu Controller Input**: The in-game Select Menu responds to XInput gamepad input (`Select` / `Back`). PlayStation controllers can use DS4Windows / Steam Input, or press `F9` on the keyboard.

---

### 🙏 Credits & Acknowledgments

- **EKA2L1 Project** ([github.com/EKA2L1/EKA2L1](https://github.com/EKA2L1/EKA2L1)) — Open-source Symbian OS / N-Gage 2.0 emulator core, CPU translation, and REST Control API. (GNU GPLv3).
- **libSDL** ([libsdl.org](https://www.libsdl.org/)) — Gamepad mapping database and SDL2 runtime. (zlib License).
- **Qt Project** ([qt.io](https://www.qt.io/)) — Qt6 GUI and multimedia platform plugins.
- **FFmpeg** ([ffmpeg.org](https://ffmpeg.org/)) — Audio and video decoding libraries.
- **Capcom Co., Ltd.** — Original *Resident Evil: Degeneration* mobile survival horror game.
- **Nokia Corporation** — Historical N-Gage 2.0 platform.
