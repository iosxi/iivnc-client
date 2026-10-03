"""iivnc-client のキー・マウスの変換を確かめる。

    python tools/inputcheck.py

iivnc-server を -testsrc static(入力は再現せずログに書くだけ)で動かし、クライアントをつなぐ。
クライアントの窓へ PostMessage で WM_KEYDOWN / WM_MOUSEMOVE などを直接送る
(利用者のキーボード・マウスは使わない)。サーバーのログに届いたキーシム・QEMU の番号・
SendInput の中身を期待値と比べる。窓は前面に出さない。
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SERVER = os.path.join(os.path.dirname(ROOT), 'iivnc-server')
sys.path.insert(0, os.path.join(SERVER, 'tools'))
import test as srv  # noqa: E402

CLIENT = os.path.join(ROOT, 'iivnc-client.exe')
LOG = os.path.join(srv.TEST, 'test.log')
user32 = ctypes.WinDLL('user32', use_last_error=True)
user32.FindWindowW.restype = wintypes.HWND
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]

WM_KEYDOWN, WM_KEYUP, WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_CLOSE = 0x100, 0x101, 0x200, 0x201, 0x202, 0x10


def klp(scan, ext=False, up=False):
    return 1 | (scan << 16) | ((1 << 24) if ext else 0) | ((0xC0000000) if up else 0)


def main():
    if os.path.exists(LOG):
        os.remove(LOG)
    srv.start(['-testsrc', 'static'])
    ini = os.path.join(ROOT, 'build', 'test', 'client.ini')
    os.makedirs(os.path.dirname(ini), exist_ok=True)
    open(ini, 'w', encoding='utf-8').write('log=1\nfit=1\n')
    p = subprocess.Popen([CLIENT, f'127.0.0.1::{srv.PORT}', '-ini', ini, '-idleexit', '600000'])
    try:
        hwnd = None
        for _ in range(50):
            hwnd = user32.FindWindowW('iivnc.Client.View', None)
            if hwnd:
                break
            time.sleep(0.1)
        time.sleep(1.5)                                     # 接続と QEMU キーの取り決めを待つ
        rc = wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rc))
        cw, ch = rc.right, rc.bottom
        post = lambda m, w, l: user32.PostMessageW(hwnd, m, w, l)
        keys = [
            ('A', 0x41, 0x1E, False),
            ('F5', 0x74, 0x3F, False),
            ('右 Ctrl', 0x11, 0x1D, True),
            ('← (矢印)', 0x25, 0x4B, True),
            ('テンキー 4', 0x64, 0x4B, False),
            ('テンキーの Enter', 0x0D, 0x1C, True),
            ('Esc', 0x1B, 0x01, False),
        ]
        for _, vk, sc, ext in keys:
            post(WM_KEYDOWN, vk, klp(sc, ext))
            post(WM_KEYUP, vk, klp(sc, ext, True))
        post(WM_KEYDOWN, 0x10, klp(0x2A))                  # Shift を押したまま A
        post(WM_KEYDOWN, 0x41, klp(0x1E))
        post(WM_KEYUP, 0x41, klp(0x1E, up=True))
        post(WM_KEYUP, 0x10, klp(0x2A, up=True))
        post(WM_KEYDOWN, 0xF3, klp(0x29))                  # 半角/全角(押しただけ)
        # マウス: 窓の中心 → 相手の画面の中心 (960,540) になるはず
        post(WM_MOUSEMOVE, 0, (ch // 2 << 16) | (cw // 2))
        post(WM_LBUTTONDOWN, 1, (ch // 2 << 16) | (cw // 2))
        post(WM_LBUTTONUP, 0, (ch // 2 << 16) | (cw // 2))
        time.sleep(1.0)
        post(WM_CLOSE, 0, 0)
        p.wait(10)
    finally:
        if p.poll() is None:
            p.kill()
        srv.stop()
    got = [l.split('[dryrun-keysym] ', 1)[1].strip() for l in open(LOG, encoding='utf-8') if '[dryrun-keysym]' in l]
    mouse = [l.split('[dryrun] ', 1)[1].strip() for l in open(LOG, encoding='utf-8') if '[dryrun] mouse' in l]
    expect = [
        'down keysym=61 qnum=1E', 'up keysym=61 qnum=1E',
        'down keysym=FFC2 qnum=3F', 'up keysym=FFC2 qnum=3F',
        'down keysym=FFE4 qnum=9D', 'up keysym=FFE4 qnum=9D',
        'down keysym=FF51 qnum=CB', 'up keysym=FF51 qnum=CB',
        'down keysym=FFB4 qnum=4B', 'up keysym=FFB4 qnum=4B',
        'down keysym=FF8D qnum=9C', 'up keysym=FF8D qnum=9C',
        'down keysym=FF1B qnum=1', 'up keysym=FF1B qnum=1',
        'down keysym=FFE1 qnum=2A', 'down keysym=41 qnum=1E', 'up keysym=41 qnum=1E', 'up keysym=FFE1 qnum=2A',
        'down keysym=FF2A qnum=29', 'up keysym=FF2A qnum=29',
    ]
    ok = True
    for i in range(max(len(got), len(expect))):
        g = got[i] if i < len(got) else '(無し)'
        e = expect[i] if i < len(expect) else '(無し)'
        ok &= g == e
        print(f'  {"OK" if g == e else "NG"}  {g:34s}' + ('' if g == e else f'  期待: {e}'))
    absx = ((2 * 960 + 1) * 65536) // (2 * 1920)
    absy = ((2 * 540 + 1) * 65536) // (2 * 1080)
    want = f'mouse flags=C001 dx={absx} dy={absy} data=0'
    print(f'  窓 {cw}x{ch} の中心 → {mouse[:1]}')
    good = bool(mouse) and abs(int(mouse[0].split('dx=')[1].split()[0]) - absx) <= 40 and \
        abs(int(mouse[0].split('dy=')[1].split()[0]) - absy) <= 70 and \
        any('flags=0002' in m for m in mouse) and any('flags=0004' in m for m in mouse)
    ok &= good
    print(f'  {"OK" if good else "NG"}  マウス(中心へ移動・左クリック。期待 {want} 付近)')
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
