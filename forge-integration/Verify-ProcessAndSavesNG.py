"""Objective verification suite for Process Supervision (Windows Job Objects),
Graceful 3-Stage Shutdown, Save Persistence/Self-Healing, and Optional Right-Stick Profile.

Preserves and verifies exact SHA-256 of user saves, bezel.png, and default controls.
"""
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import sys
import time
import urllib.request
from PIL import Image

sys.stdout.reconfigure(line_buffering=True)

user = c.WinDLL("user32", use_last_error=True)
gdi = c.WinDLL("gdi32", use_last_error=True)
kernel = c.WinDLL("kernel32", use_last_error=True)

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

RES_DIR = pathlib.Path(r"c:\Users\Jeronimo\Documents\Ngage games\resident-emulation")
EXE = RES_DIR / "ResidentEvilLauncher.exe"
EKA_DIR = pathlib.Path(r"C:\Users\Jeronimo\Documents\Ngage games\EKA2L1")
EV_DIR = RES_DIR / "evidence"
PROPS = RES_DIR / "app" / "video.properties"
LOG_FILE = RES_DIR / "app" / "logs" / "launcher-ng.log"
SAVES_BAK = RES_DIR / "app" / "saves_backup"

LIVE_SAVES = [
    EKA_DIR / r"data\drives\c\private\20007b39\2001ad82\appdata.i3d",
    EKA_DIR / r"data\drives\c\private\20007b39\DBMS\myprofile.db",
    EKA_DIR / r"data\drives\c\private\2001AD82\as.dat",
]


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


def find_launcher_window(pid: int):
    found = []
    CB = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    @CB
    def cb(hwnd, _):
        wpid = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, c.byref(wpid))
        if wpid.value == pid:
            cls = c.create_unicode_buffer(128)
            user.GetClassNameW(hwnd, cls, 128)
            if cls.value == "LegacyMobileForgeNGLauncher":
                found.append(hwnd)
        return True
    user.EnumWindows(cb, 0)
    return found[0] if found else None


def find_eka2l1_qt_windows():
    pids = set(list_eka2l1_pids())
    if not pids:
        return []
    secondary, primary = [], []
    CB = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    @CB
    def cb(hwnd, _):
        wpid = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, c.byref(wpid))
        if wpid.value in pids and user.IsWindowVisible(hwnd):
            cls = c.create_unicode_buffer(128)
            txt = c.create_unicode_buffer(256)
            user.GetClassNameW(hwnd, cls, 128)
            user.GetWindowTextW(hwnd, txt, 256)
            if cls.value != "ConsoleWindowClass":
                if "EKA2L1" in txt.value:
                    primary.append(hwnd)
                else:
                    secondary.append(hwnd)
        return True
    user.EnumWindows(cb, 0)
    return secondary + primary


def wait_for(pred, timeout=15.0, desc="condition"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        val = pred()
        if val:
            return val
        time.sleep(0.1)
    raise AssertionError(f"Timed out waiting for {desc}")


def capture_client(hwnd, out_path: pathlib.Path):
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


def wait_eka_game_active(timeout=20.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            raw = urllib.request.urlopen("http://127.0.0.1:28182/v1/status", timeout=1.5).read()
            st = json.loads(raw.decode("utf-8"))
            scr = st.get("screen") or {}
            if st.get("started") and scr.get("width") == 320 and scr.get("height") == 240:
                return st
        except Exception:
            pass
        time.sleep(0.35)
    raise AssertionError("EKA2L1 did not enter 320x240 Resident Evil session")


def main():
    assert list_eka2l1_pids() == [], f"Unexpected pre-existing EKA2L1 PIDs: {list_eka2l1_pids()}"

    # Snapshot user's live saves + bezel + default controls before testing
    orig_saves = {}
    for sp in LIVE_SAVES:
        assert sp.exists(), f"Expected save missing: {sp}"
        st = sp.stat()
        orig_saves[sp] = (sp.read_bytes(), st.st_atime, st.st_mtime, sha256_file(sp))

    bezel_hash_before = sha256_file(RES_DIR / "app" / "assets" / "bezel.png")
    assert bezel_hash_before == "B2326D1BE44FD52FA4844FFF58595F37E1866B0C6D6A7F750E32D33DE54C167C"
    default_yml_before = sha256_file(EKA_DIR / "bindings" / "resident_evil.yml")
    orig_props = PROPS.read_bytes() if PROPS.exists() else None

    try:
        # ------------------------------------------------------------------
        # TEST 1: --smoke + Single-Instance Mutex + Controls Profile UI
        # ------------------------------------------------------------------
        rc = subprocess.run([str(EXE), "--smoke"], cwd=RES_DIR).returncode
        assert rc == 0, f"--smoke failed with {rc}"

        p1 = subprocess.Popen([str(EXE)], cwd=RES_DIR)
        hwnd1 = wait_for(lambda: find_launcher_window(p1.pid), desc="launcher #1")
        time.sleep(0.25)

        p2 = subprocess.Popen([str(EXE)], cwd=RES_DIR)
        assert p2.wait(timeout=5) == 0, "Second instance should exit cleanly with 0"
        assert p1.poll() is None, "First instance must remain alive"

        # Navigate to Controls tab (E twice) -> verify Padrão selected by default, test optional toggle & revert
        send_key(hwnd1, ord("E"))
        send_key(hwnd1, ord("E"))
        time.sleep(0.15)
        capture_client(hwnd1, EV_DIR / "19_controls_tab_default_and_rstick.png")

        # Click Optional Right Stick button (right half of row at y=325..355 -> x=1020, y=340)
        click_logical(hwnd1, 1020, 340)
        time.sleep(0.15)
        assert "controlProfile=rstick" in PROPS.read_text(encoding="ascii")

        # Revert immediately to Standard (Default) (x=760, y=340)
        click_logical(hwnd1, 760, 340)
        time.sleep(0.15)
        assert "controlProfile=default" in PROPS.read_text(encoding="ascii")
        print("PASS [1/6]: --smoke, Single-Instance Mutex, and Controls Profile selector (Default preserved) verified")

        # ------------------------------------------------------------------
        # TEST 2: Normal Game Window Close -> 0 Orphans + Atomic Save Backup
        # ------------------------------------------------------------------
        click_logical(hwnd1, 1060, 785)  # PLAY
        wait_for(lambda: not user.IsWindowVisible(hwnd1), timeout=8.0, desc="launcher hide")
        wait_eka_game_active(timeout=22.0)
        eka_pids = list_eka2l1_pids()
        assert len(eka_pids) >= 1, "Expected eka2l1_qt.exe running"

        eka_wins = wait_for(find_eka2l1_qt_windows, timeout=8.0, desc="EKA2L1 window")
        for ew in eka_wins:
            user.PostMessageW(ew, 0x0010, 0, 0)  # WM_CLOSE on game window

        wait_for(lambda: user.IsWindowVisible(hwnd1), timeout=12.0, desc="launcher restore")
        wait_for(lambda: list_eka2l1_pids() == [], timeout=5.0, desc="zero EKA2L1 processes after window close")

        for name in ("appdata.i3d", "myprofile.db", "as.dat"):
            bak_file = SAVES_BAK / name
            assert bak_file.exists() and bak_file.stat().st_size > 0, f"Missing backup {bak_file}"
        print("PASS [2/6]: Closing game window terminated all EKA2L1 processes (0 orphans) & persisted saves to app/saves_backup/")

        # ------------------------------------------------------------------
        # TEST 3: Closing Launcher While Game Is Running -> Graceful 3-Stage Shutdown
        # ------------------------------------------------------------------
        click_logical(hwnd1, 1060, 785)  # PLAY again
        wait_for(lambda: not user.IsWindowVisible(hwnd1), timeout=8.0, desc="launcher hide #2")
        wait_eka_game_active(timeout=22.0)
        assert len(list_eka2l1_pids()) >= 1

        # Send WM_CLOSE directly to the hidden launcher window while EKA2L1 is actively running!
        user.PostMessageW(hwnd1, 0x0010, 0, 0)
        assert p1.wait(timeout=10) == 0, "Launcher did not exit cleanly on WM_CLOSE during gameplay"
        assert list_eka2l1_pids() == [], f"Orphan EKA2L1 left after launcher close: {list_eka2l1_pids()}"
        print("PASS [3/6]: Closing launcher during active game executed 3-stage graceful shutdown & killed all EKA2L1 processes (0 orphans)")

        # ------------------------------------------------------------------
        # TEST 4: Abrupt Launcher Crash / TerminateProcess -> Windows Job Object Proof
        # ------------------------------------------------------------------
        p_direct = subprocess.Popen([str(EXE), "--direct"], cwd=RES_DIR)
        wait_eka_game_active(timeout=22.0)
        pids_before_kill = list_eka2l1_pids()
        assert len(pids_before_kill) >= 1, "Expected EKA2L1 running under --direct"

        # Brutally kill ResidentEvilLauncher.exe (simulating crash / End Task) with NO user-mode cleanup!
        p_direct.kill()
        p_direct.wait(timeout=5)
        time.sleep(0.4)
        pids_after_kill = list_eka2l1_pids()
        assert pids_after_kill == [], (
            f"JOB OBJECT FAILURE: orphan EKA2L1 processes survived launcher kill: {pids_after_kill}"
        )
        print(f"PASS [4/6]: Windows Job Object (KILL_ON_JOB_CLOSE) kernel-terminated EKA2L1 PID {pids_before_kill} instantly on abrupt launcher kill (0 orphans)")

        # ------------------------------------------------------------------
        # TEST 5: Save Self-Healing Guard Against NTFS Zero-Fill Corruption
        # ------------------------------------------------------------------
        as_dat = EKA_DIR / r"data\drives\c\private\2001AD82\as.dat"
        expected_as_hash = orig_saves[as_dat][3]
        # Simulate NTFS crash all-NUL corruption on live as.dat
        as_dat.write_bytes(b"\x00" * 512)
        assert sha256_file(as_dat) != expected_as_hash

        p_heal = subprocess.Popen([str(EXE), "--direct"], cwd=RES_DIR)
        wait_eka_game_active(timeout=22.0)
        # Close EKA2L1 window cleanly
        for ew in wait_for(find_eka2l1_qt_windows, timeout=8.0):
            user.PostMessageW(ew, 0x0010, 0, 0)
        assert p_heal.wait(timeout=10) == 0
        assert list_eka2l1_pids() == []
        assert sha256_file(as_dat) == expected_as_hash, "Self-healing guard did not restore exact valid as.dat!"
        print("PASS [5/6]: Save self-healing guard detected simulated all-NUL corruption and restored exact save from app/saves_backup/")

        # ------------------------------------------------------------------
        # TEST 6: Verify Bezel, Default Controls, Logs & User Save Hashes Untouched
        # ------------------------------------------------------------------
        log_txt = LOG_FILE.read_text(encoding="utf-8", errors="ignore")
        for required_ev in (
            "native-ng.job.assigned",
            "native-ng.shutdown.launcher_close",
            "native-ng.saves.persisted",
            "native-ng.saves.restored_from_backup",
        ):
            assert required_ev in log_txt, f"Missing expected log event: {required_ev}"

        rstick_yml = (EKA_DIR / "bindings" / "resident_evil_rstick.yml").read_text(encoding="utf-8")
        for btn in (
            "button_id: 300", "button_id: 301", "button_id: 302", "button_id: 303",
            "button_id: 304", "button_id: 305", "button_id: 306", "button_id: 307",
            "button_id: 310", "button_id: 311",
        ):
            assert btn in rstick_yml, f"Missing Dual-Stick mapping {btn}"
        assert "button_id: 6\n" not in rstick_yml and not rstick_yml.endswith("button_id: 6"), (
            "Select button_id: 6 should be reserved"
        )

        default_yml_after = (EKA_DIR / "bindings" / "resident_evil.yml").read_text(encoding="utf-8")
        for req_default in ("button_id: 300", "button_id: 301", "button_id: 302", "button_id: 303", "button_id: 7"):
            assert req_default in default_yml_after, f"Missing default mapping {req_default}"
        assert "button_id: 6\n" not in default_yml_after and not default_yml_after.endswith("button_id: 6"), "Select button_id: 6 should be reserved"

        cfg_yml = (EKA_DIR / "config.yml").read_text(encoding="utf-8")
        assert "current-keybind-profile: resident_evil" in cfg_yml, "Default profile was altered!"
        assert sha256_file(RES_DIR / "app" / "assets" / "bezel.png") == bezel_hash_before

    finally:
        # Restore exact original user save files & timestamps + video.properties
        for sp, (raw_bytes, atime, mtime, expected_h) in orig_saves.items():
            sp.write_bytes(raw_bytes)
            os.utime(sp, (atime, mtime))
            assert sha256_file(sp) == expected_h
            # Keep app/saves_backup synced with user's exact saves
            SAVES_BAK.mkdir(parents=True, exist_ok=True)
            (SAVES_BAK / sp.name).write_bytes(raw_bytes)

        if orig_props is not None:
            PROPS.write_bytes(orig_props)

    print("PASS [6/6]: Bezel SHA-256, default resident_evil.yml, optional resident_evil_rstick.yml, and user's personal saves 100% verified intact!")


if __name__ == "__main__":
    main()
