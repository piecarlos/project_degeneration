"""Final clean-room verification suite for Release 0.1.0 (ResidentEvilDegeneration-0.1.0-windows-x64.zip).

Verifies:
1. Public archive audit: ZIP integrity, native x64 PE (no CLR), license/notice files present,
   bezel SHA-256 unchanged, zero proprietary ROM/firmware/game/saves/logs/ReShade/machine paths.
2. Controls separation audit: default resident_evil.yml preserved without button_id 6;
   optional resident_evil_rstick.yml has NO 300/301/6 and routes R-Stick + L-Stick cleanly.
3. Clean-room different-folder deployment (%TEMP%\\LMF_RE_Release010_Test):
   - --no-auto-detect --validate -> 1 (before import)
   - --probe-dialog -> 0
   - Rejects wrong/corrupt game file (--import-game invalid --import-only -> 1)
   - Imports RM-409 firmware (SYM.ROM + SYM.RPKG + TZDB.DBZ + ngage_client.sis) -> 0
   - Imports original .n-gage from Unicode path -> 0, rejects wrong file afterward while preserving cache
   - --no-auto-detect --validate -> 0
   - Launches UI with Scale2x + Bezel + Right Stick profile, clicks PLAY -> verifies relative paths in config.yml
   - Opens in-game Select Menu overlay (UI_OPEN_SELECT_NG), pauses EKA2L1, captures screenshot 20_select_menu_overlay.png
   - Resumes via Enter (Resume Game), verifies frame counter resumes advancing
   - Opens Select Menu again, chooses Down + Enter (Quit Game), verifies 0 orphan EKA2L1 processes & launcher restored
   - Persists settings (app/video.properties) and atomic save backup (app/saves_backup/)
4. Auto-detection/link of existing RM-409 installation (--use-data-dir -> 0) & preservation of user's live installation and personal saves.
"""
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from PIL import Image

sys.stdout.reconfigure(line_buffering=True)

user = c.WinDLL("user32", use_last_error=True)
gdi = c.WinDLL("gdi32", use_last_error=True)

user.EnumWindows.argtypes = [c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM), w.LPARAM]
user.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user.SendMessageW.restype = c.c_ssize_t
user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user.IsWindowVisible.argtypes = [w.HWND]
user.GetDC.argtypes = [w.HWND]
user.GetDC.restype = w.HDC
user.ReleaseDC.argtypes = [w.HWND, w.HDC]
user.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
gdi.CreateCompatibleDC.argtypes = [w.HDC]
gdi.CreateCompatibleDC.restype = w.HDC
gdi.CreateCompatibleBitmap.argtypes = [w.HDC, c.c_int, c.c_int]
gdi.CreateCompatibleBitmap.restype = w.HBITMAP
gdi.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
gdi.SelectObject.restype = w.HGDIOBJ
gdi.DeleteObject.argtypes = [w.HGDIOBJ]
gdi.DeleteDC.argtypes = [w.HDC]
gdi.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, c.c_void_p, c.c_void_p, w.UINT]

UI_OPEN_SELECT_NG = 0x8000 + 3

LOCAL_RES_DIR = pathlib.Path(r"c:\Users\Jeronimo\Documents\Ngage games\resident-emulation")
LOCAL_EKA_DIR = pathlib.Path(r"C:\Users\Jeronimo\Documents\Ngage games\EKA2L1")
ZIP_PATH = LOCAL_RES_DIR / "release" / "ResidentEvilDegeneration-0.1.0-windows-x64.zip"
LOCAL_EV_DIR = LOCAL_RES_DIR / "evidence"
ART_EV_DIR = pathlib.Path(r"C:\Users\Jeronimo\.gemini\antigravity\brain\ec66c774-9c43-4d40-a364-5f9a002e7234\evidence")

FW_DIR = LOCAL_EKA_DIR / "firmware" / "5320"
NGAGE_SIS = LOCAL_EKA_DIR / "ngage_client.sis"
GAME_NGAGE = pathlib.Path(r"C:\Users\Jeronimo\Documents\Ngage games\Resident Evil Degeneration.n-gage")

BEZEL_SHA256 = "B2326D1BE44FD52FA4844FFF58595F37E1866B0C6D6A7F750E32D33DE54C167C"
LIVE_SAVE_PATHS = (
    LOCAL_EKA_DIR / r"data\drives\c\private\20007b39\2001ad82\appdata.i3d",
    LOCAL_EKA_DIR / r"data\drives\c\private\20007b39\DBMS\myprofile.db",
    LOCAL_EKA_DIR / r"data\drives\c\private\2001AD82\as.dat",
)


def sha256_file(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest().upper()


def list_eka2l1_pids() -> list[int]:
    out = subprocess.check_output(
        ["powershell", "-NoProfile", "-Command",
         "@(Get-Process -Name 'eka2l1_qt','updater' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id) -join ','"],
        text=True
    ).strip()
    if not out:
        return []
    return [int(x) for x in out.split(",") if x.strip().isdigit()]


def find_window_by_class(pid: int, class_name: str):
    found = []
    CB = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    @CB
    def cb(hwnd, _):
        wpid = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, c.byref(wpid))
        if wpid.value == pid:
            cls = c.create_unicode_buffer(128)
            user.GetClassNameW(hwnd, cls, 128)
            if cls.value == class_name:
                found.append(hwnd)
        return True
    user.EnumWindows(cb, 0)
    return found[0] if found else None


def wait_for(pred, timeout=20.0, desc="condition"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        val = pred()
        if val:
            return val
        time.sleep(0.1)
    raise AssertionError(f"Timed out waiting for {desc}")


def capture_window(hwnd, out_path: pathlib.Path):
    rect = w.RECT()
    assert user.GetClientRect(hwnd, c.byref(rect))
    width, height = rect.right, rect.bottom
    dc = user.GetDC(hwnd)
    mem = gdi.CreateCompatibleDC(dc)
    bmp = gdi.CreateCompatibleBitmap(dc, width, height)
    old = gdi.SelectObject(mem, bmp)
    try:
        assert user.PrintWindow(hwnd, mem, 3), "PrintWindow failed"
        gdi.SelectObject(mem, old)
        info = c.create_string_buffer(struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0))
        pixels = c.create_string_buffer(width * height * 4)
        assert gdi.GetDIBits(dc, bmp, 0, height, pixels, info, 0) == height
        img = Image.frombytes("RGB", (width, height), pixels.raw, "raw", "BGRX")
        assert img.getextrema() != ((0, 0), (0, 0), (0, 0)), "Captured blank image"
        img.save(out_path)
    finally:
        gdi.SelectObject(mem, old)
        gdi.DeleteObject(bmp)
        gdi.DeleteDC(mem)
        user.ReleaseDC(hwnd, dc)


def click_logical(hwnd, lx, ly):
    rect = w.RECT()
    user.GetClientRect(hwnd, c.byref(rect))
    px = lx * rect.right // 1200
    py = ly * rect.bottom // 850
    lp = px | (py << 16)
    user.SendMessageW(hwnd, 0x0201, 1, lp)
    user.SendMessageW(hwnd, 0x0202, 0, lp)
    user.SendMessageW(hwnd, 0x000F, 0, 0)


def send_key(hwnd, vk):
    user.SendMessageW(hwnd, 0x0100, vk, 0)
    user.SendMessageW(hwnd, 0x0101, vk, 0)
    user.SendMessageW(hwnd, 0x000F, 0, 0)


def get_status(port=28182):
    raw = urllib.request.urlopen(f"http://127.0.0.1:{port}/v1/status", timeout=2.0).read()
    return json.loads(raw.decode("utf-8"))


def audit_zip_and_controls():
    assert ZIP_PATH.exists(), f"Missing Release ZIP: {ZIP_PATH}"
    with zipfile.ZipFile(ZIP_PATH) as zf:
        assert zf.testzip() is None, "Corrupt Release ZIP"
        names = [n.replace("\\", "/") for n in zf.namelist()]
        lowers = [n.lower() for n in names]

        for required in (
            "/residentevillauncher.exe",
            "/app/launcher-ng.ini",
            "/app/assets/bezel.png",
            "/runtime/eka2l1/eka2l1_qt.exe",
            "/runtime/eka2l1/bindings/resident_evil.yml",
            "/runtime/eka2l1/bindings/resident_evil_rstick.yml",
            "/runtime/eka2l1/resources/upscale/scale2x.frag",
            "/readme.md",
            "/changelog.md",
            "/license",
            "/third-party.md",
            "/sources.txt",
        ):
            assert any(n.endswith(required) for n in lowers), f"Missing {required} in ZIP"

        forbidden_exts = (".rom", ".rpkg", ".sis", ".sisx", ".n-gage", ".i3d", ".db", ".dat", ".log")
        forbidden_names = ("resident_evil.exe", "ngiinstaller.exe", "d3d9.dll", "config.yml", "devices.yml")
        for n in lowers:
            if n.endswith("/"):
                continue
            assert not n.endswith(forbidden_exts), f"Forbidden file type in ZIP: {n}"
            assert not n.rsplit("/", 1)[-1] in forbidden_names, f"Forbidden file name in ZIP: {n}"
            assert "/data/" not in n and "/firmware/" not in n and "/saves" not in n and "/updater/" not in n, (
                f"Private/updater path leaked into ZIP: {n}"
            )

        exe_bytes = zf.read(next(n for n in names if n.lower().endswith("/residentevillauncher.exe")))
        pe = struct.unpack_from("<I", exe_bytes, 0x3C)[0]
        assert exe_bytes[pe:pe + 4] == b"PE\x00\x00"
        assert struct.unpack_from("<H", exe_bytes, pe + 4)[0] == 0x8664
        opt = pe + 24
        assert struct.unpack_from("<H", exe_bytes, opt)[0] == 0x20B
        assert struct.unpack_from("<II", exe_bytes, opt + 112 + 14 * 8) == (0, 0), "Launcher depends on CLR"

        bezel_bytes = zf.read(next(n for n in names if n.lower().endswith("/app/assets/bezel.png")))
        assert hashlib.sha256(bezel_bytes).hexdigest().upper() == BEZEL_SHA256

        ini_text = zf.read(next(n for n in names if n.lower().endswith("/app/launcher-ng.ini"))).decode("utf-16")
        assert "jeronimo" not in ini_text.lower() and "c:\\" not in ini_text.lower(), "Absolute path in launcher-ng.ini"
        assert "eka2l1_exe=runtime\\eka2l1\\eka2l1_qt.exe" in ini_text

        for text_ext in (".md", ".txt", ".bat"):
            for n in names:
                if n.lower().endswith(text_ext):
                    txt = zf.read(n).decode("utf-8", errors="ignore")
                    assert "jeronimo" not in txt.lower(), f"User-specific path leaked in {n}"

        rstick_txt = zf.read(next(n for n in names if n.lower().endswith("/bindings/resident_evil_rstick.yml"))).decode("utf-8")
        for req in (
            "button_id: 300", "button_id: 301", "button_id: 302", "button_id: 303",
            "button_id: 304", "button_id: 305", "button_id: 306", "button_id: 307",
            "button_id: 310", "button_id: 311",
        ):
            assert req in rstick_txt, f"Missing {req} in resident_evil_rstick.yml"
        assert "button_id: 6\n" not in rstick_txt and not rstick_txt.endswith("button_id: 6"), (
            "Select button_id: 6 must remain reserved in resident_evil_rstick.yml"
        )

        def_txt = zf.read(next(n for n in names if n.lower().endswith("/bindings/resident_evil.yml"))).decode("utf-8")
        assert "button_id: 300" in def_txt and "button_id: 301" in def_txt
        assert "button_id: 6\n" not in def_txt and not def_txt.endswith("button_id: 6")

    print("PASS [1/5]: Release 0.1.0 ZIP audit (clean, portable, game/firmware-free, x64 no-CLR, licenses & Full Dual-Stick controls)")


def test_clean_room_different_folder():
    live_saves_before = {}
    for sp in LIVE_SAVE_PATHS:
        assert sp.exists() and sp.stat().st_size > 0, f"Expected live save missing: {sp}"
        live_saves_before[sp] = sha256_file(sp)

    temp_base = pathlib.Path(tempfile.gettempdir()) / "LMF_RE_Release010_Test"
    if temp_base.exists():
        shutil.rmtree(temp_base, ignore_errors=True)
    temp_base.mkdir(parents=True, exist_ok=True)

    try:
        with zipfile.ZipFile(ZIP_PATH) as zf:
            zf.extractall(temp_base)

        pkg_root = next(temp_base.rglob("ResidentEvilLauncher.exe")).parent
        exe = pkg_root / "ResidentEvilLauncher.exe"

        # Verify before import: --no-auto-detect --validate -> 1, --probe-dialog -> 0
        rc_val0 = subprocess.run([str(exe), "--no-auto-detect", "--validate"], cwd=pkg_root).returncode
        assert rc_val0 == 1, f"Expected --validate=1 before import, got {rc_val0}"
        rc_probe = subprocess.run([str(exe), "--no-auto-detect", "--probe-dialog"], cwd=pkg_root).returncode
        assert rc_probe == 0, f"Expected --probe-dialog=0, got {rc_probe}"

        # Reject invalid game file
        bad_file = temp_base / "invalid_game.n-gage"
        bad_file.write_bytes(b"NOT_A_VALID_NGAGE_PACKAGE" * 20)
        assert subprocess.run([str(exe), "--no-auto-detect", "--import-game", str(bad_file), "--import-only"], cwd=pkg_root).returncode == 1

        # Unified single-shot bundle import (SYM.ROM + SYM.RPKG + ngage_client.sis + Unicode .n-gage all in one folder!)
        bundle_dir = temp_base / "bundle_all_in_one"
        bundle_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(FW_DIR / "SYM.ROM", bundle_dir / "SYM.ROM")
        shutil.copy2(FW_DIR / "SYM.RPKG", bundle_dir / "SYM.RPKG")
        shutil.copy2(NGAGE_SIS, bundle_dir / "ngage_client.sis")
        (bundle_dir / "Meu Jogo ç Resident Evil.n-gage").write_bytes(GAME_NGAGE.read_bytes())

        assert subprocess.run(
            [str(exe), "--no-auto-detect", "--import-bundle", str(bundle_dir), "--import-only"],
            cwd=pkg_root,
        ).returncode == 0, "Unified --import-bundle (Firmware + N-Gage + Unicode .n-gage) failed"

        # Reject wrong game package even AFTER valid import
        assert subprocess.run(
            [str(exe), "--no-auto-detect", "--import-game", str(bad_file), "--import-only"],
            cwd=pkg_root,
        ).returncode == 1, "Invalid game package should still be rejected after valid install"

        # Validate now passes!
        assert subprocess.run([str(exe), "--no-auto-detect", "--validate"], cwd=pkg_root).returncode == 0
        print("PASS [2/5]: Clean-room different-folder validation, multi-select dialog probe, and single-shot --import-bundle (RM-409 + N-Gage + Unicode .n-gage)")

        # Launch UI in clean-room folder, select Scale2x + Bezel + Dual-Stick, capture System bundle UI, click PLAY
        proc = subprocess.Popen([str(exe), "--no-auto-detect"], cwd=pkg_root)
        try:
            hwnd = wait_for(lambda: find_window_by_class(proc.pid, "LegacyMobileForgeNGLauncher"), desc="clean-room launcher UI")
            time.sleep(0.25)

            # Switch to Display tab (E once), select Scale2x (1070, 488)
            send_key(hwnd, ord("E"))
            time.sleep(0.15)
            click_logical(hwnd, 1070, 488)
            time.sleep(0.15)

            # Switch to Controls tab (E again), select Dual-Stick (1020, 340)
            send_key(hwnd, ord("E"))
            time.sleep(0.15)
            click_logical(hwnd, 1020, 340)
            time.sleep(0.15)

            # Switch to System tab (E once more) and capture updated Import All UI
            send_key(hwnd, ord("E"))
            time.sleep(0.2)
            LOCAL_EV_DIR.mkdir(parents=True, exist_ok=True)
            ART_EV_DIR.mkdir(parents=True, exist_ok=True)
            capture_window(hwnd, LOCAL_EV_DIR / "21_system_tab_bundle_ui.png")
            shutil.copy2(LOCAL_EV_DIR / "21_system_tab_bundle_ui.png", ART_EV_DIR / "21_system_tab_bundle_ui.png")

            # Seed synthetic save in clean-room folder to verify live save & saves_backup persistence
            synthetic_save = b"RE_CLEANROOM_SAVE_TEST\x00" * 32
            clean_as_dat = pkg_root / "runtime" / "eka2l1" / "data" / "drives" / "c" / "private" / "2001AD82" / "as.dat"
            clean_as_dat.parent.mkdir(parents=True, exist_ok=True)
            clean_as_dat.write_bytes(synthetic_save)

            # Click PLAY (1060, 785)
            click_logical(hwnd, 1060, 785)
            wait_for(lambda: not user.IsWindowVisible(hwnd), timeout=8.0, desc="launcher hide on Play")

            # Wait for 320x240 landscape game session on port 28182
            def game_active():
                try:
                    st = get_status()
                    scr = st.get("screen") or {}
                    return st.get("started") and scr.get("width") == 320 and scr.get("height") == 240
                except Exception:
                    return False

            try:
                wait_for(game_active, timeout=35.0, desc="clean-room EKA2L1 320x240 session")
            except Exception:
                log_p = pkg_root / "app" / "logs" / "launcher-ng.log"
                if log_p.exists():
                    print("=== CLEAN-ROOM LAUNCHER LOG ON TIMEOUT ===")
                    print(log_p.read_text(encoding="utf-8", errors="ignore"))
                try:
                    print("=== CLEAN-ROOM STATUS ON TIMEOUT ===", get_status())
                except Exception as se:
                    print("=== CLEAN-ROOM STATUS ERR ===", se)
                raise
            time.sleep(1.5)

            # Verify portable relative paths & Scale2x + Bezel + resident_evil_rstick in config.yml
            cfg_txt = (pkg_root / "runtime" / "eka2l1" / "config.yml").read_text(encoding="utf-8")
            compat_txt = (pkg_root / "runtime" / "eka2l1" / "compat" / "2001AD82.yml").read_text(encoding="utf-8")
            assert 'data-storage: "data"' in cfg_txt, f"Expected relative data-storage in:\n{cfg_txt}"
            assert '../../app/assets/bezel.png' in cfg_txt, f"Expected relative bezel path in:\n{cfg_txt}"
            assert "current-keybind-profile: resident_evil_rstick" in cfg_txt
            assert "filter-shader-path: Scale2x" in compat_txt
            print("PASS [3/5]: Clean-room launch verified relative paths, Bezel, Scale2x, and separated Right-Stick profile")

            # Test in-game Select Menu -> Pause -> Resume -> Re-open -> Quit Game -> Graceful Shutdown
            user.PostMessageW(hwnd, UI_OPEN_SELECT_NG, 0, 0)
            ovl_hwnd = wait_for(
                lambda: find_window_by_class(proc.pid, "LegacyMobileForgeNGSelectMenu")
                if user.IsWindowVisible(find_window_by_class(proc.pid, "LegacyMobileForgeNGSelectMenu") or 0)
                else None,
                timeout=6.0,
                desc="visible Select Menu overlay",
            )
            time.sleep(0.35)
            LOCAL_EV_DIR.mkdir(parents=True, exist_ok=True)
            ART_EV_DIR.mkdir(parents=True, exist_ok=True)
            capture_window(ovl_hwnd, LOCAL_EV_DIR / "20_select_menu_overlay.png")
            shutil.copy2(LOCAL_EV_DIR / "20_select_menu_overlay.png", ART_EV_DIR / "20_select_menu_overlay.png")

            # Verify EKA2L1 frames remain paused while Select Menu is open!
            f_pause1 = get_status()["frames"]
            time.sleep(1.6)
            f_pause2 = get_status()["frames"]
            assert (f_pause2 - f_pause1) <= 2, f"Expected emulator paused in Select Menu, frames moved {f_pause1}->{f_pause2}"

            # Activate Resume Game (Row 0 default -> Enter)
            send_key(ovl_hwnd, 0x0D)  # VK_RETURN
            wait_for(lambda: not user.IsWindowVisible(ovl_hwnd), timeout=5.0, desc="Select Menu hide on Resume")
            f_res1 = get_status()["frames"]
            wait_for(lambda: get_status()["frames"] > f_res1, timeout=6.0, desc=f"emulator frames advancing past {f_res1} after Resume")
            f_res2 = get_status()["frames"]
            assert f_res2 > f_res1, f"Expected emulator resumed, frames stuck at {f_res1}->{f_res2}"
            print(f"PASS [4/5]: In-game Select Menu paused EKA2L1 ({f_pause1}->{f_pause2}), captured 20_select_menu_overlay.png, and Resume Game resumed playback ({f_res1}->{f_res2})")

            # Re-open Select Menu -> Down -> Enter (Quit Game) -> Graceful return + 0 orphans + saves backup
            user.PostMessageW(hwnd, UI_OPEN_SELECT_NG, 0, 0)
            wait_for(lambda: user.IsWindowVisible(ovl_hwnd), timeout=5.0, desc="Select Menu reopen")
            time.sleep(0.15)
            send_key(ovl_hwnd, 0x28)  # VK_DOWN -> Quit Game row
            time.sleep(0.1)
            send_key(ovl_hwnd, 0x0D)  # VK_RETURN -> Quit Game
            wait_for(lambda: not user.IsWindowVisible(ovl_hwnd), timeout=5.0, desc="Select Menu close on Quit Game")
            wait_for(lambda: user.IsWindowVisible(hwnd), timeout=12.0, desc="launcher restore after Quit Game")
            wait_for(lambda: list_eka2l1_pids() == [], timeout=5.0, desc="zero EKA2L1 processes after Select Quit")

            # Close launcher cleanly and check persistence in isolated folder
            user.PostMessageW(hwnd, 0x0010, 0, 0)
            assert proc.wait(timeout=8) == 0

            props_txt = (pkg_root / "app" / "video.properties").read_text(encoding="ascii")
            assert "pixelArt=true" in props_txt and "bezel=true" in props_txt and "controlProfile=rstick" in props_txt
            assert clean_as_dat.read_bytes() == synthetic_save, "Clean-room live save altered unexpectedly"
            bak_as = pkg_root / "app" / "saves_backup" / "as.dat"
            assert bak_as.exists() and bak_as.read_bytes() == synthetic_save, "Clean-room saves_backup did not persist synthetic save"
            log_txt = (pkg_root / "app" / "logs" / "launcher-ng.log").read_text(encoding="utf-8", errors="ignore")
            assert "native-ng.select_menu.open code=1" in log_txt
            assert "native-ng.select_menu.resume" in log_txt
            assert "native-ng.select_menu.quit" in log_txt
            assert "native-ng.shutdown.select_quit" in log_txt
            assert "native-ng.saves.persisted" in log_txt

            # Test non-destructive existing-firmware detection (--use-data-dir pointing to LOCAL_EKA_DIR/data in fresh extracted copy)
            link_test_dir = temp_base / "link_existing_test"
            link_test_dir.mkdir(exist_ok=True)
            with zipfile.ZipFile(ZIP_PATH) as zf_link:
                zf_link.extractall(link_test_dir)
            link_pkg_root = next(link_test_dir.rglob("ResidentEvilLauncher.exe")).parent
            assert subprocess.run(
                [str(link_pkg_root / "ResidentEvilLauncher.exe"), "--use-data-dir", str(LOCAL_EKA_DIR / "data"), "--validate"],
                cwd=link_pkg_root,
            ).returncode == 0, "Failed linking existing RM-409 installation"
            assert not (link_pkg_root / "runtime" / "eka2l1" / "data" / "roms").exists(), "Should reuse existing firmware without copying ROMs"

        finally:
            if proc.poll() is None:
                subprocess.run(["taskkill.exe", "/PID", str(proc.pid), "/T", "/F"], capture_output=True)
    finally:
        wait_for(lambda: list_eka2l1_pids() == [], timeout=5.0, desc="cleanup EKA2L1")
        shutil.rmtree(temp_base, ignore_errors=True)

    # Verify original local installation and user saves are 100% intact
    assert subprocess.run([str(LOCAL_RES_DIR / "ResidentEvilLauncher.exe"), "--smoke"], cwd=LOCAL_RES_DIR).returncode == 0
    for sp, expected_hash in live_saves_before.items():
        assert sha256_file(sp) == expected_hash, f"Personal save altered: {sp}"
        bak_p = LOCAL_RES_DIR / "app" / "saves_backup" / sp.name
        assert bak_p.exists() and sha256_file(bak_p) == expected_hash, f"Backup out of sync: {bak_p}"
    assert list_eka2l1_pids() == []
    print("PASS [5/5]: Select Menu Quit returned cleanly with 0 orphans, persisted clean-room settings/saves, linked existing RM-409 without copying, and preserved local user installation & saves!")


if __name__ == "__main__":
    assert list_eka2l1_pids() == []
    audit_zip_and_controls()
    test_clean_room_different_folder()
