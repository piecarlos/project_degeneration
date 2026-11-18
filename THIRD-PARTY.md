# Third-Party Notices and Licensing Compliance

This project integrates open-source libraries and an upstream runtime supervisor to provide a standalone gaming environment. All software components are used in strict compliance with their respective licenses.

---

## 1. Project License (MIT License / GPL-3.0)

- **Native C17 Launcher & Windows Integration (`ResidentEvilLauncher.exe`, `forge-integration/`)**:
  - Licensed under the **MIT License** and **GPL-3.0-or-later** (see [LICENSE](LICENSE)).
  - Source code is available in this repository: [https://github.com/piecarlos/project_degeneration](https://github.com/piecarlos/project_degeneration).

---

## 2. EKA2L1 Symbian / N-Gage 2.0 Emulator Core (GNU GPLv3)

- **Upstream Project**: EKA2L1 (Experimental Symbian OS Emulator)
- **Primary Authors**: bentokun, shadowe32, and the EKA2L1 contributors.
- **License**: **GNU General Public License v3.0 (GPLv3)**
- **Source Code Availability (GPLv3 Section 6 Compliance)**:
  - In accordance with the GNU General Public License v3.0, the complete corresponding source code for the EKA2L1 runtime binary (`runtime/eka2l1/eka2l1_qt.exe`, built against upstream revision `master-90f46cf91`) is publicly accessible at the official upstream Git repository:  
    👉 **[https://github.com/EKA2L1/EKA2L1](https://github.com/EKA2L1/EKA2L1)**
  - A copy of the GNU General Public License v3.0 is included in [LICENSE](LICENSE).

---

## 3. Bundled Runtime Dependencies & Libraries

| Dependency | Purpose | License | Source / Upstream |
|---|---|---|---|
| **Simple DirectMedia Layer (SDL2)** | Gamepad input abstraction & `gamecontrollerdb.txt` | **zlib License** | [libsdl.org](https://www.libsdl.org/) |
| **Qt 6 Framework** | GUI, window management & platform plugins | **LGPLv3 / GPLv3** | [qt.io](https://www.qt.io/) |
| **FFmpeg Libraries** | Multimedia, audio/video streaming & codec decoding | **LGPLv2.1+ / GPL** | [ffmpeg.org](https://ffmpeg.org/) |
| **OpenSSL** | Cryptographic primitives & SSL runtime | **Apache-2.0** | [openssl.org](https://www.openssl.org/) |
| **DirectX Shader Compiler** | Direct3D shader compilation (`D3Dcompiler_47`) | **Microsoft Redistributable** | [microsoft.com](https://www.microsoft.com/) |
| **Scale2x GLSL Shader** | Pixel-art scaling algorithm | **GNU GPL** (Andrea Mazzoleni) | [scale2x](http://www.scale2x.it/) |

---

## 4. Proprietary Asset Disclaimer & Excluded Content

- **No Commercial ROMs or Firmware**: This project and its releases **contain NO Symbian OS ROM images (`SYM.ROM`), NO RPKG firmware files (`SYM.RPKG`), NO Nokia N-Gage client installers (`ngage_client.sis`), and NO Capcom game executables (`.n-gage`/`.sis`)**.
- **No Personal Data**: No personal save data (`.i3d`, `.dat`, `.db`) is distributed.
- **Fair-Use Preservation**: *Resident Evil* and *Resident Evil: Degeneration* are registered trademarks of **Capcom Co., Ltd.** *N-Gage*, *Symbian*, and *Nokia* are registered trademarks of **Nokia Corporation**. All trademarks and intellectual property belong to their respective copyright holders.
