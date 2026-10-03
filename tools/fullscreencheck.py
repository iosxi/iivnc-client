"""全画面のとき(キーボードを横取りしている間)に、Ctrl+Alt+Enter で全画面を抜けられるか確かめる。

    python tools/fullscreencheck.py

iivnc-server を -testsrc static(入力はログに書くだけ)・ポート 5998 で動かし、クライアントを
-fullscreen -hooktest(注入したキーもフックで横取りする)で起動する。
**クライアントの窓が手前にあるときだけ** SendInput で Ctrl+Alt+Enter を注入する
(手前でなければ、利用者の作業中の窓へキーが行ってしまうので注入しない)。
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SERVER = os.path.join(os.path.dirname(ROOT), 'iivnc-server')
CLIENT = os.path.join(ROOT, 'iivnc-client.exe')
SEXE = os.path.join(SERVER, 'iivnc-server.exe')
TEST = os.path.join(ROOT, 'build', 'test')

u = ctypes.WinDLL('user32', use_last_error=True)
u.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
u.FindWindowW.restype = wintypes.HWND
u.GetForegroundWindow.restype = wintypes.HWND


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [('wVk', wintypes.WORD), ('wScan', wintypes.WORD), ('dwFlags', wintypes.DWORD),
                ('time', wintypes.DWORD), ('dwExtraInfo', ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [('ki', KEYBDINPUT), ('pad', ctypes.c_byte * 32)]
    _anonymous_ = ('u',)
    _fields_ = [('type', wintypes.DWORD), ('u', _U)]


def send(keys):
    arr = (INPUT * len(keys))()
    for i, (vk, scan, up, ext) in enumerate(keys):
        arr[i].type = 1
        arr[i].ki.wVk = vk
        arr[i].ki.wScan = scan
        arr[i].ki.dwFlags = (2 if up else 0) | (1 if ext else 0)
    u.SendInput(len(keys), arr, ctypes.sizeof(INPUT))


def style(h):
    return u.GetWindowLongW(h, -16) & 0xFFFFFFFF


def main():
    os.makedirs(TEST, exist_ok=True)
    sini = os.path.join(TEST, 'fs-server.ini')
    open(sini, 'w', encoding='utf-8', newline='\n').write('port=5998\nlisten=127.0.0.1\nnotify=0\nlog=1\n')
    cini = os.path.join(TEST, 'fs-client.ini')
    open(cini, 'w', encoding='utf-8', newline='\n').write('log=1\n')
    busy = '--busy' in sys.argv                     # 全面が毎フレーム変わる絵を上限なしで(画面のスレッドが忙しい)
    subprocess.Popen([SEXE, '-ini', sini, '-testsrc'] + (['video', '-testfps', '0'] if busy else ['static']))
    time.sleep(1.5)
    via_menu = '--menu' in sys.argv
    # --injected: input-mouser などが注入したキー(フックは横取りせず、窓へ素通りさせる経路)を試す
    hk = [] if '--injected' in sys.argv else ['-hooktest']
    p = subprocess.Popen([CLIENT, '127.0.0.1::5998', '-ini', cini] + hk + ([] if via_menu else ['-fullscreen']))
    ok = False
    try:
        h = None
        for _ in range(50):
            h = u.FindWindowW('iivnc.Client.View', None)
            if h:
                break
            time.sleep(0.1)
        time.sleep(2.5)
        if via_menu:                                # 窓のシステム メニューの「全画面」と同じ
            u.PostMessageW(h, 0x112, 0x100, 0)      # WM_SYSCOMMAND IDM_FULLSCREEN
            time.sleep(1.0)
        full = not (style(h) & 0x00C00000)          # WS_CAPTION が無い = 全画面
        fg = u.GetForegroundWindow() == h
        print(f'全画面になった: {full}、手前にある: {fg}')
        if not full or not fg:
            print('手前に無いので注入しない(利用者の窓へキーが行くため)')
        else:
            # 左 Ctrl、左 Alt、Enter を押して離す
            # 人の押し方に近く: 間を空け、押している間は自動の繰り返しも送る
            for ev in [(0xA2, 0x1D, False, False), (0xA2, 0x1D, False, False), (0xA4, 0x38, False, False),
                       (0xA4, 0x38, False, False), (0x0D, 0x1C, False, False), (0x0D, 0x1C, True, False),
                       (0xA4, 0x38, True, False), (0xA2, 0x1D, True, False)]:
                send([ev])
                time.sleep(0.12)
            time.sleep(1.0)
            back = bool(style(h) & 0x00C00000)
            print(f'Ctrl+Alt+Enter の後、窓に戻った: {back}')
            ok = back
        u.PostMessageW(h, 0x10, 0, 0)
        p.wait(10)
    finally:
        if p.poll() is None:
            p.kill()
        subprocess.run([SEXE, '-ini', sini, '-exit'])
    log = os.path.join(TEST, 'fs-client.log')
    print(''.join(l for l in open(log, encoding='utf-8').readlines()[-12:] if '[hook]' in l or '全画面' in l))
    print('OK' if ok else 'NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
