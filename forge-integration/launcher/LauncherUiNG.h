/*
 * LauncherUiNG.h — Shell nativo Win32/GDI/WIC (C17) para backend EKA2L1 (N-Gage 2.0)
 *
 * Reaproveita o padrão visual e funcional de LauncherUi.h (LegacyMobileForge/engine/native/LauncherUi.h):
 *   - Cabeçalho com arte do caso (assets/bezel.png — moldura 1.png) via WIC
 *   - Abas: Início / Imagem / Controles / Sistema (incluindo importadores C17 via IFileOpenDialog)
 *   - Resolução (540p, 1080p, 1440p, 3K, 4K), Janela/Tela cheia, Nítido/Suave/Scale2x,
 *     Escala inteira e Moldura ilustrada
 *   - Controles: mantém perfil padrão validado (resident_evil) e oferece perfil separado
 *     Analógico Direito (resident_evil_rstick: Esquerdo = movimento, Direito = rotação + mira)
 *   - Menu Select em jogo (LegacyMobileForgeNGSelectMenu): pausa a emulação e oferece
 *     Resume Game / Quit Game com encerramento gracioso e retorno sem processos órfãos
 *
 * NÃO MODIFICA LauncherUi.h nem GameLauncher.c originais do Forge.
 */

#include <wincodec.h>
#include <xinput.h>
#include <windowsx.h>
#include <dwmapi.h>

static const COLORREF NG_BG    = RGB(9, 12, 19);
static const COLORREF NG_PANEL = RGB(21, 27, 39);
static const COLORREF NG_LINE  = RGB(48, 61, 80);
static const COLORREF NG_BLUE  = RGB(45, 126, 255);
static const COLORREF NG_TEXT  = RGB(239, 244, 255);
static const COLORREF NG_MUTED = RGB(157, 176, 202);
static const COLORREF NG_OK    = RGB(98, 214, 149);
static const COLORREF NG_ERR   = RGB(255, 108, 92);

/* Paleta do Menu Select em jogo (equivalente ao SettingsOverlay.java do Meteos Astroblocks) */
static const COLORREF OVL_BG     = RGB(14, 21, 33);
static const COLORREF OVL_LINE   = RGB(42, 61, 79);
static const COLORREF OVL_ROW    = RGB(20, 31, 45);
static const COLORREF OVL_ACTIVE = RGB(28, 64, 80);
static const COLORREF OVL_ACCENT = RGB(95, 217, 226);

#define UI_DONE_NG            (WM_APP + 2)
#define UI_OPEN_SELECT_NG     (WM_APP + 3)
#define UI_CLOSE_SELECT_NG    (WM_APP + 4)

typedef struct { RECT box; int action, value; } NgHit;

static HWND    ng_window;
static HWND    ng_overlay_window;
static HBITMAP ng_art;
static int     ng_art_w, ng_art_h;
static int     ng_tab, ng_focus, ng_count;
static NgHit   ng_hits[48];
static BOOL    ng_running;
static DWORD   ng_direct_exit_code;
static HANDLE  ng_worker;
static HANDLE  ng_import_worker;
static WCHAR   ng_notice[256];
static WCHAR   ng_import_result_notice[256];
static HMODULE ng_xinput;
typedef DWORD (WINAPI *NgGetState)(DWORD, XINPUT_STATE *);
static NgGetState ng_get_state;
static WORD       ng_last_buttons;
static ULONGLONG  ng_repeat_at;
static int        ng_last_axis;

static BOOL       ng_overlay_open;
static BOOL       ng_emu_paused;
static int        ng_overlay_row;
static BOOL       ng_last_f9;
static RECT       ng_ovl_hits[2];

/* ------------------------------------------------------------------ helpers */

static void ng_fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void ng_text(HDC dc, const WCHAR *text, RECT r,
                    int size, int weight, COLORREF color, UINT flags) {
    HFONT f = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(f);
}

static void ng_outline(HDC dc, RECT r, COLORREF c, int t) {
    ng_fill(dc, (RECT){r.left, r.top, r.right, r.top + t}, c);
    ng_fill(dc, (RECT){r.left, r.bottom - t, r.right, r.bottom}, c);
    ng_fill(dc, (RECT){r.left, r.top, r.left + t, r.bottom}, c);
    ng_fill(dc, (RECT){r.right - t, r.top, r.right, r.bottom}, c);
}

static void ng_button(HDC dc, RECT r, const WCHAR *label,
                      int action, int value, BOOL selected) {
    int idx = ng_count;
    if (idx >= (int)ARRAYSIZE(ng_hits)) return;
    ng_hits[ng_count++] = (NgHit){r, action, value};
    ng_fill(dc, r, selected ? NG_BLUE : NG_PANEL);
    ng_outline(dc, r,
        ng_focus == idx ? RGB(221, 237, 255) : (selected ? NG_BLUE : NG_LINE),
        ng_focus == idx ? 2 : 1);
    ng_text(dc, label, r, 13, FW_SEMIBOLD, NG_TEXT,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void ng_row(HDC dc, int width, int y,
                   const WCHAR *label, const WCHAR *desc,
                   const WCHAR **options, int count, int current, int action) {
    ng_text(dc, label, (RECT){36, y, width - 36, y + 25},
            16, FW_SEMIBOLD, NG_TEXT, DT_SINGLELINE);
    ng_text(dc, desc, (RECT){36, y + 29, width - 36, y + 53},
            13, FW_NORMAL, NG_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
    int right = width - 36, cw = min(560, width / 2), left = right - cw;
    for (int i = 0; i < count; i++)
        ng_button(dc,
            (RECT){left + i * cw / count, y, right - (count - i - 1) * cw / count, y + 30},
            options[i], action, i, current == i);
    ng_fill(dc, (RECT){36, y + 66, width - 36, y + 67}, NG_LINE);
}

/* ---------------------------------------------------------------- load art */

static void ng_load_art(void) {
    if (!*bezel_file || GetFileAttributesW(bezel_file) == INVALID_FILE_ATTRIBUTES)
        return;

    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder  = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv   = NULL;
    void *bits = NULL;

    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL,
        CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&factory);
    if (SUCCEEDED(hr))
        hr = IWICImagingFactory_CreateDecoderFromFilename(factory, bezel_file, NULL,
            GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr))
        hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr))
        hr = IWICBitmapFrameDecode_GetSize(frame, &w, &h);
    if (!w || !h || w > 8192 || h > 8192) hr = E_INVALIDARG;
    if (SUCCEEDED(hr))
        hr = IWICImagingFactory_CreateFormatConverter(factory, &conv);
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_Initialize(conv,
            (IWICBitmapSource *)frame,
            &GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, NULL, 0,
            WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = (LONG)w;
        bi.bmiHeader.biHeight      = -(LONG)h;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        ng_art = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (ng_art && SUCCEEDED(IWICFormatConverter_CopyPixels(
                conv, NULL, w * 4, w * h * 4, bits))) {
            ng_art_w = (int)w;
            ng_art_h = (int)h;
        } else if (ng_art) {
            DeleteObject(ng_art);
            ng_art = NULL;
        }
    }
    if (conv)    IWICFormatConverter_Release(conv);
    if (frame)   IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (factory) IWICImagingFactory_Release(factory);
}

/* ----------------------------------------------------- in-game Select Menu */

static void ng_open_select_menu(void) {
    if (!ng_running || !ng_overlay_window || ng_overlay_open) return;
    if (!InterlockedCompareExchange(&ng_game_live, 1, 1)) return;

    char resp[512];
    DWORD status = 0;
    if (http_local_ex(active_control_port, 800, L"POST", L"/v1/emulator/pause",
                      resp, sizeof(resp), &status)) {
        ng_emu_paused = TRUE;
    }

    RECT target_rect = {0};
    DWORD eka_pid = (DWORD)InterlockedCompareExchange((volatile LONG *)&ng_eka_pid, 0, 0);
    HWND eka_win = eka_pid ? find_eka2l1_qt_window(eka_pid) : NULL;
    if (!eka_win || !GetWindowRect(eka_win, &target_rect)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &target_rect, 0);
    }

    UINT dpi = GetDpiForWindow(ng_overlay_window);
    if (!dpi) dpi = 96;
    int ovl_w = MulDiv(560, (int)dpi, 96);
    int ovl_h = MulDiv(320, (int)dpi, 96);
    int x = target_rect.left + ((target_rect.right - target_rect.left) - ovl_w) / 2;
    int y = target_rect.top  + ((target_rect.bottom - target_rect.top) - ovl_h) / 2;

    ng_overlay_row = 0;
    ng_overlay_open = TRUE;
    SetWindowPos(ng_overlay_window, HWND_TOPMOST, x, y, ovl_w, ovl_h, SWP_SHOWWINDOW);
    SetForegroundWindow(ng_overlay_window);
    InvalidateRect(ng_overlay_window, NULL, FALSE);
    UpdateWindow(ng_overlay_window);
    event("select_menu.open", ng_emu_paused ? 1 : 0);
}

static void ng_resume_from_select_menu(void) {
    if (!ng_overlay_open) return;
    ng_overlay_open = FALSE;
    if (ng_overlay_window) ShowWindow(ng_overlay_window, SW_HIDE);

    if (ng_emu_paused) {
        char resp[512];
        DWORD status = 0;
        http_local_ex(active_control_port, 800, L"POST", L"/v1/emulator/resume",
                      resp, sizeof(resp), &status);
        ng_emu_paused = FALSE;
    }

    DWORD eka_pid = (DWORD)InterlockedCompareExchange((volatile LONG *)&ng_eka_pid, 0, 0);
    HWND eka_win = eka_pid ? find_eka2l1_qt_window(eka_pid) : NULL;
    if (eka_win) SetForegroundWindow(eka_win);
    event("select_menu.resume", 0);
}

static void ng_quit_from_select_menu(void) {
    if (ng_overlay_window) ShowWindow(ng_overlay_window, SW_HIDE);
    ng_overlay_open = FALSE;

    if (ng_emu_paused) {
        char resp[512];
        DWORD status = 0;
        http_local_ex(active_control_port, 800, L"POST", L"/v1/emulator/resume",
                      resp, sizeof(resp), &status);
        ng_emu_paused = FALSE;
    }

    InterlockedExchange(&ng_quit_from_select, 1);
    event("select_menu.quit", 0);
    if (ng_stop_event) SetEvent(ng_stop_event);
}

static void ng_overlay_paint(HDC dest, int width, int height) {
    int pw = width, ph = height;
    HDC dc = CreateCompatibleDC(dest);
    HBITMAP buf = CreateCompatibleBitmap(dest, width, height);
    HGDIOBJ old = SelectObject(dc, buf);

    SetMapMode(dc, MM_ANISOTROPIC);
    SetWindowExtEx(dc, 560, 320, NULL);
    SetViewportExtEx(dc, width, height, NULL);
    width = 560;
    height = 320;

    ng_fill(dc, (RECT){0, 0, width, height}, OVL_BG);
    ng_outline(dc, (RECT){0, 0, width, height}, OVL_LINE, 2);

    ng_text(dc, L"RESIDENT EVIL: DEGENERATION", (RECT){26, 20, 410, 52},
            18, FW_BOLD, RGB(232, 238, 245), DT_SINGLELINE | DT_END_ELLIPSIS);
    ng_text(dc, tr(L"PAUSED", L"EM PAUSA"), (RECT){410, 22, width - 26, 48},
            13, FW_SEMIBOLD, ng_emu_paused ? OVL_ACCENT : NG_MUTED, DT_RIGHT | DT_SINGLELINE);
    ng_text(dc, tr(L"Game session menu", L"Menu da partida"), (RECT){26, 52, width - 26, 74},
            13, FW_NORMAL, RGB(145, 162, 181), DT_SINGLELINE);

    ng_ovl_hits[0] = (RECT){24, 96, width - 24, 154};
    ng_ovl_hits[1] = (RECT){24, 166, width - 24, 224};

    const WCHAR *row_labels[] = {
        L"Resume Game",
        L"Quit Game"
    };
    const WCHAR *row_hints[] = {
        tr(L"Continue playing", L"Continuar partida"),
        tr(L"Save & return to launcher", L"Salvar e voltar ao launcher")
    };

    for (int i = 0; i < 2; i++) {
        RECT r = ng_ovl_hits[i];
        BOOL active = (ng_overlay_row == i);
        ng_fill(dc, r, active ? OVL_ACTIVE : OVL_ROW);
        if (active) ng_fill(dc, (RECT){r.left, r.top + 4, r.left + 4, r.bottom - 4}, OVL_ACCENT);
        ng_text(dc, row_labels[i], (RECT){r.left + 18, r.top, r.left + 250, r.bottom},
                17, active ? FW_BOLD : FW_SEMIBOLD,
                active ? RGB(239, 244, 255) : RGB(175, 190, 210),
                DT_VCENTER | DT_SINGLELINE);
        ng_text(dc, row_hints[i], (RECT){r.left + 240, r.top, r.right - 16, r.bottom},
                13, FW_NORMAL, active ? OVL_ACCENT : RGB(145, 162, 181),
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    ng_fill(dc, (RECT){24, 256, width - 24, 257}, OVL_LINE);
    ng_text(dc,
        tr(L"↑ ↓ Navigate     A / Enter Confirm     B / Select / Esc Resume",
           L"↑ ↓ Navegar     A / Enter Confirmar     B / Select / Esc Voltar"),
        (RECT){24, 270, width - 24, 300}, 13, FW_NORMAL, OVL_ACCENT,
        DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SetMapMode(dc, MM_TEXT);
    BitBlt(dest, 0, 0, pw, ph, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(buf);
    DeleteDC(dc);
}

static LRESULT CALLBACK ng_overlay_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        RECT r; GetClientRect(window, &r);
        ng_overlay_paint(dc, r.right, r.bottom);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT r; GetClientRect(window, &r);
        ng_overlay_paint((HDC)wp, r.right, r.bottom);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_UP || wp == VK_DOWN || wp == 'W' || wp == 'S' || wp == VK_TAB) {
            ng_overlay_row = (ng_overlay_row + 1) % 2;
            InvalidateRect(window, NULL, FALSE);
        } else if (wp == VK_RETURN || wp == VK_SPACE) {
            if (ng_overlay_row == 0) ng_resume_from_select_menu();
            else ng_quit_from_select_menu();
        } else if (wp == VK_ESCAPE || wp == VK_F9 || wp == VK_BACK) {
            ng_resume_from_select_menu();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        RECT r; GetClientRect(window, &r);
        POINT p = {MulDiv(GET_X_LPARAM(lp), 560, max(1, r.right)),
                   MulDiv(GET_Y_LPARAM(lp), 320, max(1, r.bottom))};
        for (int i = 0; i < 2; i++) {
            if (PtInRect(&ng_ovl_hits[i], p)) {
                ng_overlay_row = i;
                if (i == 0) ng_resume_from_select_menu();
                else ng_quit_from_select_menu();
                break;
            }
        }
        return 0;
    }
    case WM_CLOSE:
        ng_resume_from_select_menu();
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

/* ------------------------------------------------------------------ paint */

static void ng_paint(HDC dest, int width, int height) {
    int pw = width, ph = height;
    HDC dc = CreateCompatibleDC(dest);
    HBITMAP buf = CreateCompatibleBitmap(dest, width, height);
    HGDIOBJ old = SelectObject(dc, buf);

    SetMapMode(dc, MM_ANISOTROPIC);
    SetWindowExtEx(dc, 1200, 850, NULL);
    SetViewportExtEx(dc, width, height, NULL);
    width = 1200;
    height = 850;

    ng_count = 0;
    ng_fill(dc, (RECT){0, 0, width, height}, NG_BG);

    /* Arte do caso no cabeçalho */
    if (ng_art) {
        HDC img = CreateCompatibleDC(dc);
        HGDIOBJ bef = SelectObject(img, ng_art);
        SetStretchBltMode(dc, HALFTONE);
        StretchBlt(dc, 0, 0, width, 166, img, 0, 0,
                   ng_art_w, ng_art_h / 2, SRCCOPY);
        SelectObject(img, bef);
        DeleteDC(img);
    }

    /* Cabeçalho */
    ng_fill(dc, (RECT){26, 24, min(width - 185, 680), 141}, RGB(9, 22, 43));
    ng_text(dc, title, (RECT){42, 34, min(width - 190, 665), 85},
            30, FW_BOLD, NG_TEXT, DT_SINGLELINE | DT_END_ELLIPSIS);
    ng_text(dc, L"L E G A C Y   M O B I L E   F O R G E   ·   N - G A G E   2 . 0",
            (RECT){44, 99, min(width - 190, 665), 122}, 12, FW_SEMIBOLD,
            RGB(120, 199, 255), DT_SINGLELINE);

    /* Card do jogo */
    BOOL ok_eka = FALSE, ok_rom = FALSE, ok_ngc = FALSE, ok_game = FALSE, ok_shd = FALSE, ok_bzl = FALSE;
    BOOL ready = check_backend_files(&ok_eka, &ok_rom, &ok_ngc, &ok_game, &ok_shd, &ok_bzl);

    ng_fill(dc, (RECT){32, 182, width - 32, 248}, NG_PANEL);
    ng_fill(dc, (RECT){32, 182, 36, 248}, ready ? NG_BLUE : NG_ERR);
    ng_text(dc, title, (RECT){52, 191, width - 290, 220},
            21, FW_SEMIBOLD, NG_TEXT, DT_SINGLELINE | DT_END_ELLIPSIS);

    WCHAR subtitle[256];
    StringCchPrintfW(subtitle, ARRAYSIZE(subtitle),
        L"N-Gage 2.0  ·  v%s  ·  UID %s  ·  %s  ·  320×240  ·  Select / F9: Menu",
        release_version, game_uid, device_name);
    ng_text(dc, subtitle, (RECT){52, 222, width - 290, 245},
            12, FW_NORMAL, NG_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);

    if (!ready) {
        ng_button(dc, (RECT){width - 266, 196, width - 48, 234},
                  tr(L"Configure / Import", L"Configurar / Importar"),
                  10, 3, FALSE);
    }

    /* Abas */
    const WCHAR *tabs[] = {
        tr(L"QUICK SETUP", L"INÍCIO"),
        tr(L"DISPLAY",     L"IMAGEM"),
        tr(L"CONTROLS",    L"CONTROLES"),
        tr(L"SYSTEM",      L"SISTEMA")
    };
    for (int i = 0; i < 4; i++)
        ng_button(dc, (RECT){32 + i * 148, 264, 176 + i * 148, 302},
                  tabs[i], 10, i, ng_tab == i);

    int y = 325;
    const WCHAR *yesno[] = {tr(L"Off", L"Não"), tr(L"On", L"Sim")};

    if (ng_tab == 0) {
        ng_text(dc, tr(L"Choose how your game looks",
                       L"Escolha o visual do jogo"),
                (RECT){36, y, width - 36, y + 30}, 23, FW_SEMIBOLD, NG_TEXT, DT_SINGLELINE);
        ng_text(dc, tr(L"Original game: 320×240 (N-Gage 2.0). Filter applies to game framebuffer before the custom border.",
                       L"Resolução original: 320×240 (N-Gage 2.0). O filtro atua apenas no jogo, antes da moldura ilustrada."),
                (RECT){36, y + 37, width - 36, y + 65}, 14, FW_NORMAL, NG_MUTED,
                DT_SINGLELINE | DT_END_ELLIPSIS);

        const WCHAR *names[]   = {
            tr(L"SHARP PIXELS", L"PIXELS NÍTIDOS"),
            tr(L"SMOOTH",       L"SUAVE"),
            L"SCALE2X"
        };
        const WCHAR *details[] = {
            tr(L"Original crisp nearest-neighbor pixels",
               L"Pixels originais nítidos (nearest-neighbor)"),
            tr(L"Soft bilinear texture filtering",
               L"Filtragem bilinear suave"),
            tr(L"Edge-aware 2× pixel filter (GLSL pre-bezel)",
               L"Filtro de contornos 2× no framebuffer (antes da moldura)")
        };
        int card = (width - 88) / 3;
        for (int i = 0; i < 3; i++) {
            RECT r = {36 + i * (card + 8), y + 89, 36 + i * (card + 8) + card, y + 189};
            ng_button(dc, r, L"", 20, i, ng_settings.filter == i);
            ng_text(dc, names[i],
                    (RECT){r.left + 16, r.top + 18, r.right - 10, r.top + 47},
                    18, FW_BOLD, NG_TEXT, DT_SINGLELINE);
            ng_text(dc, details[i],
                    (RECT){r.left + 16, r.top + 58, r.right - 10, r.bottom - 8},
                    13, FW_NORMAL, NG_TEXT, DT_WORDBREAK);
        }
        ng_text(dc, tr(L"Direct launch starts Resident Evil: Degeneration with custom border, save guard, Windows Job Object supervision, and Select / F9 pause menu.",
                       L"Inicialização direta abre Resident Evil: Degeneration com moldura ativa, proteção de saves, Windows Job Object e menu Select / F9 em jogo."),
                (RECT){36, y + 214, width - 36, y + 262}, 14, FW_NORMAL, NG_MUTED, DT_WORDBREAK);

    } else if (ng_tab == 1) {
        const WCHAR *res[]     = {L"960 x 540", L"1080p", L"1440p", L"3K", L"4K"};
        const WCHAR *modes[]   = {tr(L"Window", L"Janela"), tr(L"Fullscreen", L"Tela cheia")};
        const WCHAR *filters[] = {tr(L"Sharp", L"Nítido"), tr(L"Smooth", L"Suave"), L"Scale2x"};

        ng_row(dc, width, y,
               tr(L"Resolution", L"Resolução"),
               tr(L"Host window size (clamped to desktop work area).",
                  L"Tamanho da janela no Windows (limitado à área útil do monitor)."),
               res, 5, ng_settings.resolution, 21);
        ng_row(dc, width, y + 74,
               tr(L"Display mode", L"Modo de exibição"),
               tr(L"Windowed or fullscreen presentation (--fullscreen).",
                  L"Apresentação em janela ou tela cheia (--fullscreen)."),
               modes, 2, ng_settings.fullscreen, 22);
        ng_row(dc, width, y + 148,
               tr(L"Image filter", L"Filtro de imagem"),
               tr(L"Applied to the 320×240 framebuffer before compositing the border.",
                  L"Aplicado ao framebuffer 320×240 antes da composição da moldura."),
               filters, 3, ng_settings.filter, 20);
        ng_row(dc, width, y + 222,
               tr(L"Integer scaling", L"Escala inteira"),
               tr(L"On: whole pixel multiples in EKA2L1. Off: proportional fit.",
                  L"Sim: múltiplos inteiros no EKA2L1. Não: ajuste proporcional."),
               yesno, 2, ng_settings.integer, 23);
        ng_row(dc, width, y + 296,
               tr(L"Illustrated border", L"Moldura ilustrada"),
               tr(L"Composes moldura 1.png (1672×941) around the 4:3 game viewport.",
                  L"Compõe a moldura personalizada (1672×941) ao redor da tela 4:3 do jogo."),
               yesno, 2, ng_settings.bezel, 24);

    } else if (ng_tab == 2) {
        const WCHAR *profiles[] = {
            tr(L"Standard (Default)", L"Padrão (Validado)"),
            tr(L"Dual-Stick (Full)", L"Dual-Stick (Completo)")
        };
        ng_row(dc, width, y,
               tr(L"Controller profile", L"Perfil de controle"),
               ng_settings.control_profile == 1
                   ? tr(L"resident_evil_rstick: Left Stick = 4-way Move/Turn/Menus; Right Stick = Turn L/R & 4-way Aim + R3 Zoom.",
                        L"resident_evil_rstick: Esq. = 4 direções (andar, curvar, menus); Dir. = Rotação E/D, Mira 4 dir. + R3 Zoom.")
                   : tr(L"resident_evil (default): Preserves validated Keyboard + D-Pad + Left Stick layout.",
                        L"resident_evil (padrão): Mantém o mapeamento clássico validado Teclado + D-Pad + Analógico Esq."),
               profiles, 2, ng_settings.control_profile, 26);

        const WCHAR *labels[] = {
            tr(L"Move / Menu navigation",           L"Mover Leon / Navegar nos menus"),
            tr(L"Turn / Laser-sight & Sniper Aim",  L"Rotação / Mira laser e Sniper"),
            tr(L"Confirm / Aim / Fire & Knife",     L"Confirmar / Mirar / Atirar / Faca"),
            tr(L"In-game Select Menu (Pause/Quit)", L"Menu Select em Jogo (Resume / Quit)"),
            tr(L"Reload & Shortcuts",               L"Recarregar & Atalhos"),
        };
        const WCHAR *keys[] = {
            ng_settings.control_profile == 1
                ? tr(L"Arrows or WASD  |  Left Stick (4-way Move/Turn/Menu) & D-Pad",
                     L"Setas ou WASD  |  Analógico Esq. (4 direções: andar/curvar/menus) e D-Pad")
                : tr(L"Arrows or WASD  |  D-Pad / Left Stick",
                     L"Setas ou WASD  |  Direcional / Analógico Esquerdo"),
            ng_settings.control_profile == 1
                ? tr(L"Right Stick (Turn L/R & 4-way Aim) + Left Stick L/R + R3 ('3' Zoom)",
                     L"Analógico Dir. (Girar E/D + Mira 4 dir.) + Esq. L/R + R3 ('3' Zoom)")
                : tr(L"Left Stick or D-Pad (original N-Gage 2.0 digital turn/aim)",
                     L"Analógico Esquerdo ou D-Pad (rotação/mira digital original)"),
            tr(L"Enter/Space (Aim/Fire) · F2/Q (Knife)  |  A/RT (Fire) · B/LB (Knife)",
               L"Enter/Espaço (Mirar/Atirar) · F2/Q (Faca)  |  A/RT · B/LB"),
            tr(L"Gamepad Select (Back) or F9 — pauses EKA2L1 (Resume / Quit)",
               L"Botão Select (Back) no controle ou F9 — pausa em Resume / Quit"),
            tr(L"F1/Tab (Pause) · R / X / L3 (Reload) · 0-9, *",
               L"F1/Tab (Pausa) · R / X / L3 (Recarregar) · 0-9, *"),
        };
        for (int i = 0; i < 5; i++) {
            ng_text(dc, labels[i], (RECT){36, y + 78 + i * 44, width / 2 - 8, y + 102 + i * 44},
                    14, FW_SEMIBOLD, NG_TEXT, DT_SINGLELINE | DT_END_ELLIPSIS);
            ng_text(dc, keys[i], (RECT){width / 2, y + 78 + i * 44, width - 36, y + 108 + i * 44},
                    13, FW_NORMAL, NG_MUTED, DT_WORDBREAK);
        }
        ng_text(dc,
            tr(L"Note: Original N-Gage 2.0 engine uses tank controls (no strafe) with 4 directional slots (16/17/14/15).",
               L"Nota: O jogo original usa tank controls (sem strafe lateral) com 4 direções compartilhadas entre giro, mira e menus."),
            (RECT){36, y + 306, width - 36, y + 348}, 13, FW_NORMAL, NG_MUTED, DT_WORDBREAK);

    } else {
        const WCHAR *langs[] = {tr(L"Windows", L"Windows"), L"English", L"Português (BR)"};
        ng_row(dc, width, y,
               tr(L"Language", L"Idioma"),
               tr(L"Launcher & Select Menu only. Original game text is unchanged.",
                  L"Interface do launcher e Menu Select. O idioma do jogo é ajustado em Options."),
               langs, 3, ng_settings.language, 25);

        WCHAR line[512];
        StringCchPrintfW(line, ARRAYSIZE(line), L"Runtime EKA2L1: %s   |   Data: %s", eka2l1_exe, active_data_dir);
        ng_text(dc, line, (RECT){36, y + 76, width - 36, y + 98},
                12, FW_SEMIBOLD, NG_TEXT, DT_SINGLELINE | DT_END_ELLIPSIS);

        StringCchPrintfW(line, ARRAYSIZE(line),
            tr(L"Emulator: %s  ·  Firmware (%s): %s  ·  N-Gage 2.0: %s  ·  Game (%s): %s",
               L"Emulador: %s  ·  Firmware (%s): %s  ·  N-Gage 2.0: %s  ·  Jogo (%s): %s"),
            ok_eka ? L"OK" : L"MISSING",
            device_name, ok_rom ? L"OK" : L"MISSING",
            ok_ngc ? L"OK" : L"MISSING",
            game_uid, ok_game ? L"OK" : L"MISSING");
        ng_text(dc, line, (RECT){36, y + 103, width - 36, y + 127},
                14, FW_SEMIBOLD, ready ? NG_OK : NG_ERR, DT_SINGLELINE | DT_END_ELLIPSIS);

        StringCchPrintfW(line, ARRAYSIZE(line),
            tr(L"Custom border (bezel.png): %s   ·   Scale2x GLSL shader: %s   ·   ReShade: Disabled",
               L"Moldura (bezel.png): %s   ·   Shader GLSL Scale2x: %s   ·   ReShade: Desativado"),
            ok_bzl ? L"OK" : L"MISSING",
            ok_shd ? L"OK" : L"MISSING");
        ng_text(dc, line, (RECT){36, y + 132, width - 36, y + 154},
                13, FW_NORMAL, (ok_bzl && ok_shd) ? NG_MUTED : NG_ERR, DT_SINGLELINE | DT_END_ELLIPSIS);

        ng_text(dc,
            tr(L"Quick Setup: choose Import Full Folder (point to a folder with SYM.ROM, SYM.RPKG, ngage_client.sis and .n-gage) or Select Files (Ctrl+A) to import everything in one go with real-time progress.",
               L"Importação rápida: clique em Importar Pasta Completa (pasta com SYM.ROM, SYM.RPKG, ngage_client.sis e .n-gage) ou Selecionar Arquivos (Ctrl+A) para importar tudo de uma vez sem travamentos."),
            (RECT){36, y + 162, width - 36, y + 206},
            13, FW_NORMAL, NG_MUTED, DT_WORDBREAK);

        if (InterlockedCompareExchange(&ng_importing, 0, 0) != 0) {
            int pct = (int)InterlockedCompareExchange(&ng_prog_pct, 0, 0);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            WCHAR step_txt[256] = {0}, pct_txt[32] = {0};
            ng_get_progress_text(step_txt, ARRAYSIZE(step_txt));
            StringCchPrintfW(pct_txt, ARRAYSIZE(pct_txt), L"%d%%", pct);

            ng_fill(dc, (RECT){36, y + 214, width - 36, y + 306}, NG_PANEL);
            ng_outline(dc, (RECT){36, y + 214, width - 36, y + 306}, NG_LINE, 1);
            ng_text(dc, *step_txt ? step_txt : tr(L"Importing...", L"Importando..."),
                    (RECT){52, y + 224, width - 130, y + 248},
                    14, FW_SEMIBOLD, RGB(120, 199, 255), DT_SINGLELINE | DT_END_ELLIPSIS);
            ng_text(dc, pct_txt, (RECT){width - 125, y + 224, width - 52, y + 248},
                    14, FW_BOLD, NG_TEXT, DT_RIGHT | DT_SINGLELINE);

            RECT bar = {52, y + 256, width - 52, y + 280};
            ng_fill(dc, bar, NG_BG);
            ng_outline(dc, bar, NG_LINE, 1);
            int inner_w = (bar.right - bar.left) - 2;
            int fill_w = (inner_w * pct) / 100;
            if (fill_w > 0) {
                ng_fill(dc, (RECT){bar.left + 1, bar.top + 1, bar.left + 1 + fill_w, bar.bottom - 1}, NG_BLUE);
                ng_fill(dc, (RECT){bar.left + 1, bar.top + 1, bar.left + 1 + fill_w, bar.top + 3}, RGB(120, 199, 255));
            }
            ng_text(dc,
                tr(L"Working in background — window stays responsive.",
                   L"Executando em segundo plano — a interface permanece responsiva."),
                (RECT){52, y + 284, width - 52, y + 303},
                12, FW_NORMAL, NG_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            const WCHAR *primary_labels[] = {
                tr(L"Import Full Folder",      L"Importar Pasta Completa"),
                tr(L"Select Files (Ctrl+A)",   L"Selecionar Arquivos (Ctrl+A)"),
                tr(L"Link Existing EKA2L1",    L"Vincular EKA2L1 Existente")
            };
            const int primary_actions[] = {108, 109, 107};
            for (int i = 0; i < 3; i++) {
                int bx = 36 + i * 379;
                ng_button(dc, (RECT){bx, y + 216, bx + 370, y + 258},
                          primary_labels[i], primary_actions[i], 0, (!ready && i == 0));
            }

            const WCHAR *adv_labels[] = {
                tr(L"Firmware (ROM/RPKG)", L"Firmware (ROM/RPKG)"),
                tr(L"N-Gage Client (.sis)", L"Cliente N-Gage (.sis)"),
                tr(L"Game (.n-gage)", L"Jogo (.n-gage)"),
                tr(L"Open logs folder", L"Abrir pasta de logs")
            };
            const int adv_actions[] = {104, 105, 106, 102};
            for (int i = 0; i < 4; i++) {
                int bx = 36 + i * 284;
                ng_button(dc, (RECT){bx, y + 268, bx + 275, y + 304},
                          adv_labels[i], adv_actions[i], 0, FALSE);
            }
        }
    }

    /* Rodapé */
    BOOL import_busy = (InterlockedCompareExchange(&ng_importing, 0, 0) != 0);
    int footer = height - 110;
    ng_fill(dc, (RECT){0, footer, width, height}, NG_PANEL);
    ng_fill(dc, (RECT){0, footer, width, footer + 1}, NG_LINE);

    WCHAR footer_title[256] = {0};
    if (import_busy) {
        WCHAR step_txt[220] = {0};
        ng_get_progress_text(step_txt, ARRAYSIZE(step_txt));
        int pct = (int)InterlockedCompareExchange(&ng_prog_pct, 0, 0);
        StringCchPrintfW(footer_title, ARRAYSIZE(footer_title), L"[%d%%] %s",
                         pct, *step_txt ? step_txt : tr(L"Importing...", L"Importando..."));
    } else if (*ng_notice) {
        StringCchCopyW(footer_title, ARRAYSIZE(footer_title), ng_notice);
    } else if (ready) {
        StringCchCopyW(footer_title, ARRAYSIZE(footer_title),
            tr(L"Ready — Direct Launch via EKA2L1 N-Gage 2.0",
               L"Pronto — Inicialização Direta via EKA2L1 N-Gage 2.0"));
    } else {
        StringCchCopyW(footer_title, ARRAYSIZE(footer_title),
            tr(L"Setup required: use Import Full Folder or Select Files under System",
               L"Configuração necessária: importe tudo de uma vez na aba Sistema"));
    }

    ng_text(dc, footer_title,
            (RECT){36, footer + 18, width - 270, footer + 46},
            16, FW_SEMIBOLD, (import_busy || *ng_notice || ready) ? NG_TEXT : NG_ERR,
            DT_SINGLELINE | DT_END_ELLIPSIS);
    ng_text(dc,
        tr(L"Saved in app/video.properties  ·  Saves mirrored in app/saves_backup  ·  ReShade disabled",
           L"Preferências em app/video.properties  ·  Saves protegidos em app/saves_backup  ·  ReShade desativado"),
        (RECT){36, footer + 50, width - 270, footer + 74},
        12, FW_NORMAL, NG_MUTED, DT_SINGLELINE);
    ng_button(dc, (RECT){width - 246, footer + 17, width - 36, footer + 72},
              import_busy ? tr(L"WORKING...", L"AGUARDE...")
                          : (ready ? tr(L"PLAY", L"JOGAR") : tr(L"SETUP", L"CONFIGURAR")),
              103, 0, !import_busy);
    ng_text(dc,
        tr(L"A / Enter: select     Arrows: navigate     LB / RB or Q / E: category     Start: play     Select / F9 (in-game): menu",
           L"A / Enter: selecionar     Setas: navegar     LB / RB ou Q / E: abas     Start: jogar     Select / F9 (no jogo): menu"),
        (RECT){36, height - 29, width - 36, height - 7},
        12, FW_NORMAL, NG_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);

    SetMapMode(dc, MM_TEXT);
    BitBlt(dest, 0, 0, pw, ph, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(buf);
    DeleteDC(dc);
}

/* ---------------------------------------------------------------- actions */

static DWORD WINAPI ng_game_thread(void *unused) {
    (void)unused;
    DWORD code = launch_eka2l1();
    PostMessageW(ng_window, UI_DONE_NG, code, 0);
    return code;
}

static void ng_wait_thread_pumping(HANDLE *ph) {
    if (!ph || !*ph) return;
    ULONGLONG deadline = GetTickCount64() + 7000ULL;
    for (;;) {
        DWORD wr = MsgWaitForMultipleObjects(1, ph, FALSE, 100, QS_ALLINPUT);
        if (wr == WAIT_OBJECT_0) break;
        if (wr == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) break;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (GetTickCount64() >= deadline) break;
    }
    CloseHandle(*ph);
    *ph = NULL;
}

static void ng_stop_worker_if_running(void) {
    if (ng_overlay_window) ShowWindow(ng_overlay_window, SW_HIDE);
    ng_overlay_open = FALSE;
    if (ng_import_worker) {
        if (ng_stop_event) SetEvent(ng_stop_event);
        ng_wait_thread_pumping(&ng_import_worker);
        InterlockedExchange(&ng_importing, 0);
    }
    if (!ng_running || !ng_worker) return;
    if (ng_emu_paused) {
        char resp[256];
        DWORD status = 0;
        http_local_ex(active_control_port, 600, L"POST", L"/v1/emulator/resume",
                      resp, sizeof(resp), &status);
        ng_emu_paused = FALSE;
    }
    if (ng_stop_event) SetEvent(ng_stop_event);
    ng_wait_thread_pumping(&ng_worker);
    ng_running = FALSE;
}

typedef struct {
    int action;
    WCHAR p1[PATH_CAP];
    WCHAR p2[PATH_CAP];
    NgImportBundle bundle;
} NgImportJob;

static DWORD WINAPI ng_import_thread(void *param) {
    NgImportJob *job = (NgImportJob *)param;
    if (!job) return 1;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    BOOL ok = FALSE;
    WCHAR result_msg[256] = {0};

    if (job->action == 108) {
        DWORD expected_uid = (DWORD)wcstoul(game_uid, NULL, 0);
        if (!expected_uid) expected_uid = 0x2001AD82UL;
        ZeroMemory(&job->bundle, sizeof(job->bundle));
        ng_report_progress(2, tr(L"Scanning selected folder...", L"Analisando pasta selecionada..."));
        scan_folder_for_bundle(job->p1, 0, expected_uid, &job->bundle);
        ok = execute_import_bundle(&job->bundle, result_msg, ARRAYSIZE(result_msg));
    } else if (job->action == 109) {
        ok = execute_import_bundle(&job->bundle, result_msg, ARRAYSIZE(result_msg));
    } else if (job->action == 107) {
        ok = import_firmware_source(job->p1, NULL, NULL);
        StringCchCopyW(result_msg, ARRAYSIZE(result_msg), ok
            ? tr(L"Linked existing EKA2L1 RM-409 data folder.",
                 L"Pasta EKA2L1 RM-409 existente vinculada com sucesso.")
            : tr(L"Selected folder has no valid RM-409 firmware.",
                 L"A pasta selecionada não contém firmware RM-409 válido."));
        ng_report_progress(100, result_msg);
    } else if (job->action == 104) {
        ok = import_firmware_source(job->p1, job->p2[0] ? job->p2 : NULL, NULL);
        StringCchCopyW(result_msg, ARRAYSIZE(result_msg), ok
            ? tr(L"RM-409 firmware imported cleanly.",
                 L"Firmware RM-409 importado com sucesso.")
            : tr(L"Firmware import failed. Need valid SYM.ROM + SYM.RPKG.",
                 L"Falha ao importar firmware (verifique SYM.ROM + SYM.RPKG)."));
        ng_report_progress(100, result_msg);
    } else if (job->action == 105) {
        ng_report_progress(60, tr(L"Installing N-Gage 2.0 client...",
                                  L"Instalando cliente N-Gage 2.0..."));
        ok = import_ngage_client_sis(job->p1);
        StringCchCopyW(result_msg, ARRAYSIZE(result_msg), ok
            ? tr(L"N-Gage 2.0 runtime installed cleanly.",
                 L"Cliente N-Gage 2.0 instalado com sucesso.")
            : tr(L"Invalid N-Gage installer or firmware missing.",
                 L"Instalador N-Gage inválido ou firmware ausente."));
        ng_report_progress(100, result_msg);
    } else if (job->action == 106) {
        ok = import_game_package(job->p1);
        StringCchCopyW(result_msg, ARRAYSIZE(result_msg), ok
            ? tr(L"Game verified & imported. Ready to play!",
                 L"Jogo verificado e importado. Pronto para jogar!")
            : tr(L"Invalid game package or Firmware/N-Gage missing.",
                 L"Arquivo de jogo inválido ou Firmware/N-Gage ausente."));
        ng_report_progress(100, result_msg);
    }

    AcquireSRWLockExclusive(&ng_prog_lock);
    StringCchCopyW(ng_import_result_notice, ARRAYSIZE(ng_import_result_notice), result_msg);
    ReleaseSRWLockExclusive(&ng_prog_lock);

    free(job);
    CoUninitialize();
    PostMessageW(ng_window, UI_IMPORT_DONE_NG, (WPARAM)ok, 0);
    return ok ? 0 : 1;
}

static void ng_handle_import_action(int action) {
    if (InterlockedCompareExchange(&ng_importing, 0, 0) != 0) return;
    NgImportJob *job = (NgImportJob *)calloc(1, sizeof(NgImportJob));
    if (!job) return;
    job->action = action;
    BOOL launch_job = FALSE;

    if (action == 108) {
        if (SUCCEEDED(pick_path_dialog(ng_window, FALSE, TRUE,
                tr(L"Select folder with Firmware (SYM.ROM/RPKG), N-Gage.sis and .n-gage",
                   L"Selecione a pasta com Firmware (SYM.ROM/RPKG), N-Gage.sis e jogo (.n-gage)"),
                NULL, job->p1, PATH_CAP)) && *job->p1) {
            launch_job = TRUE;
        }
    } else if (action == 109) {
        DWORD sel_count = 0;
        HRESULT hr = pick_multi_files_dialog(ng_window, FALSE,
            tr(L"Select all required files together (Ctrl+A / Ctrl+Click)",
               L"Selecione todos os arquivos necessários de uma vez (Ctrl+A)"),
            &job->bundle, &sel_count);
        if (SUCCEEDED(hr) && sel_count > 0) {
            BOOL any_valid = (job->bundle.rom[0] || job->bundle.rpkg[0] ||
                              job->bundle.ngage_sis[0] || job->bundle.game_pkg[0]);
            if (!any_valid) {
                StringCchCopyW(ng_notice, ARRAYSIZE(ng_notice),
                    tr(L"None of the selected files matched SYM.ROM, SYM.RPKG, N-Gage.sis or Resident Evil (.n-gage).",
                       L"Nenhum arquivo selecionado corresponde a SYM.ROM, SYM.RPKG, ngage_client.sis ou Resident Evil."));
            } else {
                launch_job = TRUE;
            }
        }
    } else if (action == 104) {
        static const COMDLG_FILTERSPEC rom_flt = {L"Nokia 5320 ROM (SYM.ROM)", L"SYM.ROM;*.rom"};
        static const COMDLG_FILTERSPEC rpkg_flt = {L"Symbian RPKG (SYM.RPKG)", L"SYM.RPKG;*.rpkg"};
        if (SUCCEEDED(pick_path_dialog(ng_window, FALSE, FALSE,
                tr(L"Select SYM.ROM (Nokia 5320 RM-409)", L"Selecione SYM.ROM (Nokia 5320 RM-409)"),
                &rom_flt, job->p1, PATH_CAP)) && *job->p1) {
            WCHAR sib_dir[PATH_CAP], sib_rpkg[PATH_CAP];
            StringCchCopyW(sib_dir, PATH_CAP, job->p1);
            WCHAR *sl = wcsrchr(sib_dir, L'\\');
            if (sl) *sl = 0;
            joined(sib_rpkg, sib_dir, L"SYM.RPKG");
            if (GetFileAttributesW(sib_rpkg) == INVALID_FILE_ATTRIBUTES) {
                pick_path_dialog(ng_window, FALSE, FALSE,
                    tr(L"Select SYM.RPKG (Nokia 5320 RM-409)", L"Selecione SYM.RPKG (Nokia 5320 RM-409)"),
                    &rpkg_flt, job->p2, PATH_CAP);
            }
            launch_job = TRUE;
        }
    } else if (action == 105) {
        static const COMDLG_FILTERSPEC sis_flt = {L"N-Gage 2.0 Installer (*.sis)", L"*.sis;*.sisx"};
        if (SUCCEEDED(pick_path_dialog(ng_window, FALSE, FALSE,
                tr(L"Select N-Gage 2.0 Client SIS", L"Selecione o instalador N-Gage 2.0 (.sis)"),
                &sis_flt, job->p1, PATH_CAP)) && *job->p1) {
            launch_job = TRUE;
        }
    } else if (action == 106) {
        static const COMDLG_FILTERSPEC game_flt = {L"N-Gage 2.0 Game (*.n-gage;*.sis;*.sisx)", L"*.n-gage;*.sis;*.sisx"};
        if (SUCCEEDED(pick_path_dialog(ng_window, FALSE, FALSE,
                tr(L"Select your Resident Evil: Degeneration package",
                   L"Selecione seu arquivo Resident Evil Degeneration (.n-gage)"),
                &game_flt, job->p1, PATH_CAP)) && *job->p1) {
            launch_job = TRUE;
        }
    } else if (action == 107) {
        if (SUCCEEDED(pick_path_dialog(ng_window, FALSE, TRUE,
                tr(L"Select existing EKA2L1 or data folder (RM-409)",
                   L"Selecione uma pasta EKA2L1 ou data existente (RM-409)"),
                NULL, job->p1, PATH_CAP)) && *job->p1) {
            launch_job = TRUE;
        }
    }

    if (!launch_job) {
        free(job);
        return;
    }

    if (ng_stop_event) ResetEvent(ng_stop_event);
    ng_notice[0] = 0;
    InterlockedExchange(&ng_importing, 1);
    ng_progress_hwnd = ng_window;
    ng_report_progress(2, tr(L"Starting background task...", L"Iniciando tarefa em segundo plano..."));
    InvalidateRect(ng_window, NULL, FALSE);

    ng_import_worker = CreateThread(NULL, 0, ng_import_thread, job, 0, NULL);
    if (!ng_import_worker) {
        InterlockedExchange(&ng_importing, 0);
        free(job);
        StringCchCopyW(ng_notice, ARRAYSIZE(ng_notice),
            tr(L"Could not start background import thread.",
               L"Não foi possível iniciar a thread de importação."));
    }
}

static void ng_activate(int action, int value) {
    if (ng_running) return;
    BOOL busy = (InterlockedCompareExchange(&ng_importing, 0, 0) != 0);
    switch (action) {
    case 10: ng_tab = value; ng_focus = value; break;
    case 20: ng_settings.filter          = value; ng_save_settings(); break;
    case 21: ng_settings.resolution      = value; ng_save_settings(); break;
    case 22: ng_settings.fullscreen      = value; ng_save_settings(); break;
    case 23: ng_settings.integer         = value; ng_save_settings(); break;
    case 24: ng_settings.bezel           = value; ng_save_settings(); break;
    case 25:
        ng_settings.language = value;
        portuguese = value == 2 || (value == 0 &&
            PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE);
        ng_save_settings();
        break;
    case 26: ng_settings.control_profile = value; ng_save_settings(); break;
    case 102:
        make_directory(logs);
        ShellExecuteW(ng_window, L"open", logs, NULL, NULL, SW_SHOWNORMAL);
        break;
    case 104:
    case 105:
    case 106:
    case 107:
    case 108:
    case 109:
        if (!busy) ng_handle_import_action(action);
        break;
    case 103: { /* PLAY / SETUP */
        if (busy) break;
        BOOL ok_e = FALSE, ok_r = FALSE, ok_n = FALSE, ok_g = FALSE;
        if (!check_backend_files(&ok_e, &ok_r, &ok_n, &ok_g, NULL, NULL)) {
            ng_tab = 3;
            ng_focus = 3;
            StringCchCopyW(ng_notice, ARRAYSIZE(ng_notice),
                tr(L"Use Import Full Folder or Select Files under System before playing.",
                   L"Use Importar Pasta Completa ou Selecionar Arquivos na aba Sistema."));
            break;
        }
        ng_save_settings();
        ng_running = TRUE;
        ng_worker = CreateThread(NULL, 0, ng_game_thread, NULL, 0, NULL);
        if (ng_worker) {
            ShowWindow(ng_window, SW_HIDE);
            event("ui-ng.play", (DWORD)ng_settings.control_profile);
        } else {
            ng_running = FALSE;
            StringCchCopyW(ng_notice, ARRAYSIZE(ng_notice),
                tr(L"Could not start EKA2L1 worker thread.",
                   L"Não foi possível iniciar a thread do EKA2L1."));
        }
        break;
    }
    }
    InvalidateRect(ng_window, NULL, FALSE);
}

static void ng_move(int delta) {
    if (ng_count) ng_focus = (ng_focus + ng_count + delta) % ng_count;
    InvalidateRect(ng_window, NULL, FALSE);
}

static void ng_category(int delta) {
    ng_tab = (ng_tab + 4 + delta) % 4;
    ng_focus = ng_tab;
    InvalidateRect(ng_window, NULL, FALSE);
}

static void ng_poll_pad(void) {
    if (ng_running) {
        BOOL f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        BOOL f9_pressed = f9_down && !ng_last_f9;
        ng_last_f9 = f9_down;

        HWND fg = GetForegroundWindow();
        DWORD fg_pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fg_pid);
        DWORD eka_pid = (DWORD)InterlockedCompareExchange((volatile LONG *)&ng_eka_pid, 0, 0);
        BOOL active_focus = (fg == ng_overlay_window) || (eka_pid != 0 && fg_pid == eka_pid);

        if (f9_pressed && active_focus) {
            if (!ng_overlay_open) ng_open_select_menu();
            else ng_resume_from_select_menu();
            return;
        }

        if (!ng_get_state || !active_focus) {
            ng_last_buttons = 0;
            ng_last_axis = 0;
            return;
        }

        XINPUT_STATE state = {0};
        BOOL connected = FALSE;
        for (DWORD slot = 0; slot < 4; slot++)
            if (ng_get_state(slot, &state) == ERROR_SUCCESS) { connected = TRUE; break; }
        if (!connected) { ng_last_buttons = 0; ng_last_axis = 0; return; }

        WORD buttons = state.Gamepad.wButtons;
        WORD pressed = (WORD)(buttons & ~ng_last_buttons);
        ng_last_buttons = buttons;

        if (!ng_overlay_open) {
            if (pressed & XINPUT_GAMEPAD_BACK) {
                ng_open_select_menu();
            }
            return;
        }

        if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) {
            ng_resume_from_select_menu();
            return;
        }
        if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START)) {
            if (ng_overlay_row == 0) ng_resume_from_select_menu();
            else ng_quit_from_select_menu();
            return;
        }
        int axis =
            (buttons & (XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT)) ? 1 :
            (buttons & (XINPUT_GAMEPAD_DPAD_UP   | XINPUT_GAMEPAD_DPAD_LEFT))  ? -1 :
            (state.Gamepad.sThumbLY < -16000) ? 1 :
            (state.Gamepad.sThumbLY >  16000) ? -1 : 0;
        ULONGLONG now = GetTickCount64();
        if (axis && (axis != ng_last_axis || now >= ng_repeat_at)) {
            ng_overlay_row = (ng_overlay_row + 1) % 2;
            InvalidateRect(ng_overlay_window, NULL, FALSE);
            ng_repeat_at = now + (axis != ng_last_axis ? 350ULL : 150ULL);
        }
        ng_last_axis = axis;
        return;
    }

    ng_last_f9 = FALSE;
    if (!ng_get_state || GetForegroundWindow() != ng_window) {
        ng_last_buttons = 0;
        ng_last_axis = 0;
        return;
    }
    XINPUT_STATE state = {0};
    BOOL connected = FALSE;
    for (DWORD slot = 0; slot < 4; slot++)
        if (ng_get_state(slot, &state) == ERROR_SUCCESS) { connected = TRUE; break; }
    if (!connected) { ng_last_buttons = 0; ng_last_axis = 0; return; }

    WORD buttons = state.Gamepad.wButtons;
    WORD pressed = (WORD)(buttons & ~ng_last_buttons);
    ng_last_buttons = buttons;

    if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)  ng_category(-1);
    if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) ng_category(1);
    if (pressed & XINPUT_GAMEPAD_START) { ng_activate(103, 0); return; }
    if ((pressed & XINPUT_GAMEPAD_A) && ng_focus < ng_count)
        ng_activate(ng_hits[ng_focus].action, ng_hits[ng_focus].value);
    if (pressed & XINPUT_GAMEPAD_B) ng_category(-1);

    int axis =
        (buttons & (XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN)) ? 1 :
        (buttons & (XINPUT_GAMEPAD_DPAD_LEFT  | XINPUT_GAMEPAD_DPAD_UP))   ? -1 :
        (state.Gamepad.sThumbLX >  16000 || state.Gamepad.sThumbLY < -16000) ?  1 :
        (state.Gamepad.sThumbLX < -16000 || state.Gamepad.sThumbLY >  16000) ? -1 : 0;
    ULONGLONG now = GetTickCount64();
    if (axis && (axis != ng_last_axis || now >= ng_repeat_at)) {
        ng_move(axis);
        ng_repeat_at = now + (axis != ng_last_axis ? 350ULL : 110ULL);
    }
    ng_last_axis = axis;
}

/* ---------------------------------------------------------------- WndProc */

static LRESULT CALLBACK ng_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        RECT r; GetClientRect(window, &r);
        ng_paint(dc, r.right, r.bottom);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT r; GetClientRect(window, &r);
        ng_paint((HDC)wp, r.right, r.bottom);
        return 0;
    }
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize = (POINT){720, 560};
        return 0;
    case WM_SIZE:
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        RECT r; GetClientRect(window, &r);
        POINT p = {MulDiv(GET_X_LPARAM(lp), 1200, max(1, r.right)),
                   MulDiv(GET_Y_LPARAM(lp),  850, max(1, r.bottom))};
        for (int i = 0; i < ng_count; i++)
            if (PtInRect(&ng_hits[i].box, p)) {
                ng_focus = i;
                ng_activate(ng_hits[i].action, ng_hits[i].value);
                break;
            }
        return 0;
    }
    case WM_KEYDOWN:
        if      (wp == VK_LEFT  || wp == VK_UP)   ng_move(-1);
        else if (wp == VK_RIGHT || wp == VK_DOWN ||
                 (wp == VK_TAB && !(GetKeyState(VK_SHIFT) & 0x8000))) ng_move(1);
        else if (wp == VK_TAB)  ng_move(-1);
        else if (wp == 'Q')     ng_category(-1);
        else if (wp == 'E')     ng_category(1);
        else if ((wp == VK_RETURN || wp == VK_SPACE) && ng_focus < ng_count)
            ng_activate(ng_hits[ng_focus].action, ng_hits[ng_focus].value);
        else if (wp == VK_ESCAPE)
            SendMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    case WM_TIMER:
        ng_poll_pad();
        return 0;
    case UI_OPEN_SELECT_NG:
        ng_open_select_menu();
        return 0;
    case UI_CLOSE_SELECT_NG:
        ng_resume_from_select_menu();
        return 0;
    case UI_IMPORT_PROGRESS_NG:
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case UI_IMPORT_DONE_NG:
        if (ng_import_worker) {
            CloseHandle(ng_import_worker);
            ng_import_worker = NULL;
        }
        AcquireSRWLockShared(&ng_prog_lock);
        StringCchCopyW(ng_notice, ARRAYSIZE(ng_notice), ng_import_result_notice);
        ReleaseSRWLockShared(&ng_prog_lock);
        InterlockedExchange(&ng_importing, 0);
        ng_read_settings();
        resolve_active_data_dir();
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case UI_DONE_NG:
        if (ng_overlay_open && ng_overlay_window) {
            ng_overlay_open = FALSE;
            ShowWindow(ng_overlay_window, SW_HIDE);
        }
        ng_emu_paused = FALSE;
        if (ng_worker) { CloseHandle(ng_worker); ng_worker = NULL; }
        ng_running = FALSE;
        ng_last_buttons = 0;
        ng_last_f9 = FALSE;
        ng_read_settings();
        resolve_active_data_dir();
        if (ng_direct_mode) {
            ng_direct_exit_code = (DWORD)wp;
            event("ui-ng.direct_return", ng_direct_exit_code);
            DestroyWindow(window);
            return 0;
        }
        StringCchCopyW(ng_notice, ARRAYSIZE(ng_notice),
            wp ? tr(L"EKA2L1 closed with an error. Check local logs.",
                    L"EKA2L1 encerrou com código de erro. Consulte os logs locais.")
               : tr(L"Game closed & saves persisted. Ready to play again.",
                    L"Jogo encerrado e saves salvos. Pronto para jogar novamente."));
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        InvalidateRect(window, NULL, FALSE);
        event("ui-ng.return", (DWORD)wp);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            ng_stop_worker_if_running();
            ng_save_settings();
        }
        return 0;
    case WM_CLOSE:
        ng_stop_worker_if_running();
        ng_save_settings();
        if (ng_overlay_window) {
            DestroyWindow(ng_overlay_window);
            ng_overlay_window = NULL;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        ng_stop_worker_if_running();
        if (ng_overlay_window) {
            DestroyWindow(ng_overlay_window);
            ng_overlay_window = NULL;
        }
        KillTimer(window, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

/* ---------------------------------------------------------------- run_ui */

static DWORD run_launcher_ui_ng(HINSTANCE instance, BOOL direct) {
    ng_direct_mode = direct;
    ng_direct_exit_code = 0;
    ng_read_settings();
    resolve_active_data_dir();

    BOOL ok_e = FALSE, ok_r = FALSE, ok_n = FALSE, ok_g = FALSE;
    BOOL ready = check_backend_files(&ok_e, &ok_r, &ok_n, &ok_g, NULL, NULL);
    if (direct && !ready) {
        event("preflight.missing", ERROR_FILE_NOT_FOUND);
        return 1;
    }

    ng_tab = ready ? 0 : 3;
    ng_focus = ng_tab;
    if (!direct) ng_load_art();

    ng_xinput = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ng_xinput)
        ng_xinput = LoadLibraryExW(L"xinput9_1_0.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ng_xinput)
        ng_get_state = (NgGetState)(void *)GetProcAddress(ng_xinput, "XInputGetState");

    WNDCLASSW cls = {0};
    cls.lpfnWndProc   = ng_proc;
    cls.hInstance     = instance;
    cls.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    cls.lpszClassName = L"LegacyMobileForgeNGLauncher";
    cls.hIcon         = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassW(&cls);

    WNDCLASSW ovl_cls = {0};
    ovl_cls.lpfnWndProc   = ng_overlay_proc;
    ovl_cls.hInstance     = instance;
    ovl_cls.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    ovl_cls.lpszClassName = L"LegacyMobileForgeNGSelectMenu";
    ovl_cls.hIcon         = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassW(&ovl_cls);

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = min(1200, work.right  - work.left);
    int h = min(900,  work.bottom - work.top);

    ng_window = CreateWindowExW(0, cls.lpszClassName, title,
        WS_OVERLAPPEDWINDOW,
        work.left + (work.right  - work.left - w) / 2,
        work.top  + (work.bottom - work.top  - h) / 2,
        w, h, NULL, NULL, instance, NULL);
    if (!ng_window) {
        event("ui-ng.failed", GetLastError());
        if (ng_art) DeleteObject(ng_art);
        if (ng_xinput) FreeLibrary(ng_xinput);
        return 1;
    }

    ng_overlay_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        ovl_cls.lpszClassName, L"Select Menu",
        WS_POPUP, 0, 0, 560, 320, NULL, NULL, instance, NULL);
    if (ng_overlay_window) {
        SetLayeredWindowAttributes(ng_overlay_window, 0, 246, LWA_ALPHA);
        BOOL dark_ovl = TRUE;
        DwmSetWindowAttribute(ng_overlay_window, 20, &dark_ovl, sizeof(dark_ovl));
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(ng_window, 20, &dark, sizeof(dark));
    SetTimer(ng_window, 1, 30, NULL);

    if (direct) {
        ng_activate(103, 0);
        if (!ng_running) {
            DestroyWindow(ng_window);
            if (ng_xinput) { FreeLibrary(ng_xinput); ng_xinput = NULL; }
            return 1;
        }
    } else {
        ShowWindow(ng_window, SW_SHOW);
        UpdateWindow(ng_window);
        event("ui-ng.open", 0);
    }

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (ng_art)    { DeleteObject(ng_art); ng_art = NULL; }
    if (ng_xinput) { FreeLibrary(ng_xinput); ng_xinput = NULL; }
    return direct ? ng_direct_exit_code : 0;
}
