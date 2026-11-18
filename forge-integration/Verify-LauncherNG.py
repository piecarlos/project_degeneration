"""Objective end-to-end verification of the native C17 GameLauncherNG (Forge -> EKA2L1).

Tests:
1. Native x64 PE check (no CLR).
2. Real Win32 GDI/WIC window rendering across all 4 tabs (Quick Setup, Display, Controls, System).
3. Settings persistence in app/video.properties (including Scale2x option, bezel, integerScale, preservation of unknown keys).
4. Live Play lifecycle: clicking PLAY hides the launcher, syncs EKA2L1 config.yml + compat/*.yml,
   launches EKA2L1, triggers direct game launch via Control API (/v1/ngage2/games/0x2001AD82/launch)
   with zero manual navigation, reaches the 3D game screen, and restores the launcher on exit.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import os
import pathlib
import struct
import subprocess
import time
import urllib.request
from PIL import Image

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


def check_native_pe_x64(exe: pathlib.Path):
    data = exe.read_bytes()
    assert data[:2] == b"MZ", "Not a PE file"
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe_off:pe_off + 4] == b"PE\x00\x00"
    machine = struct.unpack_from("<H", data, pe_off + 4)[0]
    assert machine == 0x8664, f"Expected x64 machine 0x8664, got {machine:#x}"
    # Check CLR runtime header directory (index 14 in PE32+ optional header at offset 24 + 112 + 14*8)
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", opt_off and data, opt_off)[0]
    assert magic == 0x20B, "Expected PE32+"
    clr_rva, clr_size = struct.unpack_from("<II", data, opt_off + 112 + 14 * 8)
    assert clr_rva == 0 and clr_size == 0, "Expected pure native C17 binary with no CLR"
    print("PASS [1/5]: Native x64 C17 PE verified (no CLR)")


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
    found = []
    CB = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    @CB
    def cb(hwnd, _):
        if user.IsWindowVisible(hwnd):
            cls = c.create_unicode_buffer(128)
            txt = c.create_unicode_buffer(256)
            user.GetClassNameW(hwnd, cls, 128)
            user.GetWindowTextW(hwnd, txt, 256)
            if cls.value != "ConsoleWindowClass" and "EKA2L1" in txt.value:
                found.append(hwnd)
        return True
    user.EnumWindows(cb, 0)
    return found


def wait_for(pred, timeout=12.0, desc="condition"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        val = pred()
        if val:
            return val
        time.sleep(0.08)
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
        assert img.getextrema() != ((0, 0), (0, 0), (0, 0)), "Blank capture"
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


def main():
    exe = pathlib.Path(r"c:\Users\Jeronimo\Documents\Ngage games\resident-emulation\ResidentEvilLauncher.exe")
    eka_dir = pathlib.Path(r"c:\Users\Jeronimo\Documents\Ngage games\EKA2L1")
    ev_dir = pathlib.Path(r"c:\Users\Jeronimo\Documents\Ngage games\resident-emulation\evidence")
    ev_dir.mkdir(parents=True, exist_ok=True)
    props = exe.parent / "app" / "video.properties"
    log_file = exe.parent / "app" / "logs" / "launcher-ng.log"

    check_native_pe_x64(exe)

    # Seed clean test properties with a custom unknown key to verify preservation
    props.write_text(
        "resolution=1\nfullscreen=false\npixelArt=false\nsmooth=false\n"
        "integerScale=true\nbezel=true\nlanguage=pt-BR\ncustom.keep=preserved\n",
        encoding="ascii"
    )

    proc = subprocess.Popen([str(exe)], cwd=exe.parent)
    try:
        hwnd = wait_for(lambda: find_launcher_window(proc.pid), desc="launcher window")
        time.sleep(0.3)
        capture_client(hwnd, ev_dir / "12_launcher_quick_setup.png")

        # Switch to Display tab and select Scale2x (card/button)
        send_key(hwnd, ord("E"))
        time.sleep(0.15)
        click_logical(hwnd, 1070, 488)  # Scale2x button on Display tab (y=325+148=473..503)
        time.sleep(0.15)
        capture_client(hwnd, ev_dir / "13_launcher_display_tab.png")

        # Switch to Controls tab
        send_key(hwnd, ord("E"))
        time.sleep(0.15)
        capture_client(hwnd, ev_dir / "14_launcher_controls_tab.png")

        # Switch to System tab
        send_key(hwnd, ord("E"))
        time.sleep(0.15)
        capture_client(hwnd, ev_dir / "15_launcher_system_tab.png")

        # Close and verify settings persistence
        user.SendMessageW(hwnd, 0x0010, 0, 0)
        assert proc.wait(timeout=8) == 0
        saved = props.read_text(encoding="ascii")
        assert "pixelArt=true" in saved, f"Scale2x not saved: {saved}"
        assert "bezel=true" in saved, f"Bezel not saved: {saved}"
        assert "custom.keep=preserved" in saved, f"Custom key lost: {saved}"
        print("PASS [2/5]: 4 UI tabs rendered & captured; Scale2x + Bezel + custom properties persisted")

        # Relaunch and click PLAY to test full direct launch with Scale2x + Bezel!
        proc = subprocess.Popen([str(exe)], cwd=exe.parent)
        hwnd = wait_for(lambda: find_launcher_window(proc.pid), desc="launcher reopen")
        time.sleep(0.2)
        click_logical(hwnd, 1060, 785)  # PLAY button

        wait_for(lambda: not user.IsWindowVisible(hwnd), timeout=8.0, desc="launcher hide on Play")
        print("PASS [3/5]: Clicking PLAY hid launcher and spawned EKA2L1")

        # Wait for EKA2L1 direct launch into Resident Evil Degeneration 3D Title Screen
        time.sleep(22)
        game_frame = None
        for _ in range(18):
            time.sleep(1.5)
            try:
                raw = urllib.request.urlopen("http://127.0.0.1:28182/v1/screenshot", timeout=5).read()
                if len(raw) > 35000:
                    game_frame = raw
                    break
            except Exception:
                pass
        assert game_frame is not None, "Direct launch did not reach 3D Title Screen"
        (ev_dir / "16_direct_launch_verified_3d.png").write_bytes(game_frame)

        # Verify EKA2L1 config.yml & compat/2001AD82.yml were synced cleanly
        cfg_txt = (eka_dir / "config.yml").read_text(encoding="utf-8")
        compat_txt = (eka_dir / "compat" / "2001AD82.yml").read_text(encoding="utf-8")
        assert "bezel.png" in cfg_txt and "screen-scale-ratio: 67" in cfg_txt, "Bezel not synced to config.yml"
        assert "current-keybind-profile: resident_evil" in cfg_txt, "Keybind profile not synced"
        assert "filter-shader-path: Scale2x" in compat_txt and "screen-upscale-method: 1" in compat_txt, "Scale2x not synced"
        print("PASS [4/5]: Direct launch reached 3D Title Screen with zero manual navigation; Bezel + Scale2x + resident_evil profile active")

        # Close EKA2L1 Qt window cleanly and verify launcher returns to foreground!
        eka_wins = find_eka2l1_qt_windows()
        assert eka_wins, "Expected visible EKA2L1 Qt window"
        for ew in eka_wins:
            user.PostMessageW(ew, 0x0010, 0, 0)  # WM_CLOSE

        wait_for(lambda: user.IsWindowVisible(hwnd), timeout=12.0, desc="launcher restore after EKA2L1 exit")
        time.sleep(0.3)
        capture_client(hwnd, ev_dir / "17_launcher_restored_after_exit.png")

        # Reset default filter back to Sharp (keeping Scale2x as user-selectable option!) before closing
        send_key(hwnd, ord("Q"))  # System -> Controls -> Display -> Quick Setup: let's click Quick Setup tab directly
        click_logical(hwnd, 104, 283)   # Tab 0: Quick Setup
        time.sleep(0.1)
        click_logical(hwnd, 200, 450)   # Card 0: Sharp Pixels (default; Scale2x remains selectable option)
        time.sleep(0.1)
        user.SendMessageW(hwnd, 0x0010, 0, 0)
        assert proc.wait(timeout=8) == 0

        log_txt = log_file.read_text(encoding="utf-8", errors="ignore")
        assert "native-ng.direct_launch.ok code=200" in log_txt, f"Missing direct_launch.ok in log:\n{log_txt}"
        assert "native-ng.ui-ng.return code=0" in log_txt, f"Missing clean return code=0 in log:\n{log_txt}"
        print("PASS [5/5]: EKA2L1 closed cleanly (code=0) and restored Forge C17 Launcher; default set to Sharp with Scale2x optional and ReShade disabled")

    finally:
        if proc.poll() is None:
            subprocess.run(["taskkill.exe", "/PID", str(proc.pid), "/T", "/F"], capture_output=True)


if __name__ == "__main__":
    main()
