"""
SdlPadBridgeNG.py — Documentação e verificação da rota nativa de controles SDL2 no EKA2L1

ESTADO: validado via suporte nativo do EKA2L1 (bindings/resident_evil.yml).

Decisão arquitetural (17/11/2026)
---------------------------------
O EKA2L1 já incorpora nativamente `SDL2.dll` + `resources/gamecontrollerdb.txt`
e carrega perfis YAML em `bindings/<perfil>.yml` via `--keybindprofile <perfil>`.
Portanto, a Opção 1 (suporte nativo SDL2 do EKA2L1) elimina a necessidade de
um processo intermediário `SendInput` para o jogo, evitando latência extra ou
conflitos de foco. No launcher pré-jogo (`GameLauncherNG.c` / `LauncherUiNG.h`),
mantém-se o polling nativo Win32 `XInput` do Forge.

Limitação registrada:
- O perfil `bindings/resident_evil.yml` foi validado estruturalmente e as ações
  Symbian foram confirmadas em gameplay 3D real no EKA2L1, porém nenhum controle
  físico (Xbox/DualSense/GameSir) estava conectado ao host durante a sessão de
  testes automatizados. O teste físico permanece recomendado ao conectar o pad.
"""

VALIDATED_PROFILE = "bindings/resident_evil.yml"
CLI_FLAG = "--keybindprofile resident_evil"

VERIFIED_SYMBIAN_TARGETS = {
    16:  "Up / Walk Forward (rotacionado automaticamente pelo Symbian RM-409)",
    17:  "Down / Step Back",
    14:  "Left / Turn Left",
    15:  "Right / Turn Right",
    167: "Center Select / Action / Aim / Fire",
    164: "Left Softkey (Top-Left in Landscape): SELECT / NEXT / PAUSE (Map & Inventory)",
    165: "Right Softkey (Bottom-Left in Landscape): BACK / SKIP / KNIFE / EXIT AIM",
    180: "Call / Green (atalho secundário / N-Gage A candidato)",
}

if __name__ == "__main__":
    print(f"Perfil nativo ativo: {VALIDATED_PROFILE} ({CLI_FLAG})")
    for code, desc in VERIFIED_SYMBIAN_TARGETS.items():
        print(f"  scancode {code:3d} -> {desc}")
