# Changelog — Resident Evil: Degeneration (N-Gage 2.0 on Windows)

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [0.1.0] - 2026-11-18

### Added (English)
- **In-Game Select Overlay Menu (`Select` / `Back` / `F9`)**:
  - Native Win32 overlay window (`LegacyMobileForgeNGSelectMenu`) inspired by the *Meteos Astroblocks* launcher engine.
  - Automatically pauses EKA2L1 emulation execution when opened (`POST /v1/emulator/pause`) and displays **Resume Game** and **Quit Game**.
  - Smoothly resumes emulation upon returning to gameplay (`POST /v1/emulator/resume`) or triggers a three-stage graceful shutdown returning cleanly to the launcher.
- **Portable Distribution Architecture (Release 0.1.0)**:
  - Reusable EKA2L1 runtime configuration supporting fully relative paths across `app\launcher-ng.ini`, `app\video.properties`, and runtime configs.
  - Automated zero-copy detection of existing EKA2L1 installations with valid Nokia 5320 RM-409 firmware in standard Windows directories.
  - **Asynchronous Unified Importer with Progress Bar**:
    - Added **Import Full Folder** and **Select Files (Ctrl+A)** in the launcher's System tab to import `SYM.ROM`, `SYM.RPKG`, `ngage_client.sis` (`0x20003B78`), and game package (`0x2001AD82`) in a single step.
    - Implemented background worker threads with real-time UI progress bars, completely eliminating interface freezing during multi-megabyte decompression and installation.
    - Added CLI option `--import-bundle <folder>` for headless automation.
  - Strict compliance with licensing and copyright: absolutely zero ROMs, firmware dumps, game binaries, personal saves, or ReShade binary DLLs included in public archives.
- **Kernel Process Supervision & Save Hardening**:
  - Assigned all child emulator processes to a dedicated **Windows Job Object** (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), guaranteeing immediate kernel-level termination of child processes if the launcher closes, crashes, or is killed.
  - Single-instance application mutex (`Local\ResidentEvilDegeneration_InstanceMutex`) and dynamic REST API port hygiene (`28182..28199`).
  - Atomic write-through save mirroring (`MOVEFILE_WRITE_THROUGH`) in `app\saves_backup\` (`appdata.i3d`, `userdata.i3d`, `DBMS\myprofile.db`, `as.dat`) with automated self-healing against 0-byte corrupted save files.
- **Bilingual Documentation & Cover Artwork**:
  - Full English (`README.md`) and Portuguese (`README.pt-BR.md`) documentation suites.
  - High-resolution cover artwork (`docs/assets/cover.png`) showcasing 3D gameplay inside the custom bezel.
  - Project positioning clarified as *Standalone Windows launcher powered by EKA2L1*.
  - Credits to EKA2L1 team, SDL2, Qt, FFmpeg, and Scale2x in documentation.
  - GitHub issue templates for Bug Reports and Feature Requests (`.github/ISSUE_TEMPLATE/`).
  - Comprehensive source code compliance and distribution notices in `THIRD-PARTY.md` and `SOURCES.txt`.

### Changed & Fixed (English)
- **Dual-Stick Control Scheme (`resident_evil_rstick.yml`)**:
  - Restored full 4-direction movement (`16/17/14/15`) to the Left Stick (`303/302/301/300`), ensuring complete left/right steering and inventory/menu navigation.
  - Configured Right Stick (`307/306/305/304`) for horizontal camera rotation (`14/15`) and 4-way laser aiming (`14/15/16/17`) when aiming.
  - Added stick clicks: `R3` (`311` -> key `'3'` `51`) for quick knife/zoom and `L3` (`310` -> key `180`) for weapon reload.
  - Unbound `Select` / `Back` (`button_id: 6`) from in-game Symbian scancodes to prevent accidental weapon switching when opening the Select Menu.
  - Fully preserved and verified the default classic control profile (`resident_evil.yml`).
- **Eliminated UI Freeze on File Validation**:
  - Replaced full-file reads in `file_has_valid_payload` with bounded 64 KB header + midpoint signature scans, reducing validation latency from hundreds of milliseconds to under 1 millisecond.
- **Configuration & Device Self-Healing**:
  - Fixed trailing newline handling during `config.yml` rewrite.
  - Added self-healing recreation of empty or corrupted `devices.yml` in runtime directories.

### Preserved (English)
- Custom illustrated 1080p bezel frame (`1672×941`, ratio 67% viewport = `960×720`).
- Optional Scale2x GLSL filter (Pixel-sharp default) and ReShade strictly disabled.

---

### Adicionado (Português)
- **Menu Select Sobreposto em Jogo (`Select` / `Back` / `F9`)**:
  - Overlay nativo Win32 (`LegacyMobileForgeNGSelectMenu`) com opções de **Resume Game** e **Quit Game**, pausando a emulação via `/v1/emulator/pause`.
- **Importador Unificado e Empacotamento Portátil**:
  - Importação em segundo plano com barra de progresso em tempo real para pasta completa ou seleção múltipla (`Ctrl+A`).
  - Detecção automática de firmware existente sem duplicação de dados.
  - Suporte completo a caminhos relativos para portabilidade em qualquer pasta ou pendrive.
- **Isolamento de Processos e Proteção de Saves**:
  - Windows Job Objects impedindo processos órfãos (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`).
  - Espelhamento atômico de saves (`app/saves_backup/`) com autorrecuperação contra arquivos zerados.
- **Documentação Bilíngue e Capa**:
  - Documentação completa em inglês (`README.md`) e português (`README.pt-BR.md`), com capa oficial em `docs/assets/cover.png`.
  - Posicionamento oficial do projeto como *launcher autônomo para Windows baseado no EKA2L1*.
  - Créditos aos desenvolvedores do EKA2L1, SDL2, Qt, FFmpeg e Scale2x.
  - Modelos de issue para relato de bugs e sugestões no GitHub (`.github/ISSUE_TEMPLATE/`).
  - Notificações de conformidade com a GPLv3 em `THIRD-PARTY.md` e `SOURCES.txt`.

### Corrigido e Ajustado (Português)
- **Analógico Esquerdo e Dual-Stick (`resident_evil_rstick.yml`)**:
  - Corrigido analógico esquerdo para responder nas 4 direções (cima, baixo, esquerda, direita).
  - Analógico direito para rotação e mira livre nas 4 direções.
  - Atalhos `R3` (Faca/Zoom) e `L3` (Recarregar).
- **Fim do Travamento da Interface**:
  - Validação ultrarrápida (< 1 ms) e execução em worker thread assíncrona.
