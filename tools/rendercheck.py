"""描画の方式(GDI / GPU)を、つないだまま切り替えても絵が出るか確かめる。

    python tools/rendercheck.py

iivnc-server を -testsrc static・ポート 5998 で動かし、まっさらな ini でクライアントをつなぐ
(既定の GDI になるはず)。窓のシステム メニューの「描画」と同じ WM_SYSCOMMAND を送って
GDI → GPU → GDI と切り替え、そのたびに窓の中身を PrintWindow(PW_RENDERFULLCONTENT)で
撮って、最初の GDI の絵と比べる。専用メモリも測る。マウスが親の窓に届くか(子窓に吸われないか)も
WindowFromPoint で見る(そのときだけ窓を最前面にする)。キーもマウスも送らない。
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SERVER = os.path.join(os.path.dirname(ROOT), 'iivnc-server')
CLIENT = os.path.join(ROOT, 'iivnc-client.exe')
SEXE = os.path.join(SERVER, 'iivnc-server.exe')
TEST = os.path.join(ROOT, 'build', 'test')
IDM_RENDER_GDI, IDM_RENDER_GPU = 0x10E, 0x10F

u = ctypes.WinDLL('user32', use_last_error=True)
g = ctypes.WinDLL('gdi32')
u.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
u.FindWindowW.restype = wintypes.HWND
u.GetDC.restype = wintypes.HDC
g.CreateCompatibleDC.restype = wintypes.HDC
g.CreateCompatibleDC.argtypes = [wintypes.HDC]
g.CreateDIBSection.restype = wintypes.HBITMAP
g.CreateDIBSection.argtypes = [wintypes.HDC, ctypes.c_void_p, wintypes.UINT, ctypes.c_void_p, wintypes.HANDLE, wintypes.DWORD]
g.DeleteObject.argtypes = [wintypes.HGDIOBJ]
g.DeleteDC.argtypes = [wintypes.HDC]
u.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
g.SelectObject.restype = wintypes.HGDIOBJ
g.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
u.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
u.WindowFromPoint.restype = wintypes.HWND
u.WindowFromPoint.argtypes = [wintypes.POINT]
u.FindWindowExW.restype = wintypes.HWND
u.FindWindowExW.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
u.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT]
u.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]


class BIH(ctypes.Structure):
    _fields_ = [('biSize', wintypes.DWORD), ('biWidth', ctypes.c_long), ('biHeight', ctypes.c_long),
                ('biPlanes', wintypes.WORD), ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD),
                ('biSizeImage', wintypes.DWORD), ('a', ctypes.c_long), ('b', ctypes.c_long),
                ('c', wintypes.DWORD), ('d', wintypes.DWORD)]


def grab(h):
    """窓の中身(枠を除く)を BGRA のバイト列で返す"""
    rc = wintypes.RECT()
    u.GetClientRect(h, ctypes.byref(rc))
    wr = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(wr))
    pt = wintypes.POINT(0, 0)
    u.ClientToScreen(h, ctypes.byref(pt))
    ww, wh = wr.right - wr.left, wr.bottom - wr.top
    sdc = u.GetDC(None)
    dc = g.CreateCompatibleDC(sdc)
    bi = BIH(ctypes.sizeof(BIH), ww, -wh, 1, 32, 0, 0, 0, 0, 0, 0)
    bits = ctypes.c_void_p()
    bmp = g.CreateDIBSection(dc, ctypes.byref(bi), 0, ctypes.byref(bits), None, 0)
    old = g.SelectObject(dc, bmp)
    u.PrintWindow(h, dc, 2)                         # PW_RENDERFULLCONTENT
    raw = ctypes.string_at(bits, ww * wh * 4)
    g.SelectObject(dc, old)
    g.DeleteObject(bmp)
    g.DeleteDC(dc)
    u.ReleaseDC(None, sdc)
    ox, oy, cw, ch = pt.x - wr.left, pt.y - wr.top, rc.right, rc.bottom
    rows = [raw[((oy + y) * ww + ox) * 4:((oy + y) * ww + ox + cw) * 4] for y in range(ch)]
    return cw, ch, b''.join(rows)


def stats(img):
    w, h, b = img
    step = 4 * 97                                    # 間引いて数える
    px = [b[i:i + 3] for i in range(0, len(b) - 3, step)]
    mean = sum(sum(p) for p in px) / (len(px) * 3)
    return mean, len(set(px))


def diff(a, b):
    if a[:2] != b[:2]:
        return 255.0
    x, y = a[2], b[2]
    n = 0
    s = 0
    for i in range(0, len(x), 4 * 31):
        s += abs(x[i] - y[i]) + abs(x[i + 1] - y[i + 1]) + abs(x[i + 2] - y[i + 2])
        n += 3
    return s / n


def private_mb(pid):
    out = subprocess.run(['powershell', '-NoProfile', '-Command', f'(Get-Process -Id {pid}).PrivateMemorySize64'],
                         capture_output=True, text=True).stdout.strip()
    return int(out) / 1048576 if out.isdigit() else -1


def main():
    os.makedirs(TEST, exist_ok=True)
    sini = os.path.join(TEST, 'rc-server.ini')
    open(sini, 'w', encoding='utf-8', newline='\n').write('port=5998\nlisten=127.0.0.1\nnotify=0\n')
    cini = os.path.join(TEST, 'rc-client.ini')
    if os.path.exists(cini):
        os.remove(cini)
    clog = os.path.join(TEST, 'rc-client.log')
    if os.path.exists(clog):
        os.remove(clog)
    subprocess.Popen([SEXE, '-ini', sini, '-testsrc', 'static'])
    time.sleep(1.5)
    p = subprocess.Popen([CLIENT, '127.0.0.1::5998', '-ini', cini, '-log'])
    ok = True
    try:
        h = None
        for _ in range(50):
            h = u.FindWindowW('iivnc.Client.View', None)
            if h:
                break
            time.sleep(0.1)
        time.sleep(1.5)
        # 縮めて描かせる(等倍だと GDI と GPU の絵が画素まで同じになり、どちらが描いたか見分けられない)
        u.SetWindowPos(h, None, 0, 0, 1300, 780, 0x16)  # SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE
        time.sleep(1.5)
        steps = [('既定', None), ('GPU へ', IDM_RENDER_GPU), ('GDI へ', IDM_RENDER_GDI), ('GPU へ', IDM_RENDER_GPU)]
        base = None
        for name, cmd in steps:
            if cmd:
                u.PostMessageW(h, 0x112, cmd, 0)         # WM_SYSCOMMAND
                time.sleep(2)
            img = grab(h)
            mean, colors = stats(img)
            if base is None:
                base = img
            d = diff(base, img)
            # GDI はいつも同じ絵(差 0)、GPU は縮め方が違うので少しだけ違う絵になるはず
            good = colors > 20 and (d < 0.5 if cmd != IDM_RENDER_GPU else 0.05 < d < 6)
            ok &= good
            print(f'  {"OK" if good else "NG"}  {name:6s} 窓 {img[0]}x{img[1]}  明るさ {mean:5.1f}  色数 {colors:4d}  '
                  f'最初の絵との差 {d:4.2f}  専用 {private_mb(p.pid):6.1f}MB')
            # マウスの行き先: GPU のときも子窓(キャンバス)ではなく、親の窓に届くこと。
            # カーソルは動かさず、マウスと同じ当たり判定の WindowFromPoint で見る(そのときだけ最前面にする)
            rc = wintypes.RECT()
            u.GetClientRect(h, ctypes.byref(rc))
            pt = wintypes.POINT(rc.right // 2, rc.bottom // 2)
            u.ClientToScreen(h, ctypes.byref(pt))
            u.SetWindowPos(h, wintypes.HWND(-1), 0, 0, 0, 0, 0x13)     # HWND_TOPMOST、NOMOVE|NOSIZE|NOACTIVATE
            hit = u.WindowFromPoint(pt)
            u.SetWindowPos(h, wintypes.HWND(-2), 0, 0, 0, 0, 0x13)     # HWND_NOTOPMOST
            child = u.FindWindowExW(h, None, 'iivnc.Client.Canvas', None)
            hok = hit == h
            ok &= hok
            print(f'  {"OK" if hok else "NG"}  {"":6s} 中心のマウスの行き先: {"親の窓" if hit == h else "子窓" if hit == child else hex(hit or 0)}'
                  f'(子窓 {"あり" if child else "なし"})')
        # GPU のまま全画面へ: 子窓が窓の中身いっぱいに付いてくること
        for name in ('全画面', '窓に戻す'):
            u.PostMessageW(h, 0x112, 0x100, 0)          # WM_SYSCOMMAND IDM_FULLSCREEN
            time.sleep(1.5)
            child = u.FindWindowExW(h, None, 'iivnc.Client.Canvas', None)
            cr, kr = wintypes.RECT(), wintypes.RECT()
            u.GetClientRect(h, ctypes.byref(cr))
            u.GetClientRect(child, ctypes.byref(kr))
            mean, colors = stats(grab(h))
            fok = bool(child) and (cr.right, cr.bottom) == (kr.right, kr.bottom) and colors > 20
            ok &= fok
            print(f'  {"OK" if fok else "NG"}  {name:6s} 窓 {cr.right}x{cr.bottom}  子窓 {kr.right}x{kr.bottom}  色数 {colors}')
        u.PostMessageW(h, 0x10, 0, 0)
        p.wait(10)
    finally:
        if p.poll() is None:
            p.kill()
        subprocess.run([SEXE, '-ini', sini, '-exit'])
    lines = [l.rstrip() for l in open(clog, encoding='utf-8') if 'D3D11' in l or '描画' in l]
    print('\n'.join('  ' + l for l in lines))
    saved = [l.strip() for l in open(cini, encoding='utf-8') if l.startswith('render=')]
    print(f'  ini に残った値: {saved}')
    ok &= saved == ['render=gpu']
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
