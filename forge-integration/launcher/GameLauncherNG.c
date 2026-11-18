/*
 * GameLauncherNG.c — Launcher nativo C17 para backend EKA2L1 (N-Gage 2.0)
 *
 * Reaproveita a arquitetura de GameLauncher.c (LegacyMobileForge/engine/native/GameLauncher.c):
 *   - Perfil INI (app/launcher-ng.ini) e preferências compartilhadas (app/video.properties)
 *   - Caminhos portáteis relativos em runtime e detecção não-destrutiva de firmware existente
 *   - Importação nativa C17 (IFileOpenDialog / CLI) de firmware (SYM.ROM + SYM.RPKG),
 *     cliente N-Gage 2.0 (.sis) e jogo (.n-gage multipart ou .sis/.sisx) sem downloads externos
 *   - Rotação de logs locais (app/logs/launcher-ng.log)
 *   - Sincronização atômica de moldura (bezel.png), filtro (Sharp / Smooth / Scale2x),
 *     escala inteira e perfil nativo de controles SDL2/YAML (bindings/resident_evil.yml padrão,
 *     bindings/resident_evil_rstick.yml opcional com separação Esquerdo=Movimento / Direito=Rotação+Mira)
 *   - Inicialização direta sem navegação manual via Control API local do EKA2L1
 *   - Menu Select em jogo (Resume Game / Quit Game) com pausa/retomada via /v1/emulator/pause|resume
 *   - Supervisão determinística via Windows Job Objects (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) +
 *     encerramento gracioso em 3 estágios + backup atômico e autorreparo de saves
 *   - ReShade mantido desativado conforme diretriz do caso
 *
 * Build (MSVC C17 /MT /W4 /WX):
 *   cl /nologo /TC /std:c17 /utf-8 /O2 /MT /W4 /WX GameLauncherNG.c ...
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <bcrypt.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define PATH_CAP 4096
#define MAX_IMPORT_BYTES (128ULL * 1024ULL * 1024ULL)

typedef struct {
    int resolution;       /* 0=540p, 1=1080p, 2=1440p, 3=3K, 4=4K */
    int filter;           /* 0=sharp, 1=smooth, 2=scale2x */
    int integer;          /* 0=off, 1=on */
    int fullscreen;       /* 0=window, 1=fullscreen */
    int bezel;            /* 0=off, 1=on */
    int language;         /* 0=auto, 1=en, 2=pt-BR */
    int control_profile;  /* 0=resident_evil (padrão validado), 1=resident_evil_rstick (opcional) */
} NgSettings;

static WCHAR root[PATH_CAP];
static WCHAR app[PATH_CAP];
static WCHAR logs[PATH_CAP];
static WCHAR saves_backup_dir[PATH_CAP];
static WCHAR title[256];
static WCHAR release_version[32];
static WCHAR eka2l1_exe[PATH_CAP];
static WCHAR eka2l1_dir[PATH_CAP];
static WCHAR active_data_dir[PATH_CAP];
static WCHAR saved_data_dir[PATH_CAP];
static WCHAR bezel_file[PATH_CAP];
static WCHAR device_name[128];
static WCHAR game_uid[64];
static WCHAR keybind_profile[128];
static INTERNET_PORT configured_control_port = 28182;
static INTERNET_PORT active_control_port = 28182;
static int bezel_scale_ratio = 67;
static BOOL portuguese;
static BOOL g_no_auto_detect = FALSE;
static BOOL ng_direct_mode = FALSE;
static NgSettings ng_settings;
static HANDLE ng_stop_event = NULL;
static HANDLE ng_instance_mutex = NULL;

/* Estado compartilhado entre worker threads e janela/overlay UI */
static volatile LONG  ng_game_live = 0;
static volatile LONG  ng_quit_from_select = 0;
static volatile DWORD ng_eka_pid = 0;

#define UI_IMPORT_PROGRESS_NG (WM_APP + 5)
#define UI_IMPORT_DONE_NG     (WM_APP + 6)

static SRWLOCK       ng_prog_lock = SRWLOCK_INIT;
static volatile LONG ng_importing = 0;
static volatile LONG ng_prog_pct = 0;
static WCHAR         ng_prog_step[256];
static HWND          ng_progress_hwnd = NULL;

static const WCHAR *tr(const WCHAR *en, const WCHAR *pt) {
    return portuguese ? pt : en;
}

static void ng_report_progress(int pct, const WCHAR *step) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    InterlockedExchange(&ng_prog_pct, (LONG)pct);
    AcquireSRWLockExclusive(&ng_prog_lock);
    if (step && *step) {
        StringCchCopyW(ng_prog_step, ARRAYSIZE(ng_prog_step), step);
    } else {
        ng_prog_step[0] = 0;
    }
    ReleaseSRWLockExclusive(&ng_prog_lock);
    if (ng_progress_hwnd && IsWindow(ng_progress_hwnd)) {
        PostMessageW(ng_progress_hwnd, UI_IMPORT_PROGRESS_NG, (WPARAM)pct, 0);
    }
}

static void ng_get_progress_text(WCHAR *out, size_t cap) {
    if (!out || !cap) return;
    AcquireSRWLockShared(&ng_prog_lock);
    StringCchCopyW(out, cap, ng_prog_step);
    ReleaseSRWLockShared(&ng_prog_lock);
}

/* ---------------------------------------------------------------------------
 * Utilitários de caminho, diretórios e log rotativo
 * --------------------------------------------------------------------------*/

static BOOL joined(WCHAR *out, const WCHAR *base, const WCHAR *relative) {
    return SUCCEEDED(StringCchPrintfW(out, PATH_CAP, L"%s\\%s", base, relative));
}

static BOOL make_directory(const WCHAR *path) {
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static BOOL make_directories_recursive(const WCHAR *full_path) {
    WCHAR temp[PATH_CAP];
    if (FAILED(StringCchCopyW(temp, PATH_CAP, full_path))) return FALSE;
    for (WCHAR *p = temp; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            if (p > temp && *(p - 1) != L':') {
                WCHAR ch = *p;
                *p = 0;
                make_directory(temp);
                *p = ch;
            }
        }
    }
    return make_directory(temp);
}

static BOOL resolve_path(WCHAR *out, const WCHAR *base, const WCHAR *spec) {
    WCHAR combined[PATH_CAP];
    if (!spec || !*spec) return FALSE;
    if ((spec[0] && spec[1] == L':') || (spec[0] == L'\\' && spec[1] == L'\\')) {
        if (FAILED(StringCchCopyW(combined, PATH_CAP, spec))) return FALSE;
    } else {
        if (!joined(combined, base, spec)) return FALSE;
    }
    for (WCHAR *p = combined; *p; p++)
        if (*p == L'/') *p = L'\\';
    DWORD n = GetFullPathNameW(combined, PATH_CAP, out, NULL);
    return n > 0 && n < PATH_CAP;
}

static void make_relative_or_keep(WCHAR *out, size_t cap,
                                  const WCHAR *from_dir, const WCHAR *target, DWORD target_attr) {
    WCHAR rel[MAX_PATH] = {0};
    if (PathRelativePathToW(rel, from_dir, FILE_ATTRIBUTE_DIRECTORY, target, target_attr) && rel[0]) {
        const WCHAR *src = (!wcsncmp(rel, L".\\", 2)) ? (rel + 2) : rel;
        if (!*src) src = L".";
        StringCchCopyW(out, cap, src);
    } else {
        StringCchCopyW(out, cap, target);
    }
}

static void event(const char *kind, DWORD code) {
    WCHAR file[PATH_CAP], previous[PATH_CAP];
    SYSTEMTIME t;
    HANDLE h;
    DWORD count;
    char line[240];
    LARGE_INTEGER size;
    if (!make_directory(logs) ||
        !joined(file, logs, L"launcher-ng.log") ||
        !joined(previous, logs, L"launcher-ng.previous.log")) return;
    h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (GetFileSizeEx(h, &size) && size.QuadPart > 1024 * 1024) {
            CloseHandle(h);
            MoveFileExW(file, previous, MOVEFILE_REPLACE_EXISTING);
        } else {
            CloseHandle(h);
        }
    }
    h = CreateFileW(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    GetSystemTime(&t);
    int length = sprintf_s(line, sizeof(line),
        "%04u-%02u-%02uT%02u:%02u:%02uZ native-ng.%s code=%lu\r\n",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, kind, code);
    if (length > 0) WriteFile(h, line, (DWORD)length, &count, NULL);
    CloseHandle(h);
}

/* ---------------------------------------------------------------------------
 * Validação de carga útil e persistência de preferências (app/video.properties)
 * --------------------------------------------------------------------------*/

static BOOL file_has_valid_payload(const WCHAR *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER sz = {0};
    BOOL valid = FALSE;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0) {
        BYTE buf[16384];
        DWORD checked = 0;
        DWORD read_n = 0;
        while (checked < 65536 && ReadFile(h, buf, sizeof(buf), &read_n, NULL) && read_n > 0) {
            checked += read_n;
            for (DWORD i = 0; i < read_n; i++) {
                if (buf[i] != 0) {
                    valid = TRUE;
                    break;
                }
            }
            if (valid) break;
        }
        if (!valid && sz.QuadPart > 65536LL) {
            LARGE_INTEGER mid;
            mid.QuadPart = sz.QuadPart / 2;
            if (SetFilePointerEx(h, mid, NULL, FILE_BEGIN) &&
                ReadFile(h, buf, sizeof(buf), &read_n, NULL) && read_n > 0) {
                for (DWORD i = 0; i < read_n; i++) {
                    if (buf[i] != 0) {
                        valid = TRUE;
                        break;
                    }
                }
            }
        }
    }
    CloseHandle(h);
    return valid;
}

static void ng_read_settings(void) {
    WCHAR path[PATH_CAP];
    FILE *f = NULL;
    char line[4096];
    /* Padrão: 1080p, Sharp, Integer On, Janela, Moldura On, Idioma Auto, Controles Padrão (0) */
    ng_settings = (NgSettings){1, 0, 1, 0, 1, 0, 0};
    saved_data_dir[0] = 0;

    if (joined(path, app, L"video.properties") && !_wfopen_s(&f, path, L"rb") && f) {
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char *key = line;
            char *val = eq + 1;
            size_t vlen = strlen(val);
            while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n' ||
                                val[vlen - 1] == ' '  || val[vlen - 1] == '\t')) {
                val[--vlen] = 0;
            }
            while (*val == ' ' || *val == '\t') val++;

            if (!strcmp(key, "resolution")) ng_settings.resolution = atoi(val);
            else if (!strcmp(key, "smooth") && !strcmp(val, "true")) ng_settings.filter = 1;
            else if (!strcmp(key, "pixelArt") && !strcmp(val, "true")) ng_settings.filter = 2;
            else if (!strcmp(key, "integerScale")) ng_settings.integer = !strcmp(val, "true");
            else if (!strcmp(key, "fullscreen")) ng_settings.fullscreen = !strcmp(val, "true");
            else if (!strcmp(key, "bezel")) ng_settings.bezel = !strcmp(val, "true");
            else if (!strcmp(key, "language"))
                ng_settings.language = !strcmp(val, "en") ? 1 : !strcmp(val, "pt-BR") ? 2 : 0;
            else if (!strcmp(key, "controlProfile"))
                ng_settings.control_profile = !strcmp(val, "rstick") ? 1 : 0;
            else if (!strcmp(key, "dataDir") && *val)
                MultiByteToWideChar(CP_UTF8, 0, val, -1, saved_data_dir, PATH_CAP);
        }
        fclose(f);
    }
    if (ng_settings.resolution < 0 || ng_settings.resolution > 4)
        ng_settings.resolution = 1;
    if (ng_settings.control_profile < 0 || ng_settings.control_profile > 1)
        ng_settings.control_profile = 0;
    portuguese = ng_settings.language == 2 ||
        (ng_settings.language == 0 && PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE);
}

static BOOL ng_known_property(const char *line) {
    const char *keys[] = {
        "resolution=", "smooth=", "pixelArt=", "integerScale=",
        "fullscreen=", "bezel=", "language=", "controlProfile=", "dataDir="
    };
    for (int i = 0; i < (int)ARRAYSIZE(keys); i++)
        if (!strncmp(line, keys[i], strlen(keys[i]))) return TRUE;
    return FALSE;
}

static BOOL ng_save_settings(void) {
    WCHAR path[PATH_CAP], tmp[PATH_CAP];
    FILE *input = NULL, *output = NULL;
    char line[4096];
    BOOL ok = FALSE;
    if (!make_directory(app) ||
        !joined(path, app, L"video.properties") ||
        !GetTempFileNameW(app, L"vid", 0, tmp)) return FALSE;
    if (_wfopen_s(&output, tmp, L"wb")) goto done;
    if (!_wfopen_s(&input, path, L"rb") && input) {
        while (fgets(line, sizeof(line), input)) {
            if (!ng_known_property(line)) {
                fputs(line, output);
                size_t l = strlen(line);
                if (l > 0 && line[l - 1] != '\n') fputc('\n', output);
            }
        }
        fclose(input);
    }
    fprintf(output,
        "resolution=%d\nsmooth=%s\npixelArt=%s\nintegerScale=%s\n"
        "fullscreen=%s\nbezel=%s\nlanguage=%s\ncontrolProfile=%s\n",
        ng_settings.resolution,
        ng_settings.filter == 1 ? "true" : "false",
        ng_settings.filter == 2 ? "true" : "false",
        ng_settings.integer ? "true" : "false",
        ng_settings.fullscreen ? "true" : "false",
        ng_settings.bezel ? "true" : "false",
        ng_settings.language == 1 ? "en" : ng_settings.language == 2 ? "pt-BR" : "auto",
        ng_settings.control_profile == 1 ? "rstick" : "default");

    if (*saved_data_dir) {
        WCHAR resolved[PATH_CAP], rel[PATH_CAP];
        if (resolve_path(resolved, root, saved_data_dir)) {
            make_relative_or_keep(rel, PATH_CAP, root, resolved, FILE_ATTRIBUTE_DIRECTORY);
        } else {
            StringCchCopyW(rel, PATH_CAP, saved_data_dir);
        }
        char data_u8[4096] = {0};
        WideCharToMultiByte(CP_UTF8, 0, rel, -1, data_u8, (int)sizeof(data_u8) - 1, NULL, NULL);
        for (char *p = data_u8; *p; p++)
            if (*p == '\\') *p = '/';
        fprintf(output, "dataDir=%s\n", data_u8);
    }

    ok = !ferror(output);
    if (fclose(output)) ok = FALSE;
    output = NULL;
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
done:
    if (output) fclose(output);
    DeleteFileW(tmp);
    event("settings.save", ok ? 0 : GetLastError());
    return ok;
}

/* ---------------------------------------------------------------------------
 * Detecção não-destrutiva do firmware RM-409 e resolução de active_data_dir
 * --------------------------------------------------------------------------*/

static BOOL has_valid_rm409_firmware(const WCHAR *data_dir) {
    if (!data_dir || !*data_dir) return FALSE;
    WCHAR dev_lower[128], rom_p[PATH_CAP], z_bin[PATH_CAP];
    StringCchCopyW(dev_lower, ARRAYSIZE(dev_lower), device_name);
    CharLowerW(dev_lower);

    StringCchPrintfW(rom_p, PATH_CAP, L"%s\\roms\\%s\\SYM.ROM", data_dir, dev_lower);
    StringCchPrintfW(z_bin, PATH_CAP, L"%s\\drives\\z\\%s\\sys\\bin", data_dir, dev_lower);
    DWORD attr_z = GetFileAttributesW(z_bin);
    return file_has_valid_payload(rom_p) &&
           (attr_z != INVALID_FILE_ATTRIBUTES) &&
           ((attr_z & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

static void resolve_active_data_dir(void) {
    WCHAR candidate[PATH_CAP];
    if (*saved_data_dir && resolve_path(candidate, root, saved_data_dir) &&
        has_valid_rm409_firmware(candidate)) {
        StringCchCopyW(active_data_dir, PATH_CAP, candidate);
        return;
    }

    WCHAR default_data[PATH_CAP];
    joined(default_data, eka2l1_dir, L"data");
    if (has_valid_rm409_firmware(default_data)) {
        StringCchCopyW(active_data_dir, PATH_CAP, default_data);
        return;
    }

    if (!g_no_auto_detect) {
        const WCHAR *rel_probes[] = { L"..\\EKA2L1\\data", L"..\\..\\EKA2L1\\data" };
        for (int i = 0; i < (int)ARRAYSIZE(rel_probes); i++) {
            if (resolve_path(candidate, root, rel_probes[i]) &&
                has_valid_rm409_firmware(candidate)) {
                StringCchCopyW(active_data_dir, PATH_CAP, candidate);
                event("firmware.detected_existing", (DWORD)i);
                return;
            }
        }

        const KNOWNFOLDERID *folders[] = {
            &FOLDERID_RoamingAppData, &FOLDERID_LocalAppData, &FOLDERID_Documents
        };
        for (int i = 0; i < (int)ARRAYSIZE(folders); i++) {
            PWSTR kf = NULL;
            if (SUCCEEDED(SHGetKnownFolderPath(folders[i], 0, NULL, &kf)) && kf) {
                if (joined(candidate, kf, L"EKA2L1\\data") &&
                    has_valid_rm409_firmware(candidate)) {
                    StringCchCopyW(active_data_dir, PATH_CAP, candidate);
                    CoTaskMemFree(kf);
                    event("firmware.detected_existing", (DWORD)(10 + i));
                    return;
                }
                CoTaskMemFree(kf);
            }
        }
    }

    StringCchCopyW(active_data_dir, PATH_CAP, default_data);
}

/* ---------------------------------------------------------------------------
 * Leitura do perfil (app/launcher-ng.ini) e verificação de integridade
 * --------------------------------------------------------------------------*/

static BOOL load_profile_ng(void) {
    DWORD count = GetModuleFileNameW(NULL, root, PATH_CAP);
    if (!count || count >= PATH_CAP) return FALSE;
    WCHAR *slash = wcsrchr(root, L'\\');
    if (!slash) return FALSE;
    *slash = 0;

    WCHAR profile[PATH_CAP];
    if (!joined(app, root, L"app") ||
        !joined(profile, app, L"launcher-ng.ini") ||
        !joined(logs, app, L"logs") ||
        !joined(saves_backup_dir, app, L"saves_backup")) return FALSE;
    if (GetFileAttributesW(profile) == INVALID_FILE_ATTRIBUTES) return FALSE;

    GetPrivateProfileStringW(L"launcher-ng", L"title",
        L"Resident Evil: Degeneration (N-Gage 2.0)",
        title, ARRAYSIZE(title), profile);
    GetPrivateProfileStringW(L"launcher-ng", L"version",
        L"0.1.0", release_version, ARRAYSIZE(release_version), profile);

    WCHAR rel_exe[PATH_CAP], rel_art[PATH_CAP], lang[32], port_s[16], ratio_s[16];
    GetPrivateProfileStringW(L"launcher-ng", L"eka2l1_exe",
        L"..\\EKA2L1\\eka2l1_qt.exe", rel_exe, PATH_CAP, profile);
    GetPrivateProfileStringW(L"launcher-ng", L"device",
        L"RM-409", device_name, ARRAYSIZE(device_name), profile);
    GetPrivateProfileStringW(L"launcher-ng", L"game_uid",
        L"0x2001AD82", game_uid, ARRAYSIZE(game_uid), profile);
    GetPrivateProfileStringW(L"launcher-ng", L"keybind_profile",
        L"resident_evil", keybind_profile, ARRAYSIZE(keybind_profile), profile);
    GetPrivateProfileStringW(L"launcher-ng", L"artwork",
        L"assets\\bezel.png", rel_art, PATH_CAP, profile);
    GetPrivateProfileStringW(L"launcher-ng", L"control_port",
        L"28182", port_s, ARRAYSIZE(port_s), profile);
    GetPrivateProfileStringW(L"launcher-ng", L"bezel_scale_ratio",
        L"67", ratio_s, ARRAYSIZE(ratio_s), profile);
    GetPrivateProfileStringW(L"launcher-ng", L"language",
        L"auto", lang, ARRAYSIZE(lang), profile);

    if (!resolve_path(eka2l1_exe, root, rel_exe)) return FALSE;
    if (FAILED(StringCchCopyW(eka2l1_dir, PATH_CAP, eka2l1_exe))) return FALSE;
    WCHAR *exe_slash = wcsrchr(eka2l1_dir, L'\\');
    if (!exe_slash) return FALSE;
    *exe_slash = 0;

    if (!resolve_path(bezel_file, app, rel_art)) bezel_file[0] = 0;

    int p = _wtoi(port_s);
    if (p > 1024 && p < 65535) {
        configured_control_port = (INTERNET_PORT)p;
        active_control_port = configured_control_port;
    }

    int r = _wtoi(ratio_s);
    if (r >= 40 && r <= 100) bezel_scale_ratio = r;

    if (!wcscmp(lang, L"pt-BR")) portuguese = TRUE;
    else if (!wcscmp(lang, L"en")) portuguese = FALSE;
    else portuguese = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE);

    ng_read_settings();
    resolve_active_data_dir();
    return TRUE;
}

static const WCHAR *effective_keybind_profile(void) {
    if (ng_settings.control_profile == 1 && !wcscmp(keybind_profile, L"resident_evil")) {
        WCHAR rstick_yml[PATH_CAP];
        if (joined(rstick_yml, eka2l1_dir, L"bindings\\resident_evil_rstick.yml") &&
            GetFileAttributesW(rstick_yml) != INVALID_FILE_ATTRIBUTES) {
            return L"resident_evil_rstick";
        }
    }
    return keybind_profile;
}

static BOOL check_ngage_client_installed(const WCHAR *data_dir) {
    WCHAR ng_c[PATH_CAP], ng_e[PATH_CAP];
    if (!joined(ng_c, data_dir, L"drives\\c\\sys\\bin\\NgiInstaller.exe") ||
        !joined(ng_e, data_dir, L"drives\\e\\sys\\bin\\NgiInstaller.exe"))
        return FALSE;
    return (GetFileAttributesW(ng_c) != INVALID_FILE_ATTRIBUTES) ||
           (GetFileAttributesW(ng_e) != INVALID_FILE_ATTRIBUTES);
}

static BOOL check_game_installed(const WCHAR *data_dir) {
    WCHAR game_c[PATH_CAP], game_e[PATH_CAP];
    if (!joined(game_c, data_dir, L"drives\\c\\sys\\bin\\Resident_Evil.exe") ||
        !joined(game_e, data_dir, L"drives\\e\\sys\\bin\\Resident_Evil.exe"))
        return FALSE;
    return (GetFileAttributesW(game_c) != INVALID_FILE_ATTRIBUTES) ||
           (GetFileAttributesW(game_e) != INVALID_FILE_ATTRIBUTES);
}

static BOOL check_backend_files(BOOL *out_eka, BOOL *out_rom, BOOL *out_ngage,
                                BOOL *out_game, BOOL *out_shader, BOOL *out_bezel) {
    WCHAR shd_p[PATH_CAP];
    joined(shd_p, eka2l1_dir, L"resources\\upscale\\Scale2x.frag");

    BOOL eka = GetFileAttributesW(eka2l1_exe) != INVALID_FILE_ATTRIBUTES;
    BOOL rom = has_valid_rm409_firmware(active_data_dir);
    BOOL ngc = check_ngage_client_installed(active_data_dir);
    BOOL gm  = check_game_installed(active_data_dir);
    BOOL shd = GetFileAttributesW(shd_p) != INVALID_FILE_ATTRIBUTES;
    BOOL bzl = (*bezel_file != 0) && (GetFileAttributesW(bezel_file) != INVALID_FILE_ATTRIBUTES);

    if (out_eka)    *out_eka    = eka;
    if (out_rom)    *out_rom    = rom;
    if (out_ngage)  *out_ngage  = ngc;
    if (out_game)   *out_game   = gm;
    if (out_shader) *out_shader = shd;
    if (out_bezel)  *out_bezel  = bzl;
    return eka && rom && ngc && gm;
}

/* ---------------------------------------------------------------------------
 * Proteção, flush e backup atômico de saves (PlayServer + Arena + DBMS)
 * --------------------------------------------------------------------------*/

typedef struct {
    const WCHAR *rel_data;
    const WCHAR *backup_name;
} SaveEntrySpec;

static const SaveEntrySpec k_save_specs[] = {
    { L"drives\\c\\private\\20007b39\\2001ad82\\appdata.i3d",  L"appdata.i3d"  },
    { L"drives\\c\\private\\20007b39\\2001ad82\\userdata.i3d", L"userdata.i3d" },
    { L"drives\\c\\private\\20007b39\\DBMS\\myprofile.db",     L"myprofile.db" },
    { L"drives\\c\\private\\2001AD82\\as.dat",                 L"as.dat"       }
};

static BOOL atomic_copy_with_flush(const WCHAR *src, const WCHAR *dst) {
    WCHAR parent[PATH_CAP], tmp[PATH_CAP];
    if (FAILED(StringCchCopyW(parent, PATH_CAP, dst))) return FALSE;
    WCHAR *slash = wcsrchr(parent, L'\\');
    if (!slash) return FALSE;
    *slash = 0;
    if (!make_directories_recursive(parent)) return FALSE;
    if (!GetTempFileNameW(parent, L"sav", 0, tmp)) return FALSE;

    BOOL ok = CopyFileW(src, tmp, FALSE);
    if (ok) {
        HANDLE h = CreateFileW(tmp, GENERIC_WRITE, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(h);
            CloseHandle(h);
        }
        ok = file_has_valid_payload(tmp) &&
             MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    if (!ok) DeleteFileW(tmp);
    return ok;
}

static void preflight_protect_saves(void) {
    make_directories_recursive(saves_backup_dir);
    DWORD restored = 0, seeded = 0;

    for (int i = 0; i < (int)ARRAYSIZE(k_save_specs); i++) {
        WCHAR live_p[PATH_CAP], bak_p[PATH_CAP];
        if (!joined(live_p, active_data_dir, k_save_specs[i].rel_data) ||
            !joined(bak_p, saves_backup_dir, k_save_specs[i].backup_name))
            continue;

        BOOL live_exists = (GetFileAttributesW(live_p) != INVALID_FILE_ATTRIBUTES);
        BOOL live_valid  = live_exists && file_has_valid_payload(live_p);
        BOOL bak_valid   = file_has_valid_payload(bak_p);

        if (live_valid && !bak_valid) {
            if (atomic_copy_with_flush(live_p, bak_p)) seeded++;
        } else if (!live_valid && bak_valid) {
            if (atomic_copy_with_flush(bak_p, live_p)) restored++;
        }
    }
    if (seeded > 0)   event("saves.backup_seeded", seeded);
    if (restored > 0) event("saves.restored_from_backup", restored);
}

static void settle_and_flush_live_saves(void) {
    typedef struct { ULONGLONG size; FILETIME mtime; BOOL present; } SaveSnap;
    SaveSnap prev[ARRAYSIZE(k_save_specs)] = {0};

    for (int pass = 0; pass < 7; pass++) {
        SaveSnap cur[ARRAYSIZE(k_save_specs)] = {0};
        BOOL all_stable = (pass > 0);

        for (int i = 0; i < (int)ARRAYSIZE(k_save_specs); i++) {
            WCHAR live_p[PATH_CAP];
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (joined(live_p, active_data_dir, k_save_specs[i].rel_data) &&
                GetFileAttributesExW(live_p, GetFileExInfoStandard, &fad)) {
                cur[i].present = TRUE;
                cur[i].size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                cur[i].mtime = fad.ftLastWriteTime;
            }
            if (pass > 0) {
                if (cur[i].present != prev[i].present ||
                    cur[i].size != prev[i].size ||
                    CompareFileTime(&cur[i].mtime, &prev[i].mtime) != 0) {
                    all_stable = FALSE;
                }
            }
            prev[i] = cur[i];
        }
        if (all_stable) break;
        Sleep(220);
    }

    for (int i = 0; i < (int)ARRAYSIZE(k_save_specs); i++) {
        WCHAR live_p[PATH_CAP];
        if (!joined(live_p, active_data_dir, k_save_specs[i].rel_data)) continue;
        HANDLE h = CreateFileW(live_p, FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(h);
            CloseHandle(h);
        }
    }
}

static void persist_saves_backup_after_exit(void) {
    make_directories_recursive(saves_backup_dir);
    DWORD persisted = 0;
    for (int i = 0; i < (int)ARRAYSIZE(k_save_specs); i++) {
        WCHAR live_p[PATH_CAP], bak_p[PATH_CAP];
        if (!joined(live_p, active_data_dir, k_save_specs[i].rel_data) ||
            !joined(bak_p, saves_backup_dir, k_save_specs[i].backup_name))
            continue;
        if (file_has_valid_payload(live_p)) {
            if (atomic_copy_with_flush(live_p, bak_p)) persisted++;
        }
    }
    event("saves.persisted", persisted);
}

/* ---------------------------------------------------------------------------
 * Sincronização atômica e portátil de config.yml, devices.yml e compat/*.yml
 * --------------------------------------------------------------------------*/

static void ensure_devices_yml_intact(void) {
    WCHAR dev_yml[PATH_CAP];
    if (!joined(dev_yml, active_data_dir, L"devices.yml")) return;
    if (file_has_valid_payload(dev_yml)) return;

    WCHAR tmp[PATH_CAP];
    if (!make_directories_recursive(active_data_dir)) return;
    if (!GetTempFileNameW(active_data_dir, L"dvc", 0, tmp)) return;

    FILE *f = NULL;
    if (!_wfopen_s(&f, tmp, L"wb") && f) {
        fprintf(f,
            "RM-409:\n"
            "  platver: epoc93fp2\n"
            "  manufacturer: Nokia\n"
            "  firmcode: RM-409\n"
            "  model: Nokia 5320d-1\n"
            "  machine-uid: 536926810\n"
            "  isolated-drives: false\n");
        BOOL ok = !ferror(f);
        if (fclose(f)) ok = FALSE;
        if (ok && MoveFileExW(tmp, dev_yml, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            event("devices_yml.repaired", 0);
            return;
        }
    }
    DeleteFileW(tmp);
}

static BOOL write_compat_yml(const WCHAR *uid_hex, BOOL use_scale2x) {
    WCHAR compat_dir[PATH_CAP], file[PATH_CAP], tmp[PATH_CAP];
    if (!joined(compat_dir, eka2l1_dir, L"compat") || !make_directory(compat_dir))
        return FALSE;
    if (FAILED(StringCchPrintfW(file, PATH_CAP, L"%s\\%s.yml", compat_dir, uid_hex)))
        return FALSE;
    if (!GetTempFileNameW(compat_dir, L"cmp", 0, tmp))
        return FALSE;
    FILE *f = NULL;
    if (_wfopen_s(&f, tmp, L"wb") || !f) {
        DeleteFileW(tmp);
        return FALSE;
    }
    fprintf(f,
        "fps: 60\n"
        "time-delay: 0\n"
        "should-child-inherit-setting: true\n"
        "screen-rotation: 0\n"
        "screen-mode: -1\n"
        "screen-upscale: -1\n"
        "screen-upscale-method: %d\n"
        "filter-shader-path: %s\n"
        "t9-bypass-hack: false\n",
        use_scale2x ? 1 : 0,
        use_scale2x ? "Scale2x" : "Default");
    BOOL ok = !ferror(f);
    if (fclose(f)) ok = FALSE;
    if (ok) ok = MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp);
    return ok;
}

static BOOL sync_eka2l1_presentation(void) {
    ensure_devices_yml_intact();

    WCHAR cfg[PATH_CAP], tmp[PATH_CAP];
    if (!joined(cfg, eka2l1_dir, L"config.yml") ||
        !GetTempFileNameW(eka2l1_dir, L"cfg", 0, tmp)) return FALSE;

    WCHAR eka_default_data[PATH_CAP], data_spec[PATH_CAP];
    joined(eka_default_data, eka2l1_dir, L"data");
    if (_wcsicmp(active_data_dir, eka_default_data) == 0) {
        StringCchCopyW(data_spec, PATH_CAP, L"data");
    } else {
        make_relative_or_keep(data_spec, PATH_CAP, eka2l1_dir,
                              active_data_dir, FILE_ATTRIBUTE_DIRECTORY);
    }
    char data_u8[4096] = "data";
    WideCharToMultiByte(CP_UTF8, 0, data_spec, -1, data_u8, (int)sizeof(data_u8) - 1, NULL, NULL);
    for (char *p = data_u8; *p; p++)
        if (*p == '\\') *p = '/';

    char bezel_u8[4096] = {0};
    BOOL use_bezel = ng_settings.bezel && *bezel_file &&
                     (GetFileAttributesW(bezel_file) != INVALID_FILE_ATTRIBUTES);
    if (use_bezel) {
        WCHAR bezel_spec[PATH_CAP];
        make_relative_or_keep(bezel_spec, PATH_CAP, eka2l1_dir,
                              bezel_file, FILE_ATTRIBUTE_NORMAL);
        WideCharToMultiByte(CP_UTF8, 0, bezel_spec, -1, bezel_u8, (int)sizeof(bezel_u8) - 1, NULL, NULL);
        for (char *p = bezel_u8; *p; p++)
            if (*p == '\\') *p = '/';
    }

    const WCHAR *active_kb = effective_keybind_profile();
    char kb_u8[256] = "resident_evil";
    WideCharToMultiByte(CP_UTF8, 0, active_kb, -1, kb_u8, (int)sizeof(kb_u8) - 1, NULL, NULL);

    BOOL nearest = (ng_settings.filter != 1);
    BOOL use_scale2x = (ng_settings.filter == 2);

    FILE *in = NULL, *out = NULL;
    BOOL ok = FALSE;
    BOOL has_device_line = FALSE;
    if (_wfopen_s(&out, tmp, L"wb") || !out) goto done;

    const char *managed[] = {
        "data-storage:",
        "enable-nearest-neighbor-filter:",
        "integer-scaling:",
        "background-image:",
        "background-image-opacity:",
        "background-image-keep-aspect:",
        "screen-scale-ratio:",
        "current-keybind-profile:"
    };

    if (file_has_valid_payload(cfg) && !_wfopen_s(&in, cfg, L"rb") && in) {
        char line[4096];
        while (fgets(line, sizeof(line), in)) {
            if (line[0] == '\0') continue;
            if (!strncmp(line, "device:", 7)) has_device_line = TRUE;
            BOOL skip = FALSE;
            for (int i = 0; i < (int)ARRAYSIZE(managed); i++) {
                if (!strncmp(line, managed[i], strlen(managed[i]))) {
                    skip = TRUE;
                    break;
                }
            }
            if (!skip) {
                fputs(line, out);
                size_t llen = strlen(line);
                if (llen > 0 && line[llen - 1] != '\n') fputc('\n', out);
            }
        }
        fclose(in);
    }

    if (!has_device_line) {
        fprintf(out,
            "device: 0\n"
            "language: -1\n"
            "enable-gdb-stub: false\n"
            "gdb-port: 24689\n");
    }

    fprintf(out,
        "data-storage: \"%s\"\n"
        "enable-nearest-neighbor-filter: %s\n"
        "integer-scaling: %s\n"
        "background-image: \"%s\"\n"
        "background-image-opacity: 255\n"
        "background-image-keep-aspect: true\n"
        "screen-scale-ratio: %d\n"
        "current-keybind-profile: %s\n",
        data_u8,
        nearest ? "true" : "false",
        ng_settings.integer ? "true" : "false",
        use_bezel ? bezel_u8 : "",
        use_bezel ? bezel_scale_ratio : 100,
        kb_u8);

    ok = !ferror(out);
    if (fclose(out)) ok = FALSE;
    out = NULL;
    if (ok) ok = MoveFileExW(tmp, cfg, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

    const WCHAR *uid_clean = (!wcsncmp(game_uid, L"0x", 2) || !wcsncmp(game_uid, L"0X", 2))
                             ? game_uid + 2 : game_uid;
    write_compat_yml(uid_clean, use_scale2x);
    write_compat_yml(L"20003B78", use_scale2x);

done:
    if (out) fclose(out);
    DeleteFileW(tmp);
    event("eka2l1.sync", ok ? (DWORD)ng_settings.filter : GetLastError());
    return ok;
}

/* ---------------------------------------------------------------------------
 * Cliente WinHTTP local para Control API do EKA2L1 (127.0.0.1)
 * --------------------------------------------------------------------------*/

static BOOL http_local_ex(INTERNET_PORT port, int timeout_ms,
                          const WCHAR *method, const WCHAR *path,
                          char *out_buf, size_t out_cap, DWORD *out_status) {
    HINTERNET session = NULL, connect = NULL, request = NULL;
    BOOL ok = FALSE;
    if (out_buf && out_cap) out_buf[0] = 0;
    if (out_status) *out_status = 0;

    session = WinHttpOpen(L"LegacyMobileForgeNG/1.2",
                          WINHTTP_ACCESS_TYPE_NO_PROXY,
                          WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto done;
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    connect = WinHttpConnect(session, L"127.0.0.1", port, 0);
    if (!connect) goto done;

    request = WinHttpOpenRequest(connect, method, path, NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) goto done;

    const WCHAR *hdrs = !wcscmp(method, L"POST")
        ? L"Content-Type: application/json\r\n"
        : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (!WinHttpSendRequest(request, hdrs, (DWORD)-1L,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        goto done;
    if (!WinHttpReceiveResponse(request, NULL)) goto done;

    DWORD status = 0, status_len = sizeof(status);
    if (!WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len,
            WINHTTP_NO_HEADER_INDEX))
        goto done;
    if (out_status) *out_status = status;

    if (out_buf && out_cap > 1) {
        size_t total = 0;
        for (;;) {
            DWORD avail = 0, read_n = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || !avail) break;
            if (total + avail >= out_cap) avail = (DWORD)(out_cap - 1 - total);
            if (!avail) break;
            if (!WinHttpReadData(request, out_buf + total, avail, &read_n) || !read_n)
                break;
            total += read_n;
        }
        out_buf[total] = 0;
    }
    ok = (status >= 200 && status < 300);

done:
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

static BOOL http_local(const WCHAR *method, const WCHAR *path,
                       char *out_buf, size_t out_cap, DWORD *out_status) {
    return http_local_ex(active_control_port, 12000, method, path, out_buf, out_cap, out_status);
}

/* ---------------------------------------------------------------------------
 * Gerenciamento de janelas e processos do EKA2L1 (Windows Job Objects + 3-Stage)
 * --------------------------------------------------------------------------*/

typedef struct {
    DWORD pid;
    HWND  qt_hwnd;
    int   qt_visible_count;
    int   close_mode; /* 0 = monitor, 1 = close non-main/modal dialogs, 2 = close all */
} EkaWinSearch;

static BOOL is_eka2l1_update_dialog(const WCHAR *text) {
    if (!text || !*text || wcsstr(text, L"EKA2L1") != NULL) return FALSE;
    return (wcsstr(text, L"atualiza") != NULL ||
            wcsstr(text, L"Atualiza") != NULL ||
            wcsstr(text, L"Update") != NULL ||
            wcsstr(text, L"update") != NULL);
}

static BOOL CALLBACK enum_eka_windows(HWND hwnd, LPARAM lp) {
    EkaWinSearch *ctx = (EkaWinSearch *)lp;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != ctx->pid) return TRUE;

    WCHAR cls[128] = {0}, text[256] = {0};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    GetWindowTextW(hwnd, text, ARRAYSIZE(text));

    if (!wcscmp(cls, L"ConsoleWindowClass")) {
        if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
        return TRUE;
    }

    if (ctx->close_mode == 1) {
        if (IsWindow(hwnd) && wcsstr(text, L"EKA2L1") == NULL) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }
    if (ctx->close_mode == 2) {
        if (IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }

    if (IsWindowVisible(hwnd)) {
        if (is_eka2l1_update_dialog(text)) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return TRUE;
        }
        ctx->qt_visible_count++;
        if (wcsstr(text, L"EKA2L1") != NULL || !ctx->qt_hwnd) {
            ctx->qt_hwnd = hwnd;
        }
    }
    return TRUE;
}

static HWND find_eka2l1_qt_window(DWORD pid) {
    EkaWinSearch ctx = {pid, NULL, 0, 0};
    EnumWindows(enum_eka_windows, (LPARAM)&ctx);
    return ctx.qt_hwnd;
}

static int count_eka2l1_qt_windows(DWORD pid) {
    EkaWinSearch ctx = {pid, NULL, 0, 0};
    EnumWindows(enum_eka_windows, (LPARAM)&ctx);
    return ctx.qt_visible_count;
}

static void post_close_to_eka2l1_windows(DWORD pid) {
    EkaWinSearch ctx = {pid, NULL, 0, 1};
    EnumWindows(enum_eka_windows, (LPARAM)&ctx);
    Sleep(60);
    ctx.close_mode = 2;
    EnumWindows(enum_eka_windows, (LPARAM)&ctx);
}

static void arrange_eka2l1_window(DWORD pid) {
    HWND qt_hwnd = find_eka2l1_qt_window(pid);
    if (!qt_hwnd) return;

    if (!ng_settings.fullscreen) {
        static const int widths[]  = {960, 1920, 2560, 2880, 3840};
        static const int heights[] = {540, 1080, 1440, 1620, 2160};
        int idx = (ng_settings.resolution >= 0 && ng_settings.resolution <= 4)
                  ? ng_settings.resolution : 1;
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int max_w = work.right - work.left;
        int max_h = work.bottom - work.top;
        int w = min(widths[idx], max_w);
        int h = min(heights[idx], max_h);
        int x = work.left + (max_w - w) / 2;
        int y = work.top  + (max_h - h) / 2;
        MoveWindow(qt_hwnd, x, y, w, h, TRUE);
    }
    SetForegroundWindow(qt_hwnd);
}

static HANDLE create_emulator_job(void) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) {
        event("job.create_failed", GetLastError());
        return NULL;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        event("job.limit_failed", GetLastError());
        CloseHandle(job);
        return NULL;
    }
    return job;
}

static DWORD query_job_active_processes(HANDLE job) {
    if (!job) return 0;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
    ZeroMemory(&acct, sizeof(acct));
    if (QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                  &acct, sizeof(acct), NULL)) {
        return acct.ActiveProcesses;
    }
    return 0;
}

static BOOL is_loopback_port_free(INTERNET_PORT port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return TRUE;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return TRUE;
    }
    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    BOOL free_port = (bind(s, (const struct sockaddr *)&addr, sizeof(addr)) == 0);
    closesocket(s);
    WSACleanup();
    return free_port;
}

static BOOL CALLBACK close_stale_eka_by_title(HWND hwnd, LPARAM lp) {
    (void)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    WCHAR text[256] = {0};
    GetWindowTextW(hwnd, text, ARRAYSIZE(text));
    if (wcsstr(text, L"EKA2L1 (") != NULL) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

static void prepare_clean_control_port(void) {
    active_control_port = configured_control_port;
    if (is_loopback_port_free(active_control_port)) return;

    char resp[1024];
    DWORD status = 0;
    if (http_local_ex(active_control_port, 600, L"GET", L"/v1/status", resp, sizeof(resp), &status) &&
        strstr(resp, "\"api\":") != NULL) {
        event("orphan.detected_on_port", active_control_port);
        http_local_ex(active_control_port, 800, L"POST", L"/v1/emulator/resume", resp, sizeof(resp), &status);
        http_local_ex(active_control_port, 1200, L"DELETE", L"/v1/session", resp, sizeof(resp), &status);
        settle_and_flush_live_saves();
        EnumWindows(close_stale_eka_by_title, 0);
        for (int i = 0; i < 12; i++) {
            Sleep(250);
            if (is_loopback_port_free(active_control_port)) {
                event("orphan.cleaned", 0);
                return;
            }
        }
    }

    for (INTERNET_PORT candidate = 28182; candidate <= 28199; candidate++) {
        if (is_loopback_port_free(candidate)) {
            active_control_port = candidate;
            event("port.reassigned", active_control_port);
            return;
        }
    }
}

static void shutdown_eka2l1_gracefully(HANDLE hProcess, DWORD pid, HANDLE hJob, const char *reason) {
    event(reason, pid);

    if (WaitForSingleObject(hProcess, 0) != WAIT_OBJECT_0) {
        char resp[512];
        DWORD http_status = 0;
        http_local_ex(active_control_port, 800, L"POST", L"/v1/emulator/resume",
                      resp, sizeof(resp), &http_status);
        http_local_ex(active_control_port, 1500, L"DELETE", L"/v1/session",
                      resp, sizeof(resp), &http_status);
        settle_and_flush_live_saves();

        post_close_to_eka2l1_windows(pid);
        if (WaitForSingleObject(hProcess, 4500) == WAIT_OBJECT_0) {
            event("shutdown.graceful_ok", 0);
        } else {
            event("shutdown.graceful_timeout", 4500);
        }
    } else {
        settle_and_flush_live_saves();
    }

    if (hJob) {
        DWORD remaining = query_job_active_processes(hJob);
        if (remaining > 0) {
            TerminateJobObject(hJob, 0);
            WaitForSingleObject(hProcess, 1500);
            event("job.terminated_remaining", remaining);
        } else {
            TerminateJobObject(hJob, 0);
        }
    } else if (WaitForSingleObject(hProcess, 0) != WAIT_OBJECT_0) {
        TerminateProcess(hProcess, 0);
        WaitForSingleObject(hProcess, 1500);
    }
}

static BOOL check_ingame_process_exited(const WCHAR *log_path, ULONGLONG *inout_offset,
                                        BOOL *inout_saw_summon) {
    HANDLE h = CreateFileW(log_path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER sz = {0};
    if (!GetFileSizeEx(h, &sz) || (ULONGLONG)sz.QuadPart <= *inout_offset) {
        CloseHandle(h);
        return FALSE;
    }

    LARGE_INTEGER seek_pos;
    seek_pos.QuadPart = (LONGLONG)(*inout_offset);
    if (!SetFilePointerEx(h, seek_pos, NULL, FILE_BEGIN)) {
        CloseHandle(h);
        return FALSE;
    }

    char buf[8192];
    DWORD read_n = 0;
    BOOL exited = FALSE;
    while (ReadFile(h, buf, sizeof(buf) - 1, &read_n, NULL) && read_n > 0) {
        *inout_offset += read_n;
        buf[read_n] = 0;
        if (strstr(buf, "Trying to summon: Resident_Evil") != NULL) {
            *inout_saw_summon = TRUE;
        }
        if (*inout_saw_summon &&
            (strstr(buf, "Resident_Evil") != NULL || strstr(buf, "2001ad82") != NULL || strstr(buf, "2001AD82") != NULL) &&
            (strstr(buf, "terminated peacefully") != NULL ||
             strstr(buf, "forcefully killed") != NULL ||
             strstr(buf, "panicked with category") != NULL)) {
            exited = TRUE;
            break;
        }
    }
    CloseHandle(h);
    return exited;
}

static BOOL append_arg(WCHAR *command, size_t capacity, const WCHAR *text) {
    if (FAILED(StringCchCatW(command, capacity, *command ? L" \"" : L"\"")))
        return FALSE;
    size_t slashes = 0;
    for (const WCHAR *p = text; ; p++) {
        if (*p == L'\\') { slashes++; continue; }
        size_t n = (*p == L'\"' || !*p) ? slashes * 2 : slashes;
        while (n--)
            if (FAILED(StringCchCatW(command, capacity, L"\\"))) return FALSE;
        slashes = 0;
        if (!*p) break;
        if (*p == L'\"' && FAILED(StringCchCatW(command, capacity, L"\\")))
            return FALSE;
        WCHAR one[2] = {*p, 0};
        if (FAILED(StringCchCatW(command, capacity, one))) return FALSE;
    }
    return SUCCEEDED(StringCchCatW(command, capacity, L"\""));
}

/* ---------------------------------------------------------------------------
 * Importação nativa de Firmware (SYM.ROM + SYM.RPKG), N-Gage (.sis) e Jogo (.n-gage)
 * --------------------------------------------------------------------------*/

static BOOL read_exact(HANDLE h, void *buf, DWORD len) {
    BYTE *p = (BYTE *)buf;
    DWORD done = 0;
    while (done < len) {
        DWORD n = 0;
        if (!ReadFile(h, p + done, len - done, &n, NULL) || n == 0) return FALSE;
        done += n;
    }
    return TRUE;
}

static void suppress_eka2l1_auto_update_dialog(void) {
    HKEY hKey = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\EKA2L1\\EKA2L1", 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        static const WCHAR val[] = L"true";
        RegSetValueExW(hKey, L"noAutoCheckUpdate", 0, REG_SZ,
                       (const BYTE *)val, (DWORD)sizeof(val));
        RegCloseKey(hKey);
    }
}

static BOOL run_eka2l1_install_sis(const WCHAR *sis_path) {
    WCHAR drives_c[PATH_CAP], drives_e[PATH_CAP];
    if (!joined(drives_c, active_data_dir, L"drives\\c") ||
        !joined(drives_e, active_data_dir, L"drives\\e")) return FALSE;
    make_directories_recursive(drives_c);
    make_directories_recursive(drives_e);
    suppress_eka2l1_auto_update_dialog();
    sync_eka2l1_presentation();

    INTERNET_PORT install_port = 28197;
    for (int cand = 28199; cand >= 28183; cand--) {
        if (is_loopback_port_free((INTERNET_PORT)cand)) {
            install_port = (INTERNET_PORT)cand;
            break;
        }
    }

    WCHAR *cmd = (WCHAR *)calloc(PATH_CAP, sizeof(WCHAR));
    if (!cmd) return FALSE;

    BOOL ok = FALSE;
    HANDLE hJob = NULL;
    WCHAR port_str[16];
    StringCchPrintfW(port_str, ARRAYSIZE(port_str), L"%u", (unsigned)install_port);
    if (!append_arg(cmd, PATH_CAP, eka2l1_exe) ||
        FAILED(StringCchCatW(cmd, PATH_CAP, L" --device ")) ||
        !append_arg(cmd, PATH_CAP, device_name) ||
        FAILED(StringCchCatW(cmd, PATH_CAP, L" --install ")) ||
        !append_arg(cmd, PATH_CAP, sis_path) ||
        FAILED(StringCchCatW(cmd, PATH_CAP, L" --debug-control-port ")) ||
        FAILED(StringCchCatW(cmd, PATH_CAP, port_str))) {
        goto done;
    }

    hJob = create_emulator_job();
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;

    ng_report_progress(60, tr(L"Installing N-Gage 2.0 client...",
                              L"Instalando plataforma cliente N-Gage 2.0..."));
    if (!CreateProcessW(eka2l1_exe, cmd, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, eka2l1_dir, &si, &pi)) {
        event("install.create_failed", GetLastError());
        goto done;
    }
    if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
    ResumeThread(pi.hThread);

    DWORD exit_code = 1;
    BOOL timed_out = TRUE;
    char resp[1024];
    DWORD http_code = 0;

    for (int iter = 0; iter < 240; iter++) {
        if (ng_stop_event && WaitForSingleObject(ng_stop_event, 0) == WAIT_OBJECT_0) {
            if (hJob) TerminateJobObject(hJob, 1);
            else TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1500);
            timed_out = FALSE;
            ok = FALSE;
            break;
        }
        if ((iter % 4) == 0) {
            ng_report_progress(60 + min(15, iter / 4),
                tr(L"Installing N-Gage 2.0 client...",
                   L"Instalando plataforma cliente N-Gage 2.0..."));
        }
        count_eka2l1_qt_windows(pi.dwProcessId);
        DWORD wr = WaitForSingleObject(pi.hProcess, 250);
        if (wr == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &exit_code);
            ok = (exit_code == 0);
            timed_out = FALSE;
            break;
        }
        if (http_local_ex(install_port, 800, L"GET", L"/v1/status",
                          resp, sizeof(resp), &http_code) &&
            (strstr(resp, "\"started\":true") || strstr(resp, "\"started\": true"))) {
            ok = TRUE;
            exit_code = 0;
            timed_out = FALSE;
            ng_report_progress(76, tr(L"N-Gage 2.0 client installed.",
                                      L"Cliente N-Gage 2.0 instalado."));
            Sleep(300);
            post_close_to_eka2l1_windows(pi.dwProcessId);
            if (WaitForSingleObject(pi.hProcess, 3000) != WAIT_OBJECT_0) {
                if (hJob) TerminateJobObject(hJob, 0);
                else TerminateProcess(pi.hProcess, 0);
                WaitForSingleObject(pi.hProcess, 1500);
            }
            break;
        }
    }

    if (timed_out) {
        event("install.timeout", 60000);
        if (hJob) TerminateJobObject(hJob, 1);
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1500);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (hJob) {
        CloseHandle(hJob);
        hJob = NULL;
    }
    Sleep(150);
    event(ok ? "install.sis_ok" : "install.sis_failed", exit_code);

done:
    if (hJob) CloseHandle(hJob);
    free(cmd);
    return ok;
}

static BOOL extract_sym_rpkg(const WCHAR *rpkg_path, const WCHAR *z_rm409_dir) {
    HANDLE h = CreateFileW(rpkg_path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER total_sz = {0};
    GetFileSizeEx(h, &total_sz);

    BOOL ok = FALSE;
    BYTE *io_buf = (BYTE *)malloc(65536);
    if (!io_buf) { CloseHandle(h); return FALSE; }

    BYTE magic[24];
    static const BYTE expected_magic[16] = {
        'R',0,0,0, 'P',0,0,0, 'K',0,0,0, 'G',0,0,0
    };
    if (!read_exact(h, magic, sizeof(magic)) ||
        memcmp(magic, expected_magic, 16) != 0) {
        event("rpkg.invalid_magic", 0);
        goto done;
    }

    make_directories_recursive(z_rm409_dir);
    DWORD extracted = 0;
    ULONGLONG bytes_done = 24ULL;
    ULONGLONG denom = (total_sz.QuadPart > 0) ? (ULONGLONG)total_sz.QuadPart : 1ULL;

    for (;;) {
        if (ng_stop_event && WaitForSingleObject(ng_stop_event, 0) == WAIT_OBJECT_0) goto done;

        ULONGLONG hdr[3] = {0};
        DWORD n_read = 0;
        if (!ReadFile(h, hdr, sizeof(hdr), &n_read, NULL)) goto done;
        if (n_read == 0) { ok = TRUE; break; }
        if (n_read != sizeof(hdr)) goto done;

        ULONGLONG path_len = hdr[2];
        if (path_len == 0 || path_len > 1024) {
            event("rpkg.invalid_pathlen", (DWORD)path_len);
            goto done;
        }

        WCHAR raw_path[1025] = {0};
        if (!read_exact(h, raw_path, (DWORD)(path_len * 2))) goto done;
        raw_path[path_len] = 0;

        ULONGLONG data_size = 0;
        if (!read_exact(h, &data_size, sizeof(data_size))) goto done;
        if (data_size > 256ULL * 1024ULL * 1024ULL) {
            event("rpkg.invalid_size", 0);
            goto done;
        }

        bytes_done += sizeof(hdr) + path_len * 2ULL + sizeof(data_size) + data_size;

        for (WCHAR *p = raw_path; *p; p++)
            if (*p == L'/') *p = L'\\';
        const WCHAR *rel = raw_path;
        if ((rel[0] == L'Z' || rel[0] == L'z') && rel[1] == L':' && rel[2] == L'\\')
            rel += 3;
        while (*rel == L'\\') rel++;

        if (!*rel || wcsstr(rel, L"..") != NULL || wcschr(rel, L':') != NULL) {
            event("rpkg.unsafe_path", 0);
            goto done;
        }

        WCHAR dest[PATH_CAP], parent[PATH_CAP];
        if (!joined(dest, z_rm409_dir, rel) ||
            FAILED(StringCchCopyW(parent, PATH_CAP, dest))) goto done;
        WCHAR *slash = wcsrchr(parent, L'\\');
        if (slash) {
            *slash = 0;
            make_directories_recursive(parent);
        }

        HANDLE out = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) {
            event("rpkg.write_open_failed", GetLastError());
            goto done;
        }

        ULONGLONG left = data_size;
        BOOL file_ok = TRUE;
        while (left > 0) {
            DWORD chunk = (DWORD)min(left, (ULONGLONG)65536);
            DWORD got = 0, wrote = 0;
            if (!ReadFile(h, io_buf, chunk, &got, NULL) || got == 0 ||
                !WriteFile(out, io_buf, got, &wrote, NULL) || wrote != got) {
                file_ok = FALSE;
                break;
            }
            left -= got;
        }
        CloseHandle(out);
        if (!file_ok) goto done;
        extracted++;
        if ((extracted & 127U) == 0U) {
            int pct = 15 + (int)min(43ULL, (bytes_done * 43ULL) / denom);
            WCHAR step[160];
            StringCchPrintfW(step, ARRAYSIZE(step),
                tr(L"Extracting SYM.RPKG firmware (%lu files)...",
                   L"Extraindo firmware SYM.RPKG (%lu arquivos)..."),
                (unsigned long)extracted);
            ng_report_progress(pct, step);
        }
    }
    event("rpkg.extracted", extracted);

done:
    free(io_buf);
    CloseHandle(h);
    return ok;
}

static BOOL check_sis_header(const WCHAR *path, DWORD *out_uid2) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD hdr[4] = {0};
    BOOL ok = read_exact(h, hdr, sizeof(hdr)) && hdr[0] == 0x10201A7AUL;
    if (ok && out_uid2) *out_uid2 = hdr[2];
    CloseHandle(h);
    return ok;
}

static BOOL import_ngage_client_sis(const WCHAR *sis_path) {
    DWORD uid2 = 0;
    if (!check_sis_header(sis_path, &uid2) || uid2 != 0x20003B78UL) {
        event("ngage_sis.rejected", uid2);
        return FALSE;
    }
    if (!has_valid_rm409_firmware(active_data_dir)) {
        event("ngage_sis.firmware_missing", ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    if (!run_eka2l1_install_sis(sis_path)) return FALSE;
    return check_ngage_client_installed(active_data_dir);
}

static BOOL import_firmware_source(const WCHAR *rom_or_dir, const WCHAR *explicit_rpkg,
                                   const WCHAR *explicit_ngage_sis) {
    if (!rom_or_dir || !*rom_or_dir) return FALSE;

    /* Caso 1: Pasta EKA2L1 / data já existente com RM-409 pronto (zero cópia ou download) */
    DWORD attr = GetFileAttributesW(rom_or_dir);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        WCHAR sub_data[PATH_CAP];
        ng_report_progress(20, tr(L"Checking existing EKA2L1 RM-409 folder...",
                                  L"Verificando instalação EKA2L1 existente..."));
        if (has_valid_rm409_firmware(rom_or_dir)) {
            StringCchCopyW(active_data_dir, PATH_CAP, rom_or_dir);
            StringCchCopyW(saved_data_dir, PATH_CAP, rom_or_dir);
            ng_report_progress(75, tr(L"Syncing portable configuration...",
                                      L"Sincronizando configuração portátil..."));
            ng_save_settings();
            sync_eka2l1_presentation();
            event("firmware.linked_existing", 0);
            return TRUE;
        }
        if (joined(sub_data, rom_or_dir, L"data") && has_valid_rm409_firmware(sub_data)) {
            StringCchCopyW(active_data_dir, PATH_CAP, sub_data);
            StringCchCopyW(saved_data_dir, PATH_CAP, sub_data);
            ng_report_progress(75, tr(L"Syncing portable configuration...",
                                      L"Sincronizando configuração portátil..."));
            ng_save_settings();
            sync_eka2l1_presentation();
            event("firmware.linked_existing", 1);
            return TRUE;
        }
    }

    /* Caso 2: Pasta contendo SYM.ROM + SYM.RPKG ou arquivo SYM.ROM direto */
    WCHAR rom_src[PATH_CAP] = {0}, rpkg_src[PATH_CAP] = {0}, src_dir[PATH_CAP] = {0};
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        WCHAR p1[PATH_CAP], p2[PATH_CAP];
        if (joined(p1, rom_or_dir, L"SYM.ROM") && GetFileAttributesW(p1) != INVALID_FILE_ATTRIBUTES) {
            StringCchCopyW(rom_src, PATH_CAP, p1);
            StringCchCopyW(src_dir, PATH_CAP, rom_or_dir);
        } else if (joined(p1, rom_or_dir, L"5320\\SYM.ROM") &&
                   GetFileAttributesW(p1) != INVALID_FILE_ATTRIBUTES) {
            StringCchCopyW(rom_src, PATH_CAP, p1);
            joined(src_dir, rom_or_dir, L"5320");
        }
        if (*src_dir && joined(p2, src_dir, L"SYM.RPKG"))
            StringCchCopyW(rpkg_src, PATH_CAP, p2);
    } else {
        StringCchCopyW(rom_src, PATH_CAP, rom_or_dir);
        StringCchCopyW(src_dir, PATH_CAP, rom_or_dir);
        WCHAR *slash = wcsrchr(src_dir, L'\\');
        if (slash) *slash = 0;
        if (explicit_rpkg && *explicit_rpkg) {
            StringCchCopyW(rpkg_src, PATH_CAP, explicit_rpkg);
        } else {
            joined(rpkg_src, src_dir, L"SYM.RPKG");
        }
    }
    if (explicit_rpkg && *explicit_rpkg) StringCchCopyW(rpkg_src, PATH_CAP, explicit_rpkg);

    WIN32_FILE_ATTRIBUTE_DATA rom_fad;
    if (!*rom_src || !GetFileAttributesExW(rom_src, GetFileExInfoStandard, &rom_fad) ||
        !file_has_valid_payload(rom_src)) {
        event("firmware.rom_missing", ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    ULONGLONG rom_sz = ((ULONGLONG)rom_fad.nFileSizeHigh << 32) | rom_fad.nFileSizeLow;
    if (rom_sz < (4ULL * 1024ULL * 1024ULL) || rom_sz > (256ULL * 1024ULL * 1024ULL)) {
        event("firmware.rom_invalid_size", ERROR_INVALID_DATA);
        return FALSE;
    }
    if (!*rpkg_src || GetFileAttributesW(rpkg_src) == INVALID_FILE_ATTRIBUTES) {
        event("firmware.rpkg_missing", ERROR_FILE_NOT_FOUND);
        return FALSE;
    }

    WCHAR default_data[PATH_CAP], dev_lower[128], rom_dst[PATH_CAP], z_dst[PATH_CAP];
    joined(default_data, eka2l1_dir, L"data");
    StringCchCopyW(active_data_dir, PATH_CAP, default_data);
    saved_data_dir[0] = 0;
    ng_save_settings();

    StringCchCopyW(dev_lower, ARRAYSIZE(dev_lower), device_name);
    CharLowerW(dev_lower);
    StringCchPrintfW(rom_dst, PATH_CAP, L"%s\\roms\\%s\\SYM.ROM", active_data_dir, dev_lower);
    StringCchPrintfW(z_dst, PATH_CAP, L"%s\\drives\\z\\%s", active_data_dir, dev_lower);

    ng_report_progress(8, tr(L"Copying Nokia 5320 ROM (SYM.ROM)...",
                             L"Copiando ROM Nokia 5320 (SYM.ROM)..."));
    if (!atomic_copy_with_flush(rom_src, rom_dst)) {
        event("firmware.rom_copy_failed", GetLastError());
        return FALSE;
    }
    ng_report_progress(15, tr(L"Extracting SYM.RPKG firmware...",
                              L"Extraindo firmware SYM.RPKG..."));
    if (!extract_sym_rpkg(rpkg_src, z_dst)) {
        event("firmware.rpkg_extract_failed", GetLastError());
        return FALSE;
    }

    ng_report_progress(59, tr(L"Configuring RM-409 device & timezone...",
                              L"Configurando dispositivo RM-409..."));
    WCHAR tz_src[PATH_CAP], tz_dst[PATH_CAP];
    if (joined(tz_src, z_dst, L"private\\1020383E\\tzdb.dbz") &&
        joined(tz_dst, active_data_dir, L"drives\\c\\private\\1020383E\\TZDB.DBZ") &&
        GetFileAttributesW(tz_src) != INVALID_FILE_ATTRIBUTES) {
        atomic_copy_with_flush(tz_src, tz_dst);
    }
    ensure_devices_yml_intact();
    sync_eka2l1_presentation();

    /* Instala ngage_client.sis caso informado ou localizado ao lado do firmware */
    WCHAR ngage_candidate[PATH_CAP] = {0};
    if (explicit_ngage_sis && *explicit_ngage_sis) {
        StringCchCopyW(ngage_candidate, PATH_CAP, explicit_ngage_sis);
    } else {
        const WCHAR *names[] = { L"ngage_client.sis", L"n-gage.sis", L"ngage.sis" };
        for (int i = 0; i < (int)ARRAYSIZE(names); i++) {
            WCHAR cand[PATH_CAP];
            if (joined(cand, src_dir, names[i]) &&
                GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) {
                StringCchCopyW(ngage_candidate, PATH_CAP, cand);
                break;
            }
        }
    }
    if (*ngage_candidate && !check_ngage_client_installed(active_data_dir)) {
        if (!import_ngage_client_sis(ngage_candidate)) {
            return FALSE;
        }
    }

    BOOL ok = has_valid_rm409_firmware(active_data_dir);
    ng_report_progress(ok ? 76 : 0,
        ok ? tr(L"RM-409 firmware ready.", L"Firmware RM-409 configurado.")
           : tr(L"Incomplete firmware.", L"Firmware incompleto."));
    event(ok ? "firmware.imported" : "firmware.incomplete", ok ? 0 : 1);
    return ok;
}

static const BYTE *mem_find_bytes(const BYTE *hay, size_t hay_len,
                                  const BYTE *needle, size_t needle_len) {
    if (!needle_len || hay_len < needle_len) return NULL;
    for (size_t i = 0; i <= hay_len - needle_len; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0)
            return hay + i;
    }
    return NULL;
}

static BOOL run_eka2l1_register_ngage_game(void) {
    if (!check_ngage_client_installed(active_data_dir)) return FALSE;
    suppress_eka2l1_auto_update_dialog();
    sync_eka2l1_presentation();

    INTERNET_PORT install_port = 28197;
    for (int cand = 28199; cand >= 28183; cand--) {
        if (is_loopback_port_free((INTERNET_PORT)cand)) {
            install_port = (INTERNET_PORT)cand;
            break;
        }
    }

    WCHAR *cmd = (WCHAR *)calloc(PATH_CAP, sizeof(WCHAR));
    if (!cmd) return FALSE;

    BOOL registered = FALSE;
    HANDLE hJob = NULL;
    WCHAR port_str[16];
    StringCchPrintfW(port_str, ARRAYSIZE(port_str), L"%u", (unsigned)install_port);
    if (!append_arg(cmd, PATH_CAP, eka2l1_exe) ||
        FAILED(StringCchCatW(cmd, PATH_CAP, L" --device ")) ||
        !append_arg(cmd, PATH_CAP, device_name) ||
        FAILED(StringCchCatW(cmd, PATH_CAP, L" --debug-control-port ")) ||
        FAILED(StringCchCatW(cmd, PATH_CAP, port_str))) {
        goto done;
    }

    hJob = create_emulator_job();
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;

    ng_report_progress(82, tr(L"Booting EKA2L1 to register game...",
                              L"Iniciando EKA2L1 para registrar o jogo no PlayServer..."));
    if (!CreateProcessW(eka2l1_exe, cmd, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, eka2l1_dir, &si, &pi)) {
        event("import.ngage_create_failed", GetLastError());
        goto done;
    }
    if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
    ResumeThread(pi.hThread);

    char resp[4096];
    DWORD http_code = 0;
    BOOL started = FALSE;
    for (int i = 0; i < 100; i++) {
        if (ng_stop_event && WaitForSingleObject(ng_stop_event, 0) == WAIT_OBJECT_0) break;
        count_eka2l1_qt_windows(pi.dwProcessId);
        if (WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0) break;
        if (http_local_ex(install_port, 1200, L"GET", L"/v1/status", resp, sizeof(resp), &http_code) &&
            (strstr(resp, "\"started\":true") || strstr(resp, "\"started\": true"))) {
            started = TRUE;
            break;
        }
    }

    if (started && (!ng_stop_event || WaitForSingleObject(ng_stop_event, 0) != WAIT_OBJECT_0)) {
        ng_report_progress(85, tr(L"Running N-Gage 2.0 installer (My Games)...",
                                  L"Executando instalador nativo N-Gage 2.0..."));
        http_local_ex(install_port, 25000, L"GET", L"/v1/ngage2/games", resp, sizeof(resp), &http_code);
        if ((strstr(resp, "2001AD82") != NULL || strstr(resp, "2001ad82") != NULL) &&
            check_game_installed(active_data_dir)) {
            registered = TRUE;
        }
        for (int iter = 0; !registered && iter < 45; iter++) {
            if (ng_stop_event && WaitForSingleObject(ng_stop_event, 0) == WAIT_OBJECT_0) break;
            ng_report_progress(86 + min(12, iter / 3),
                tr(L"Installing & registering Resident Evil in PlayServer...",
                   L"Instalando e registrando Resident Evil no N-Gage PlayServer..."));
            count_eka2l1_qt_windows(pi.dwProcessId);
            if (WaitForSingleObject(pi.hProcess, 1500) == WAIT_OBJECT_0) break;
            if (check_game_installed(active_data_dir) &&
                http_local_ex(install_port, 18000, L"GET", L"/v1/ngage2/games",
                              resp, sizeof(resp), &http_code) &&
                (strstr(resp, "2001AD82") != NULL || strstr(resp, "2001ad82") != NULL)) {
                registered = TRUE;
                break;
            }
        }
    }

    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
        if (registered) {
            ng_report_progress(99, tr(L"Finalizing PlayServer registration...",
                                      L"Finalizando registro do jogo..."));
            Sleep(400);
            http_local_ex(install_port, 1500, L"DELETE", L"/v1/session", resp, sizeof(resp), &http_code);
            post_close_to_eka2l1_windows(pi.dwProcessId);
            if (WaitForSingleObject(pi.hProcess, 3500) != WAIT_OBJECT_0) {
                if (hJob) TerminateJobObject(hJob, 0);
                else TerminateProcess(pi.hProcess, 0);
                WaitForSingleObject(pi.hProcess, 1500);
            }
        } else {
            if (hJob) TerminateJobObject(hJob, 1);
            else TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1500);
        }
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (hJob) {
        CloseHandle(hJob);
        hJob = NULL;
    }
    Sleep(200);
    event(registered ? "import.ngage_registered" : "import.ngage_timeout", registered ? 0 : 1);

done:
    if (hJob) CloseHandle(hJob);
    free(cmd);
    return registered;
}

static BOOL import_game_package(const WCHAR *pkg_path) {
    if (!has_valid_rm409_firmware(active_data_dir)) {
        event("import.firmware_required", ERROR_FILE_NOT_FOUND);
        return FALSE;
    }

    HANDLE h = CreateFileW(pkg_path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        event("import.open_failed", GetLastError());
        return FALSE;
    }

    LARGE_INTEGER fsz = {0};
    if (!GetFileSizeEx(h, &fsz) || fsz.QuadPart < 64 ||
        (ULONGLONG)fsz.QuadPart > MAX_IMPORT_BYTES) {
        CloseHandle(h);
        event("import.invalid_size", ERROR_INVALID_DATA);
        return FALSE;
    }

    size_t buf_len = (size_t)fsz.QuadPart;
    BYTE *buf = (BYTE *)malloc(buf_len);
    if (!buf) {
        CloseHandle(h);
        return FALSE;
    }
    if (!read_exact(h, buf, (DWORD)buf_len)) {
        free(buf);
        CloseHandle(h);
        return FALSE;
    }
    CloseHandle(h);

    DWORD expected_uid = (DWORD)wcstoul(game_uid, NULL, 0);
    if (expected_uid == 0) expected_uid = 0x2001AD82UL;

    int slice_count = 0;
    BOOL matched_uid = FALSE;

    DWORD magic0 = 0;
    memcpy(&magic0, buf, 4);
    BOOL is_raw_sis = (magic0 == 0x10201A7AUL);
    if (is_raw_sis) {
        DWORD uid2 = 0;
        memcpy(&uid2, buf + 8, 4);
        if (uid2 == expected_uid) {
            matched_uid = TRUE;
            slice_count = 1;
        }
    } else {
        /* Contêiner MIME multipart (.n-gage) */
        char boundary_sep[128] = "--KBoundary";
        const BYTE *b_attr = mem_find_bytes(buf, min(buf_len, (size_t)2048),
                                            (const BYTE *)"boundary=\"", 10);
        if (b_attr) {
            const BYTE *b_start = b_attr + 10;
            const BYTE *b_end = memchr(b_start, '\"', 80);
            if (b_end && (size_t)(b_end - b_start) > 0 && (size_t)(b_end - b_start) < 100) {
                size_t blen = (size_t)(b_end - b_start);
                boundary_sep[0] = '-';
                boundary_sep[1] = '-';
                memcpy(boundary_sep + 2, b_start, blen);
                boundary_sep[2 + blen] = 0;
            }
        }
        size_t sep_len = strlen(boundary_sep);
        const BYTE *cur = buf;
        const BYTE *end = buf + buf_len;
        while (cur < end && slice_count < 4) {
            const BYTE *b = mem_find_bytes(cur, (size_t)(end - cur),
                                           (const BYTE *)boundary_sep, sep_len);
            if (!b) break;
            const BYTE *part_start = b + sep_len;
            const BYTE *next_b = mem_find_bytes(part_start, (size_t)(end - part_start),
                                                (const BYTE *)boundary_sep, sep_len);
            const BYTE *part_end = next_b ? next_b : end;

            const BYTE *body = mem_find_bytes(part_start, (size_t)(part_end - part_start),
                                              (const BYTE *)"\r\n\r\n", 4);
            size_t hdr_sep = 4;
            if (!body) {
                body = mem_find_bytes(part_start, (size_t)(part_end - part_start),
                                      (const BYTE *)"\n\n", 2);
                hdr_sep = 2;
            }
            if (body) {
                body += hdr_sep;
                const BYTE *trim_end = part_end;
                while (trim_end > body && (trim_end[-1] == '\n' || trim_end[-1] == '\r'))
                    trim_end--;
                size_t slen = (size_t)(trim_end - body);
                if (slen >= 64) {
                    DWORD m = 0, u2 = 0;
                    memcpy(&m, body, 4);
                    memcpy(&u2, body + 8, 4);
                    if (m == 0x10201A7AUL && u2 == expected_uid) {
                        matched_uid = TRUE;
                        slice_count++;
                    }
                }
            }
            cur = part_end;
        }
    }

    free(buf);
    buf = NULL;

    if (!matched_uid || slice_count == 0) {
        event("import.game_rejected", ERROR_INVALID_DATA);
        return FALSE;
    }

    /* Jogo já instalado e arquivo de origem validado: preserva cache/saves */
    if (check_game_installed(active_data_dir) && check_ngage_client_installed(active_data_dir)) {
        event("import.game_cached", 0);
        return TRUE;
    }

    BOOL ok = FALSE;
    if (is_raw_sis) {
        ng_report_progress(78, tr(L"Installing Resident Evil SIS package...",
                                  L"Instalando pacote SIS de Resident Evil..."));
        ok = run_eka2l1_install_sis(pkg_path);
    } else {
        if (!check_ngage_client_installed(active_data_dir)) {
            event("import.ngage_client_required", ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
        ng_report_progress(78, tr(L"Preparing Resident Evil: Degeneration package...",
                                  L"Preparando pacote Resident Evil: Degeneration (.n-gage)..."));
        const WCHAR *uid_clean = (!wcsncmp(game_uid, L"0x", 2) || !wcsncmp(game_uid, L"0X", 2))
                                 ? game_uid + 2 : game_uid;
        WCHAR rel_staged[128], staged_ngage[PATH_CAP];
        StringCchPrintfW(rel_staged, ARRAYSIZE(rel_staged), L"drives\\e\\n-gage\\%s.n-gage", uid_clean);
        if (!joined(staged_ngage, active_data_dir, rel_staged) ||
            !atomic_copy_with_flush(pkg_path, staged_ngage)) {
            event("import.ngage_stage_failed", GetLastError());
            return FALSE;
        }
        ok = run_eka2l1_register_ngage_game();
    }

    if (ok) ok = check_game_installed(active_data_dir);
    event(ok ? "import.game_ok" : "import.game_failed", ok ? 0 : ERROR_INSTALL_FAILURE);
    return ok;
}

typedef struct {
    WCHAR rom[PATH_CAP];
    WCHAR rpkg[PATH_CAP];
    WCHAR ngage_sis[PATH_CAP];
    WCHAR game_pkg[PATH_CAP];
    WCHAR existing_data[PATH_CAP];
} NgImportBundle;

static BOOL is_rpkg_file(const WCHAR *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    LARGE_INTEGER sz = {0};
    BYTE hdr[16] = {0};
    static const BYTE expected[16] = {
        'R',0,0,0, 'P',0,0,0, 'K',0,0,0, 'G',0,0,0
    };
    BOOL ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 24 &&
              sz.QuadPart <= (512LL * 1024LL * 1024LL) &&
              read_exact(h, hdr, sizeof(hdr)) &&
              memcmp(hdr, expected, sizeof(hdr)) == 0;
    CloseHandle(h);
    return ok;
}

static BOOL is_matching_game_file(const WCHAR *path, DWORD expected_uid) {
    DWORD uid2 = 0;
    if (check_sis_header(path, &uid2)) {
        return uid2 == expected_uid;
    }

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    LARGE_INTEGER sz = {0};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 256 ||
        (ULONGLONG)sz.QuadPart > MAX_IMPORT_BYTES) {
        CloseHandle(h);
        return FALSE;
    }

    size_t probe_len = (size_t)min(sz.QuadPart, 262144LL);
    BYTE *buf = (BYTE *)malloc(probe_len);
    BOOL matched = FALSE;
    if (buf && read_exact(h, buf, (DWORD)probe_len)) {
        char boundary_sep[128] = "--KBoundary";
        const BYTE *b_attr = mem_find_bytes(buf, min(probe_len, (size_t)2048),
                                            (const BYTE *)"boundary=\"", 10);
        if (b_attr) {
            const BYTE *b_start = b_attr + 10;
            const BYTE *b_end = memchr(b_start, '\"', 80);
            if (b_end && (size_t)(b_end - b_start) > 0 && (size_t)(b_end - b_start) < 100) {
                size_t blen = (size_t)(b_end - b_start);
                boundary_sep[0] = '-';
                boundary_sep[1] = '-';
                memcpy(boundary_sep + 2, b_start, blen);
                boundary_sep[2 + blen] = 0;
            }
        }
        size_t sep_len = strlen(boundary_sep);
        const BYTE *cur = buf;
        const BYTE *end = buf + probe_len;
        while (cur < end) {
            const BYTE *b = mem_find_bytes(cur, (size_t)(end - cur),
                                           (const BYTE *)boundary_sep, sep_len);
            if (!b) break;
            const BYTE *part_start = b + sep_len;
            const BYTE *body = mem_find_bytes(part_start, (size_t)(end - part_start),
                                              (const BYTE *)"\r\n\r\n", 4);
            size_t hdr_sep = 4;
            if (!body) {
                body = mem_find_bytes(part_start, (size_t)(end - part_start),
                                      (const BYTE *)"\n\n", 2);
                hdr_sep = 2;
            }
            if (body && (size_t)(end - (body + hdr_sep)) >= 16) {
                body += hdr_sep;
                DWORD m = 0, u2 = 0;
                memcpy(&m, body, 4);
                memcpy(&u2, body + 8, 4);
                if (m == 0x10201A7AUL && u2 == expected_uid) {
                    matched = TRUE;
                    break;
                }
            }
            cur = part_start;
        }
    }
    free(buf);
    CloseHandle(h);
    return matched;
}

static void inspect_candidate_file(const WCHAR *file_path, DWORD expected_uid, NgImportBundle *b) {
    if (!file_path || !*file_path || !b) return;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(file_path, GetFileExInfoStandard, &fad) ||
        (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return;

    const WCHAR *fname = wcsrchr(file_path, L'\\');
    fname = fname ? (fname + 1) : file_path;
    const WCHAR *ext = wcsrchr(fname, L'.');

    if (is_rpkg_file(file_path)) {
        if (!b->rpkg[0] || _wcsicmp(fname, L"SYM.RPKG") == 0)
            StringCchCopyW(b->rpkg, PATH_CAP, file_path);
        return;
    }

    DWORD uid2 = 0;
    if (check_sis_header(file_path, &uid2)) {
        if (uid2 == 0x20003B78UL) {
            if (!b->ngage_sis[0] || _wcsicmp(fname, L"ngage_client.sis") == 0)
                StringCchCopyW(b->ngage_sis, PATH_CAP, file_path);
        } else if (uid2 == expected_uid) {
            if (!b->game_pkg[0])
                StringCchCopyW(b->game_pkg, PATH_CAP, file_path);
        }
        return;
    }

    ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if ((_wcsicmp(fname, L"SYM.ROM") == 0 || (ext && _wcsicmp(ext, L".rom") == 0)) &&
        sz >= (4ULL * 1024ULL * 1024ULL) && sz <= (256ULL * 1024ULL * 1024ULL) &&
        file_has_valid_payload(file_path)) {
        if (!b->rom[0] || _wcsicmp(fname, L"SYM.ROM") == 0)
            StringCchCopyW(b->rom, PATH_CAP, file_path);
        return;
    }

    if (is_matching_game_file(file_path, expected_uid)) {
        StringCchCopyW(b->game_pkg, PATH_CAP, file_path);
    }
}

static void scan_folder_for_bundle(const WCHAR *dir, int depth, DWORD expected_uid, NgImportBundle *b) {
    if (!dir || !*dir || !b) return;
    if (depth == 0 && !b->existing_data[0]) {
        WCHAR sub_data[PATH_CAP];
        if (has_valid_rm409_firmware(dir)) {
            StringCchCopyW(b->existing_data, PATH_CAP, dir);
        } else if (joined(sub_data, dir, L"data") && has_valid_rm409_firmware(sub_data)) {
            StringCchCopyW(b->existing_data, PATH_CAP, sub_data);
        }
    }

    WCHAR pattern[PATH_CAP];
    if (!joined(pattern, dir, L"*")) return;

    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pattern, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;

    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        WCHAR child[PATH_CAP];
        if (!joined(child, dir, fd.cFileName)) continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 1 &&
                _wcsicmp(fd.cFileName, L"data") != 0 &&
                _wcsicmp(fd.cFileName, L"runtime") != 0 &&
                _wcsicmp(fd.cFileName, L"backups") != 0 &&
                _wcsicmp(fd.cFileName, L"release") != 0 &&
                _wcsicmp(fd.cFileName, L".git") != 0) {
                scan_folder_for_bundle(child, depth + 1, expected_uid, b);
            }
        } else {
            inspect_candidate_file(child, expected_uid, b);
        }
    } while (FindNextFileW(hf, &fd));

    FindClose(hf);
}

static BOOL execute_import_bundle(const NgImportBundle *in_b, WCHAR *out_notice, size_t notice_cap) {
    if (out_notice && notice_cap) out_notice[0] = 0;
    if (!in_b) return FALSE;

    NgImportBundle b = *in_b;
    if (b.rom[0] && !b.rpkg[0]) {
        WCHAR sib_dir[PATH_CAP], sib_rpkg[PATH_CAP];
        if (SUCCEEDED(StringCchCopyW(sib_dir, PATH_CAP, b.rom))) {
            WCHAR *sl = wcsrchr(sib_dir, L'\\');
            if (sl) *sl = 0;
            if (joined(sib_rpkg, sib_dir, L"SYM.RPKG") && is_rpkg_file(sib_rpkg)) {
                StringCchCopyW(b.rpkg, PATH_CAP, sib_rpkg);
            }
        }
    }

    BOOL step_ok = TRUE;
    BOOL any_input = (b.rom[0] || b.rpkg[0] || b.ngage_sis[0] || b.game_pkg[0] || b.existing_data[0]);
    if (!any_input) {
        if (out_notice) {
            StringCchCopyW(out_notice, notice_cap,
                tr(L"No SYM.ROM, SYM.RPKG, ngage_client.sis or Resident Evil (.n-gage) found.",
                   L"Nenhum SYM.ROM, SYM.RPKG, ngage_client.sis ou jogo (.n-gage) encontrado."));
        }
        ng_report_progress(100, out_notice ? out_notice : L"");
        return FALSE;
    }

    ng_report_progress(3, tr(L"Analyzing selected files...", L"Analisando arquivos selecionados..."));

    if (b.existing_data[0] && !b.rom[0]) {
        if (!import_firmware_source(b.existing_data, NULL, NULL)) {
            step_ok = FALSE;
            if (out_notice) {
                StringCchCopyW(out_notice, notice_cap,
                    tr(L"Failed linking existing EKA2L1 RM-409 folder.",
                       L"Falha ao vincular pasta EKA2L1 RM-409 existente."));
            }
        }
    } else if (b.rom[0]) {
        if (!b.rpkg[0]) {
            step_ok = FALSE;
            if (out_notice) {
                StringCchCopyW(out_notice, notice_cap,
                    tr(L"Found SYM.ROM, but SYM.RPKG is missing.",
                       L"SYM.ROM encontrado, mas faltou selecionar SYM.RPKG."));
            }
        } else if (!import_firmware_source(b.rom, b.rpkg, b.ngage_sis[0] ? b.ngage_sis : NULL)) {
            step_ok = FALSE;
            if (out_notice) {
                StringCchCopyW(out_notice, notice_cap,
                    tr(L"Failed importing RM-409 firmware / N-Gage client.",
                       L"Falha ao importar firmware RM-409 ou cliente N-Gage."));
            }
        }
    } else if (b.rpkg[0] && !has_valid_rm409_firmware(active_data_dir)) {
        step_ok = FALSE;
        if (out_notice) {
            StringCchCopyW(out_notice, notice_cap,
                tr(L"Found SYM.RPKG, please include SYM.ROM together.",
                   L"SYM.RPKG encontrado; selecione junto o arquivo SYM.ROM."));
        }
    }

    if (step_ok && b.ngage_sis[0] && !check_ngage_client_installed(active_data_dir)) {
        if (!has_valid_rm409_firmware(active_data_dir)) {
            step_ok = FALSE;
            if (out_notice) {
                StringCchCopyW(out_notice, notice_cap,
                    tr(L"RM-409 firmware is required before ngage_client.sis.",
                       L"Importe o firmware RM-409 (SYM.ROM + SYM.RPKG) antes de ngage_client.sis."));
            }
        } else if (!import_ngage_client_sis(b.ngage_sis)) {
            step_ok = FALSE;
            if (out_notice) {
                StringCchCopyW(out_notice, notice_cap,
                    tr(L"Failed installing ngage_client.sis.",
                       L"Falha ao instalar ngage_client.sis."));
            }
        }
    }

    if (step_ok && b.game_pkg[0]) {
        if (!import_game_package(b.game_pkg)) {
            step_ok = FALSE;
            if (out_notice) {
                StringCchCopyW(out_notice, notice_cap,
                    tr(L"Failed importing Resident Evil (.n-gage). Check Firmware and N-Gage.",
                       L"Falha ao importar Resident Evil (.n-gage). Verifique Firmware e N-Gage."));
            }
        }
    }

    BOOL ok_e = FALSE, ok_r = FALSE, ok_n = FALSE, ok_g = FALSE;
    BOOL ready = check_backend_files(&ok_e, &ok_r, &ok_n, &ok_g, NULL, NULL);
    if (out_notice && !*out_notice) {
        if (ready) {
            StringCchCopyW(out_notice, notice_cap,
                tr(L"All files imported! Ready to play.",
                   L"Tudo importado! Pronto para jogar."));
        } else if (!ok_r) {
            StringCchCopyW(out_notice, notice_cap,
                tr(L"Still missing RM-409 firmware (SYM.ROM + SYM.RPKG).",
                   L"Ainda falta importar Firmware RM-409 (SYM.ROM + SYM.RPKG)."));
        } else if (!ok_n) {
            StringCchCopyW(out_notice, notice_cap,
                tr(L"Firmware OK! Still need ngage_client.sis.",
                   L"Firmware OK! Ainda falta importar ngage_client.sis."));
        } else if (!ok_g) {
            StringCchCopyW(out_notice, notice_cap,
                tr(L"Firmware & N-Gage OK! Still need Resident Evil (.n-gage).",
                   L"Firmware e N-Gage OK! Ainda falta selecionar Resident Evil (.n-gage)."));
        }
    }

    ng_report_progress(100, out_notice && *out_notice
        ? out_notice
        : tr(L"Operation finished.", L"Operação concluída."));
    return step_ok && ready;
}

static HRESULT pick_path_dialog(HWND owner, BOOL probe_only, BOOL pick_folder,
                                const WCHAR *dlg_title, const COMDLG_FILTERSPEC *filter,
                                WCHAR *out_path, size_t out_cap) {
    if (out_path && out_cap) out_path[0] = 0;
    IFileOpenDialog *picker = NULL;
    IShellItem *item = NULL;
    PWSTR selected = NULL;
    FILEOPENDIALOGOPTIONS opts = 0;

    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileOpenDialog, (void **)&picker);
    if (FAILED(hr)) return hr;
    hr = IFileOpenDialog_GetOptions(picker, &opts);
    if (SUCCEEDED(hr)) {
        DWORD flags = opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
        flags |= pick_folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
        hr = IFileOpenDialog_SetOptions(picker, flags);
    }
    if (SUCCEEDED(hr) && !pick_folder && filter)
        hr = IFileOpenDialog_SetFileTypes(picker, 1, filter);
    if (SUCCEEDED(hr) && dlg_title)
        hr = IFileOpenDialog_SetTitle(picker, dlg_title);
    if (!probe_only && SUCCEEDED(hr))
        hr = IFileOpenDialog_Show(picker, owner);
    if (!probe_only && SUCCEEDED(hr))
        hr = IFileOpenDialog_GetResult(picker, &item);
    if (item && SUCCEEDED(hr))
        hr = IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &selected);
    if (selected && SUCCEEDED(hr) && out_path)
        StringCchCopyW(out_path, out_cap, selected);

    if (selected) CoTaskMemFree(selected);
    if (item) IShellItem_Release(item);
    IFileOpenDialog_Release(picker);
    return hr;
}

static HRESULT pick_multi_files_dialog(HWND owner, BOOL probe_only, const WCHAR *dlg_title,
                                       NgImportBundle *out_bundle, DWORD *out_selected_count) {
    if (out_bundle) ZeroMemory(out_bundle, sizeof(*out_bundle));
    if (out_selected_count) *out_selected_count = 0;

    IFileOpenDialog *picker = NULL;
    IShellItemArray *results = NULL;
    FILEOPENDIALOGOPTIONS opts = 0;

    static const COMDLG_FILTERSPEC filters[] = {
        { L"Firmware / N-Gage / Game (*.rom;*.rpkg;*.sis;*.n-gage)", L"*.rom;*.rpkg;*.sis;*.sisx;*.n-gage" },
        { L"All files (*.*)", L"*.*" }
    };

    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileOpenDialog, (void **)&picker);
    if (FAILED(hr)) return hr;
    hr = IFileOpenDialog_GetOptions(picker, &opts);
    if (SUCCEEDED(hr)) {
        DWORD flags = opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                      FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT | FOS_NOCHANGEDIR;
        hr = IFileOpenDialog_SetOptions(picker, flags);
    }
    if (SUCCEEDED(hr))
        hr = IFileOpenDialog_SetFileTypes(picker, ARRAYSIZE(filters), filters);
    if (SUCCEEDED(hr) && dlg_title)
        hr = IFileOpenDialog_SetTitle(picker, dlg_title);
    if (!probe_only && SUCCEEDED(hr))
        hr = IFileOpenDialog_Show(picker, owner);
    if (!probe_only && SUCCEEDED(hr))
        hr = IFileOpenDialog_GetResults(picker, &results);
    if (results && SUCCEEDED(hr)) {
        DWORD count = 0;
        hr = IShellItemArray_GetCount(results, &count);
        if (SUCCEEDED(hr)) {
            if (out_selected_count) *out_selected_count = count;
            DWORD expected_uid = (DWORD)wcstoul(game_uid, NULL, 0);
            if (!expected_uid) expected_uid = 0x2001AD82UL;
            for (DWORD i = 0; i < count; i++) {
                IShellItem *item = NULL;
                if (SUCCEEDED(IShellItemArray_GetItemAt(results, i, &item)) && item) {
                    PWSTR p = NULL;
                    if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &p)) && p) {
                        if (out_bundle) inspect_candidate_file(p, expected_uid, out_bundle);
                        CoTaskMemFree(p);
                    }
                    IShellItem_Release(item);
                }
            }
        }
    }

    if (results) IShellItemArray_Release(results);
    IFileOpenDialog_Release(picker);
    return hr;
}

/* ---------------------------------------------------------------------------
 * Lançamento direto supervisionado do EKA2L1
 * --------------------------------------------------------------------------*/

static DWORD launch_eka2l1(void) {
    BOOL ok_eka = FALSE, ok_rom = FALSE, ok_ngc = FALSE, ok_game = FALSE;
    if (!check_backend_files(&ok_eka, &ok_rom, &ok_ngc, &ok_game, NULL, NULL)) {
        event("preflight.missing", ERROR_FILE_NOT_FOUND);
        return 1;
    }

    if (ng_stop_event) ResetEvent(ng_stop_event);
    InterlockedExchange(&ng_game_live, 0);
    InterlockedExchange(&ng_quit_from_select, 0);
    InterlockedExchange((volatile LONG *)&ng_eka_pid, 0);

    preflight_protect_saves();
    prepare_clean_control_port();
    sync_eka2l1_presentation();

    WCHAR *cmd = (WCHAR *)calloc(PATH_CAP, sizeof(WCHAR));
    if (!cmd) return 1;

    DWORD result = 1;
    HANDLE hJob = NULL;
    WCHAR port_str[16];
    StringCchPrintfW(port_str, ARRAYSIZE(port_str), L"%u", (unsigned)active_control_port);

    const WCHAR *active_kb = effective_keybind_profile();
    if (!append_arg(cmd, PATH_CAP, eka2l1_exe) ||
        FAILED(StringCchCatW(cmd, PATH_CAP, L" --device ")) ||
        !append_arg(cmd, PATH_CAP, device_name) ||
        FAILED(StringCchCatW(cmd, PATH_CAP, L" --keybindprofile ")) ||
        !append_arg(cmd, PATH_CAP, active_kb) ||
        FAILED(StringCchCatW(cmd, PATH_CAP, L" --debug-control-port ")) ||
        FAILED(StringCchCatW(cmd, PATH_CAP, port_str))) {
        event("command.build_failed", 0);
        goto done;
    }
    if (ng_settings.fullscreen) {
        if (FAILED(StringCchCatW(cmd, PATH_CAP, L" --fullscreen"))) goto done;
    }

    hJob = create_emulator_job();

    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    suppress_eka2l1_auto_update_dialog();
    event("eka2l1.start", active_control_port);

    if (!CreateProcessW(eka2l1_exe, cmd, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, eka2l1_dir, &si, &pi)) {
        event("eka2l1.create_failed", GetLastError());
        goto done;
    }

    InterlockedExchange((volatile LONG *)&ng_eka_pid, (LONG)pi.dwProcessId);
    if (hJob) {
        if (AssignProcessToJobObject(hJob, pi.hProcess)) {
            event("job.assigned", pi.dwProcessId);
        } else {
            event("job.assign_failed", GetLastError());
        }
    }
    ResumeThread(pi.hThread);

    char resp[4096];
    DWORD http_code = 0;
    BOOL service_ready = FALSE;
    for (int attempt = 0; attempt < 100; attempt++) {
        if (ng_stop_event && WaitForSingleObject(ng_stop_event, 0) == WAIT_OBJECT_0) break;
        if (WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0) break;
        if (http_local_ex(active_control_port, 1500, L"GET", L"/v1/status",
                          resp, sizeof(resp), &http_code)) {
            if (strstr(resp, "\"started\":true") || strstr(resp, "\"started\": true")) {
                service_ready = TRUE;
                break;
            }
        }
    }

    BOOL launched = FALSE;
    if (service_ready && (!ng_stop_event || WaitForSingleObject(ng_stop_event, 0) != WAIT_OBJECT_0)) {
        arrange_eka2l1_window(pi.dwProcessId);
        for (int retry = 0; retry < 12; retry++) {
            if (ng_stop_event && WaitForSingleObject(ng_stop_event, 0) == WAIT_OBJECT_0) break;
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
            if (http_local_ex(active_control_port, 20000, L"GET", L"/v1/ngage2/games",
                              resp, sizeof(resp), &http_code) &&
                (strstr(resp, "2001AD82") != NULL || strstr(resp, "2001ad82") != NULL)) {
                break;
            }
            Sleep(600);
        }
        WCHAR launch_path[128];
        StringCchPrintfW(launch_path, ARRAYSIZE(launch_path),
                         L"/v1/ngage2/games/%s/launch", game_uid);
        for (int ltry = 0; ltry < 4; ltry++) {
            if (ng_stop_event && WaitForSingleObject(ng_stop_event, 0) == WAIT_OBJECT_0) break;
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
            if (http_local_ex(active_control_port, 20000, L"POST", launch_path,
                              resp, sizeof(resp), &http_code)) {
                launched = TRUE;
                break;
            }
            if (http_code != 409 && http_code != 404) break;
            Sleep(700);
        }
        event(launched ? "direct_launch.ok" : "direct_launch.failed", http_code);
        if (launched) InterlockedExchange(&ng_game_live, 1);
        Sleep(300);
        arrange_eka2l1_window(pi.dwProcessId);
    } else if (!ng_stop_event || WaitForSingleObject(ng_stop_event, 0) != WAIT_OBJECT_0) {
        event("control_api.timeout", http_code);
    }

    WCHAR eka_log[PATH_CAP];
    BOOL has_eka_log = joined(eka_log, eka2l1_dir, L"EKA2L1.log");
    ULONGLONG log_offset = 0;
    BOOL saw_summon = launched;
    BOOL saw_qt_window = FALSE;
    BOOL saw_screen_visible = FALSE;
    int  no_window_ticks = 0;
    int  poll_tick = 0;
    BOOL clean_triggered = FALSE;

    for (;;) {
        HANDLE waits[2] = { pi.hProcess, ng_stop_event };
        DWORD wait_count = ng_stop_event ? 2 : 1;
        DWORD wr = WaitForMultipleObjects(wait_count, waits, FALSE, 400);

        if (wr == WAIT_OBJECT_0) {
            break;
        }
        if (ng_stop_event && wr == WAIT_OBJECT_0 + 1) {
            const char *why = InterlockedCompareExchange(&ng_quit_from_select, 0, 0)
                              ? "shutdown.select_quit" : "shutdown.launcher_close";
            InterlockedExchange(&ng_game_live, 0);
            shutdown_eka2l1_gracefully(pi.hProcess, pi.dwProcessId, hJob, why);
            clean_triggered = TRUE;
            break;
        }

        int vis_qt = count_eka2l1_qt_windows(pi.dwProcessId);
        if (vis_qt > 0) {
            saw_qt_window = TRUE;
            no_window_ticks = 0;
        } else if (saw_qt_window) {
            no_window_ticks++;
            if (no_window_ticks >= 2) {
                InterlockedExchange(&ng_game_live, 0);
                shutdown_eka2l1_gracefully(pi.hProcess, pi.dwProcessId, hJob, "shutdown.window_closed");
                clean_triggered = TRUE;
                break;
            }
        }

        poll_tick++;
        if ((poll_tick % 3) == 0) {
            if (has_eka_log && check_ingame_process_exited(eka_log, &log_offset, &saw_summon)) {
                Sleep(500);
                InterlockedExchange(&ng_game_live, 0);
                shutdown_eka2l1_gracefully(pi.hProcess, pi.dwProcessId, hJob, "shutdown.ingame_quit");
                clean_triggered = TRUE;
                break;
            }
            if (service_ready &&
                http_local_ex(active_control_port, 600, L"GET", L"/v1/status",
                              resp, sizeof(resp), &http_code)) {
                if (strstr(resp, "\"screenVisible\":true") || strstr(resp, "\"screenVisible\": true")) {
                    saw_screen_visible = TRUE;
                    InterlockedExchange(&ng_game_live, 1);
                } else if (saw_screen_visible &&
                           (strstr(resp, "\"screenVisible\":false") || strstr(resp, "\"screenVisible\": false"))) {
                    InterlockedExchange(&ng_game_live, 0);
                    shutdown_eka2l1_gracefully(pi.hProcess, pi.dwProcessId, hJob, "shutdown.session_closed");
                    clean_triggered = TRUE;
                    break;
                }
            }
        }
    }

    InterlockedExchange(&ng_game_live, 0);
    if (!clean_triggered) {
        settle_and_flush_live_saves();
        if (hJob) {
            DWORD leftover = query_job_active_processes(hJob);
            TerminateJobObject(hJob, 0);
            if (leftover > 0) event("job.cleaned_children", leftover);
        }
    }

    GetExitCodeProcess(pi.hProcess, &result);
    if (clean_triggered && result != 0) result = 0;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    InterlockedExchange((volatile LONG *)&ng_eka_pid, 0);

    persist_saves_backup_after_exit();
    event("eka2l1.exit", result);

done:
    if (hJob) {
        CloseHandle(hJob);
        event("job.closed", 0);
    }
    free(cmd);
    return result;
}

static BOOL WINAPI ng_console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (ng_stop_event) SetEvent(ng_stop_event);
        return TRUE;
    default:
        return FALSE;
    }
}

#include "LauncherUiNG.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous,
                    PWSTR command_line, int show) {
    (void)previous; (void)command_line; (void)show;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    BOOL smoke = FALSE, validate = FALSE, direct = FALSE;
    BOOL probe_dialog = FALSE, import_only = FALSE;
    const WCHAR *fw_src = NULL, *rpkg_src = NULL, *ngage_src = NULL;
    const WCHAR *game_src = NULL, *use_data = NULL, *bundle_src = NULL;
    BOOL fullscreen_override = FALSE;

    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--smoke")) smoke = TRUE;
        else if (!wcscmp(argv[i], L"--validate")) validate = TRUE;
        else if (!wcscmp(argv[i], L"--direct")) direct = TRUE;
        else if (!wcscmp(argv[i], L"--fullscreen")) fullscreen_override = TRUE;
        else if (!wcscmp(argv[i], L"--no-auto-detect")) g_no_auto_detect = TRUE;
        else if (!wcscmp(argv[i], L"--probe-dialog")) probe_dialog = TRUE;
        else if (!wcscmp(argv[i], L"--import-only")) import_only = TRUE;
        else if (!wcscmp(argv[i], L"--import-firmware") && i + 1 < argc) fw_src = argv[++i];
        else if (!wcscmp(argv[i], L"--rpkg") && i + 1 < argc) rpkg_src = argv[++i];
        else if (!wcscmp(argv[i], L"--ngage-sis") && i + 1 < argc) ngage_src = argv[++i];
        else if ((!wcscmp(argv[i], L"--import-game") || !wcscmp(argv[i], L"--import")) && i + 1 < argc)
            game_src = argv[++i];
        else if (!wcscmp(argv[i], L"--import-bundle") && i + 1 < argc) bundle_src = argv[++i];
        else if (!wcscmp(argv[i], L"--use-data-dir") && i + 1 < argc) use_data = argv[++i];
    }

    if (!load_profile_ng()) {
        LocalFree(argv);
        MessageBoxW(NULL,
            L"Perfil app\\launcher-ng.ini não encontrado ou inválido.\n"
            L"Mantenha a estrutura extraída completa.",
            L"Legacy Mobile Forge — N-Gage 2.0", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (fullscreen_override) ng_settings.fullscreen = 1;
    event("start-ng", 0);

    BOOL cli_only = (smoke || validate || probe_dialog || import_only);
    if (!cli_only) {
        ng_instance_mutex = CreateMutexW(NULL, FALSE, L"Local\\LegacyMobileForge_ResidentEvilNG_SingleInstance");
        if (ng_instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing = FindWindowW(L"LegacyMobileForgeNGLauncher", NULL);
            if (existing) {
                ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            event("single_instance.already_running", 0);
            CloseHandle(ng_instance_mutex);
            LocalFree(argv);
            return 0;
        }
    }

    ng_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetConsoleCtrlHandler(ng_console_ctrl_handler, TRUE);

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        event("com.failed", (DWORD)hr);
        if (ng_stop_event) CloseHandle(ng_stop_event);
        if (ng_instance_mutex) CloseHandle(ng_instance_mutex);
        LocalFree(argv);
        return 1;
    }

    DWORD result = 0;
    BOOL op_failed = FALSE;

    if (use_data) {
        if (!import_firmware_source(use_data, NULL, NULL)) op_failed = TRUE;
    }
    if (bundle_src && !op_failed) {
        DWORD expected_uid = (DWORD)wcstoul(game_uid, NULL, 0);
        if (!expected_uid) expected_uid = 0x2001AD82UL;
        NgImportBundle b;
        ZeroMemory(&b, sizeof(b));
        scan_folder_for_bundle(bundle_src, 0, expected_uid, &b);
        WCHAR tmp_notice[256] = {0};
        if (!execute_import_bundle(&b, tmp_notice, ARRAYSIZE(tmp_notice))) op_failed = TRUE;
    }
    if (fw_src && !op_failed) {
        if (!import_firmware_source(fw_src, rpkg_src, ngage_src)) op_failed = TRUE;
    } else if (ngage_src && !fw_src && !op_failed) {
        if (!import_ngage_client_sis(ngage_src)) op_failed = TRUE;
    }
    if (game_src && !op_failed) {
        if (!import_game_package(game_src)) op_failed = TRUE;
    }

    if (probe_dialog) {
        HRESULT phr1 = pick_path_dialog(NULL, TRUE, FALSE, L"Probe", NULL, NULL, 0);
        HRESULT phr2 = pick_multi_files_dialog(NULL, TRUE, L"Probe Multi", NULL, NULL);
        HRESULT phr = SUCCEEDED(phr1) ? phr2 : phr1;
        event("dialog.probe", (DWORD)phr);
        result = SUCCEEDED(phr) ? 0 : 1;
    } else if (import_only) {
        result = op_failed ? 1 : 0;
    } else if (smoke || validate) {
        BOOL ok_eka = FALSE, ok_rom = FALSE, ok_ngc = FALSE, ok_game = FALSE, ok_shd = FALSE, ok_bzl = FALSE;
        BOOL ok = check_backend_files(&ok_eka, &ok_rom, &ok_ngc, &ok_game, &ok_shd, &ok_bzl)
                  && ok_shd && ok_bzl && !op_failed;
        result = ok ? 0 : 1;
        event(smoke ? "smoke" : "validation", result);
    } else {
        result = run_launcher_ui_ng(instance, direct);
    }

    CoUninitialize();
    SetConsoleCtrlHandler(ng_console_ctrl_handler, FALSE);
    if (ng_stop_event) { CloseHandle(ng_stop_event); ng_stop_event = NULL; }
    if (ng_instance_mutex) { CloseHandle(ng_instance_mutex); ng_instance_mutex = NULL; }
    LocalFree(argv);
    return (int)result;
}
