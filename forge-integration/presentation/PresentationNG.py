"""
PresentationNG.py — Estado validado da apresentação visual no backend EKA2L1

ESTADO: validado (Moldura ativa por padrão; Scale2x opcional pré-moldura; ReShade desativado).

Pipeline real confirmado em EKA2L1 (`mainwindow.cpp` + `graphics_ogl.cpp`)
--------------------------------------------------------------------------
  1. Limpeza do backbuffer OpenGL (`0,0,0`)
  2. Desenho de `background-image` (`app/assets/bezel.png`, cópia íntegra de `moldura 1.png`,
     1672×941, SHA-256 `B2326D1BE44FD52FA4844FFF58595F37E1866B0C6D6A7F750E32D33DE54C167C`)
     com `background-image-keep-aspect: true` e `background-image-opacity: 255`.
  3. Aplicação opcional do shader GLSL `resources/upscale/Scale2x.frag` (algoritmo EPX/Scale2x
     idêntico a `PixelArtFilter.java`) **exclusivamente** sobre `scr->screen_texture` (320×240)
     quando `Scale2x` é selecionado no launcher (`compat/2001AD82.yml` -> `screen-upscale-method: 1`,
     `filter-shader-path: Scale2x`).
  4. Escalonamento centralizado do viewport 4:3 com `screen-scale-ratio: 67` (exatamente 960×720 =
     escala inteira 3× em 1080p, preservando integralmente o logotipo e as artes laterais de Leon e Claire).
  5. Apresentação via OpenGL (`ogl_graphics_driver`).

ReShade
-------
  - Status: DESATIVADO (`enabled = False`).
  - API host identificada: OpenGL (`QWindow::OpenGLSurface` / `ogl_graphics_driver` x64).
  - Risco documentado: `LegacyMobileForge/docs/RESHADE.md` registra falha em `atio6axx.dll` com
    OpenGL em GPU AMD; nenhuma DLL ou injeção do ReShade foi instalada ou ativada.
"""

SCALE2X_STATUS     = "validated-optional-glsl-pre-bezel"
RESHADE_STATUS     = "disabled-by-directive-host-opengl-identified"
BEZEL_SOURCE       = "app/assets/bezel.png"
BEZEL_SCALE_RATIO  = 67
FRAMEBUFFER_WIDTH  = 320
FRAMEBUFFER_HEIGHT = 240

if __name__ == "__main__":
    print(f"Scale2x: {SCALE2X_STATUS}")
    print(f"ReShade: {RESHADE_STATUS}")
    print(f"Bezel:   {BEZEL_SOURCE} (ratio={BEZEL_SCALE_RATIO}%)")
