"""窓のシステム メニュー(タイトル バーの右クリック)のチェックが、今の状態に合っているか確かめる。

    python tools/menucheck.py

iivnc-server を -testsrc static・ポート 5998 で動かしてつなぎ、WM_SYSCOMMAND で画質・速さを表示・描画を
変えてから、タイトル バーへ WM_CONTEXTMENU を送ってシステム メニューを開き、開いたメニューの窓から
MN_GETHMENU でハンドルを得てチェックを読む(WM_CANCELMODE で閉じる)。メニューは試験用の窓の上に
一瞬だけ出る。キーもマウスも送らない。サーバーのログで、画質の変更がその場で届いたかも見る。
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rendercheck as rc
u = rc.u
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))); TEST = os.path.join(ROOT, 'build', 'test'); SEXE = rc.SEXE
SLOG = os.path.join(os.path.dirname(SEXE), 'build', 'test', 'menu-server.log')
u.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.SendMessageW.restype = ctypes.c_size_t
u.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
u.GetMenuState.argtypes = [wintypes.HMENU, wintypes.UINT, wintypes.UINT]
u.GetSubMenu.restype = wintypes.HMENU
u.GetSubMenu.argtypes = [wintypes.HMENU, ctypes.c_int]
u.GetMenuItemCount.argtypes = [wintypes.HMENU]
IDS = {'高画質': 0x103, '劣化なし': 0x104, '標準': 0x105, '細い回線': 0x106, '速さ': 0x10B, 'GDI': 0x10E, 'GPU': 0x10F}

PID = [0]
u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]


def menu_states(h):
    r = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(r))
    x, y = r.left + 200, r.top + 10
    u.PostMessageW(h, 0x7B, h, (y << 16) | (x & 0xFFFF))      # WM_CONTEXTMENU(タイトル バー)
    m = None
    for _ in range(30):
        time.sleep(0.1)
        pw = None                                            # クライアントのメニューだけ(ほかのアプリのは拾わない)
        while True:
            pw = u.FindWindowExW(None, pw, '#32768', None)
            if not pw:
                break
            pid = wintypes.DWORD()
            u.GetWindowThreadProcessId(pw, ctypes.byref(pid))
            if pid.value == PID[0] and u.IsWindowVisible(pw):
                break
        if pw:
            m = u.SendMessageW(pw, 0x1E1, 0, 0)               # MN_GETHMENU
            if m: break
    out = '(メニューが開かなかった)'
    if m:
        out = ' '.join(f'{k}{"✓" if u.GetMenuState(m, v, 0) & 8 else "・"}' for k, v in IDS.items())
    u.PostMessageW(h, 0x1F, 0, 0)                             # WM_CANCELMODE
    time.sleep(0.5)
    return out

sini = os.path.join(os.path.dirname(SLOG), 'menu-server.ini')
open(sini, 'w', encoding='utf-8', newline='\n').write('port=5998\nlisten=127.0.0.1\nnotify=0\nlog=1\n')
cini = os.path.join(TEST, 'menu-client.ini')
for f in (cini, SLOG):
    if os.path.exists(f): os.remove(f)
subprocess.Popen([SEXE, '-ini', sini, '-testsrc', 'static']); time.sleep(1.5)
p = subprocess.Popen([os.path.join(ROOT, 'iivnc-client.exe'), '127.0.0.1::5998', '-ini', cini, '-log'])
try:
    PID[0] = p.pid
    h = None
    for _ in range(50):
        h = u.FindWindowW('iivnc.Client.View', None)
        if h: break
        time.sleep(0.1)
    time.sleep(2)
    res = []
    res.append(menu_states(h)); print('  最初           ', res[-1])
    for cmd in (0x106, 0x10B, 0x10F):
        u.PostMessageW(h, 0x112, cmd, 0); time.sleep(1.0)
    res.append(menu_states(h)); print('  3 つ変えたあと ', res[-1])
    for cmd in (0x104, 0x10B, 0x10E):
        u.PostMessageW(h, 0x112, cmd, 0); time.sleep(1.0)
    res.append(menu_states(h)); print('  戻したあと     ', res[-1])
    u.PostMessageW(h, 0x10, 0, 0); p.wait(10)
finally:
    if p.poll() is None: p.kill(); print('  閉じなかったので kill')
    subprocess.run([SEXE, '-ini', sini, '-exit'])
print('  サーバーの記録:')
print(''.join('    ' + l for l in open(SLOG, encoding='utf-8') if 'エンコーディング' in l))
want = ['高画質✓ 劣化なし・ 標準・ 細い回線・ 速さ・ GDI✓ GPU・',
        '高画質・ 劣化なし・ 標準・ 細い回線✓ 速さ✓ GDI・ GPU✓',
        '高画質・ 劣化なし✓ 標準・ 細い回線・ 速さ・ GDI✓ GPU・']
q = [l for l in open(SLOG, encoding='utf-8') if 'エンコーディング' in l]
ok = res == want and len(q) == 3 and 'JPEG 画質 40' in q[1] and 'JPEG 画質 -1' in q[2]
print('ALL OK' if ok else 'SOME NG')
sys.exit(0 if ok else 1)
