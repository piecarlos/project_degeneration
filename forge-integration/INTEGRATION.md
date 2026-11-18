# forge-integration — Resident Evil: Degeneration (N-Gage 2.0) — Release 0.1.0

Atualizado em 18/11/2026. Versão final de distribuição **0.1.0 (Windows x64)** com separação completa entre movimento e rotação no perfil alternativo, Menu `Select` in-game com pausa do emulador, importadores nativos em C17, detecção automática de firmware existente, caminhos portáteis relativos e empacotamento redistribuível livre de ROMs, firmware, jogos comerciais, saves ou ReShade.

## Resumo dos Resultados Validados (Release 0.1.0)

| Item | Status | Evidência / Resultado Objetivo |
|---|---|---|
| **Pacote Release `0.1.0` (`50.319.087 B`)** | ✅ Validado (`5/5 PASS`) | [`ResidentEvilDegeneration-0.1.0-windows-x64.zip`](file:///c:/Users/Jeronimo/Documents/Ngage%20games/resident-emulation/release/ResidentEvilDegeneration-0.1.0-windows-x64.zip) (`SHA-256: 7CA6943538F129CCC9745FA5886E36A7C2D594DF572C8DA54B5556F53FDB6C91`) auditado e validado em pasta isolada (`Verify-Release010.py`, incluindo `--import-bundle`) |
| **Testes Físicos + Gameplay 3D Real** | ✅ Validado | Testado fisicamente pelo usuário + suíte objetiva: Capítulo 1, áudio WASAPI estéreo (`0.034–0.064`), `28.5–30.0 FPS`, inicialização direta `GET /v1/ngage2/games` -> `POST /v1/ngage2/games/0x2001AD82/launch` |
| **Controles Dual-Stick + Padrão Preservado** | ✅ Validado | `bindings/resident_evil.yml` preservado como padrão; `bindings/resident_evil_rstick.yml` em modo **Dual-Stick Completo** (Esquerdo 4 direções `300..303` para movimento/curva/menus + Direito `304..307` + `R3=311` + `L3=310`) e `button_id: 6` reservado ao Menu Select |
| **Importação Unificada Assíncrona + Progresso** | ✅ Validado | Atalhos **Importar Pasta Completa**, **Selecionar Arquivos (Ctrl+A)** (`FOS_ALLOWMULTISELECT`) e **Vincular EKA2L1** em *worker thread* dedicada com barra de progresso em tempo real e validação rápida (<1ms) sem travar a interface |
| **Menu `Select` In-Game (Resume / Quit)** | ✅ Validado | Overlay `LegacyMobileForgeNGSelectMenu` (XInput `Select` ou `F9`), pausa (`POST /v1/emulator/pause`), retomada (`POST /v1/emulator/resume`) e `Quit Game` com desligamento gracioso |
| **Supervisão Windows Job Objects + Saves** | ✅ Validado (`6/6 PASS`) | `Verify-ProcessAndSavesNG.py`: `0` processos órfãos em fechamento normal, fechamento pelo launcher e `TerminateProcess`; espelho atômico em `app/saves_backup/` e autorreparo (`appdata.i3d`, `userdata.i3d`, `myprofile.db`, `as.dat`) |
| **Supressão do Diálogo de Atualização Qt** | ✅ Validado | Chave `QSettings` `HKCU\Software\EKA2L1\EKA2L1\noAutoCheckUpdate = "true"` + auto-fechamento de popups modais evita travamentos de `WM_CLOSE` e execução indevida de `updater.exe` |
| **Moldura Personalizada + Scale2x + ReShade Off** | ✅ Preservados | `bezel.png` (`SHA-256: B2326D1BE44FD52FA4844FFF58595F37E1866B0C6D6A7F750E32D33DE54C167C`, 1672×941, escala `67%`) + GLSL `Scale2x.frag` pré-moldura; ReShade desativado (zero `d3d9.dll`) |

## Estrutura Atualizada (`forge-integration`)

```
forge-integration/
├── INTEGRATION.md                   ← documentação técnica Release 0.1.0, controles, menu Select e testes
├── Build-LauncherNG.ps1             ← build C17 MSVC (/std:c17 /MT /W4 /WX, sem CLR)
├── Build-NGageRelease.ps1           ← gerador do pacote redistribuível Release 0.1.0 (.zip)
├── Verify-LauncherNG.py             ← suíte UI, apresentação e direct launch (5 etapas)
├── Verify-ProcessAndSavesNG.py      ← suíte Job Objects, saves e controle padrão/opcional (6 etapas)
├── Verify-Release010.py             ← auditoria do ZIP, testes em pasta isolada, importadores e Select Menu (5 etapas)
├── backend/
│   └── eka2l1_backend.json          ← contrato validado (RM-409 + Control API PlayServer)
├── launcher/
│   ├── GameLauncherNG.c             ← bootstrap C17 + portabilidade relativa + importadores nativos + Job Object + Select Menu
│   ├── LauncherUiNG.h               ← shell Win32/GDI/WIC + XInput + 4 abas + overlay LegacyMobileForgeNGSelectMenu
│   ├── launcher-ng.ini.template     ← template de configuração UTF-16LE (caminhos relativos)
│   └── ResidentEvilLauncher.exe     ← binário nativo x64 (288.256 B, SHA-256: 9162FC84...)
├── controls/
│   ├── resident_evil.yml            ← perfil PADRÃO validado (300/301 preservados; botão 6 Select liberado para overlay)
│   ├── resident_evil_rstick.yml     ← perfil Opcional Dual-Stick Completo (Esq. 300..303 + Dir. 304..307 + R3=311 + L3=310)
│   ├── keymap_evidence.json         ← engenharia reversa PlayerControls / ENGLISH.dat e políticas SDL2/XInput
│   └── SdlPadBridgeNG.py            ← registro da decisão pela rota nativa SDL2
└── presentation/
    ├── Scale2x.frag                 ← shader GLSL EPX/Scale2x pré-moldura
    └── PresentationNG.py            ← pipeline OpenGL (framebuffer -> Scale2x -> moldura)
```

## Separação de Controles e Limitação Intrínseca Documentada

A inspeção binária direta do código ARM Thumb de `Resident_Evil.unpacked.s3e` (`PlayerControls @ 424784`) e da tabela de localização `localisation/ENGLISH.dat` (comprimida em LZMA1 dentro de `main.dz`) demonstrou objetivamente:
- As **12 únicas ações remapeáveis** em *Options -> Controls* são: *Move forward*, *Move back*, *Turn left*, *Turn right*, 4 diagonais de movimento, *Reload weapon*, *Action/Aim/Attack*, *Use knife/Zoom* e *Exit aim/Use knife*.
- **Não existe canal separado de "Aim Up / Aim Down"**: `PlayerControls` multiplexa exatamente os 4 canais digitais Symbian (`16=Up`, `17=Down`, `14=Left`, `15=Right`) entre locomoção (`gundrawn == 0`, com câmera automática estilo RE4 acompanhando Leon) e controle bidimensional do laser/retícula (`gundrawn == 1`, onde Leon permanece parado enquanto mira).
- **Correção aplicada em `resident_evil_rstick.yml` (**Dual-Stick Completo**)**:
  - Como o jogo original usa *tank controls* sem *strafe* e depende dos canais `14/15` para rotação e navegação lateral em inventário/mapa, removê-los do Analógico Esquerdo impedia virar ou navegar para os lados com a alavanca esquerda.
  - No perfil atualizado **`Dual-Stick (Completo)`**, o Analógico Esquerdo mantém as 4 direções completas (`303/302/301/300 -> 16/17/14/15`) e o Analógico Direito também controla rotação/mira nas 4 direções (`307/306/305/304 -> 16/17/14/15`), mais `R3 (311) -> 51 ('3')` (*Zoom/Faca*) e `L3 (310) -> 180` (*Reload*).
  - Removido `button_id: 6` (*Select/Back*) de `resident_evil.yml` e `resident_evil_rstick.yml`, evitando envio acidental de `#` (`127`) ao jogo.

## Menu Select In-Game, Empacotamento Portátil e Importação Assíncrona

1. **Menu `Select` (`LegacyMobileForgeNGSelectMenu`)**:
   - Captura `XINPUT_GAMEPAD_BACK` (ou `F9` / `UI_OPEN_SELECT_NG` `WM_APP+3`) exclusivamente enquanto o jogo está em execução (`ng_game_live == 1`).
   - Chama `POST /v1/emulator/pause`, exibe as opções **`Resume Game`** e **`Quit Game`** centralizadas sobre a janela do jogo, retoma via `POST /v1/emulator/resume` em `Resume Game` (ou `B`/`Select`/`Esc`), e dispara `Quit Game` pelo fluxo gracioso em 3 estágios (`DELETE /v1/session` -> flush -> `WM_CLOSE` -> `TerminateJobObject`).
2. **Caminhos Portáteis e Detecção Não-Destrutiva**:
   - `config.yml` utiliza exclusivamente caminhos relativos calculados por `PathRelativePathToW` (`data-storage: "data"`, `background-image: "../../app/assets/bezel.png"` quando empacotado, `PATH_CAP=4096`).
   - `resolve_active_data_dir()` detecta instalações RM-409 preexistentes (`..\EKA2L1\data`, `..\..\EKA2L1\data`, `%APPDATA%\EKA2L1\data`, `%LOCALAPPDATA%\EKA2L1\data`, `Documents\EKA2L1\data`) sem copiar ROMs. Otimizado `file_has_valid_payload()` (checagem cabeça 64 KB + ponto médio) para evitar leitura de ~70 MB na UI.
3. **Importadores C17 Assíncronos + Barra de Progresso (UI em *Worker Thread* e CLI)**:
   - **Importar Tudo de Uma Vez (`Importar Pasta Completa` / `--import-bundle` e `Selecionar Arquivos Ctrl+A` via `FOS_ALLOWMULTISELECT`)**: `inspect_candidate_file()` / `scan_folder_for_bundle()` classificam automaticamente `SYM.ROM`, `SYM.RPKG` (magic `RPKG`), `ngage_client.sis` (`UID 0x20003B78`), pacote `.n-gage`/`.sis` (`UID 0x2001AD82`) ou pasta `EKA2L1` existente, executando tudo na ordem correta com barra de progresso em tempo real (`UI_IMPORT_PROGRESS_NG` / `ng_report_progress` 0%–100% + contador de arquivos do RPKG) sem congelar a janela.
   - **Firmware RM-409**: copia `SYM.ROM` com flush atômico, extrai streaming os ~8.988 arquivos de `SYM.RPKG`, propaga `TZDB.DBZ` e repara `devices.yml`.
   - **Cliente N-Gage 2.0**: valida cabeçalho Symbian SIS `0x10201A7A` (`UID 0x20003B78`) e supervisiona `eka2l1_qt.exe --install` via `--debug-control-port` (`/v1/status`), encerrando o emulador auxiliar assim que o serviço inicializa.
   - **Pacote `.n-gage` / `.sis`**: valida o contêiner MIME `--KBoundary` + `UID 0x2001AD82`, copia atomicamente para `<active_data_dir>\drives\e\n-gage\2001AD82.n-gage` e executa `run_eka2l1_register_ngage_game()`, que dispara o `NgiInstaller.exe` nativo em `0x20003B78` até confirmar `0x2001AD82` em `GET /v1/ngage2/games`.
4. **Política de Conteúdo e Licenças (`Build-NGageRelease.ps1`)**:
   - Exclui terminantemente `.rom`, `.rpkg`, `.sis`, `.sisx`, `.n-gage`, `.i3d`, `.db`, `.dat`, `.log`, `Resident_Evil.exe`, `NgiInstaller.exe`, `config.yml`, `devices.yml`, `updater/` e DLLs ReShade (`d3d9.dll`, `dxgi.dll`, `opengl32.dll`).
   - Inclui [`LICENSE`](file:///C:/workspace/LegacyMobileForge/LICENSE) (GPL-3.0-or-later), [`THIRD-PARTY.md`](file:///C:/workspace/LegacyMobileForge/THIRD-PARTY.md) e `SOURCES.txt` com código-fonte correspondente (EKA2L1 GPLv2+, SDL2 zlib, Qt6 LGPLv3/GPLv3, FFmpeg LGPLv2.1+, OpenSSL Apache-2.0).

Documentação reutilizável associada no *Legacy Mobile Forge*: [`NGAGE-RUNTIME.md`](file:///C:/workspace/LegacyMobileForge/docs/NGAGE-RUNTIME.md), [`PROCESS-MANAGEMENT.md`](file:///C:/workspace/LegacyMobileForge/docs/PROCESS-MANAGEMENT.md), [`NATIVE-LAUNCHER.md`](file:///C:/workspace/LegacyMobileForge/docs/NATIVE-LAUNCHER.md) e [`WINDOWS-PORTING-WORKFLOW.md`](file:///C:/workspace/LegacyMobileForge/docs/WINDOWS-PORTING-WORKFLOW.md).
