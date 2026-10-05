"""窓を最大化・元に戻したとき、絵が窓に合わせて描き直されるか確かめる。

    python tools/maxcheck.py

iivnc-server を -testsrc static・ポート 5998 で動かし、fit=1(窓に合わせる)でつなぐ。GDI と GPU の
それぞれで、WM_SYSCOMMAND の SC_MAXIMIZE / SC_RESTORE を送り、窓の中身を PrintWindow で撮って、
黒でない部分(絵)の外枠が、窓に合わせたときの大きさ(16:9 で窓いっぱい)になっているかを見る。
最大化した窓が一瞬画面に出る。キー・マウスは送らない。
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rendercheck as rc  # noqa: E402

u = rc.u
TEST = rc.TEST
EXTRA = ''.join(a + '\n' for a in sys.argv[1:])     # ini に足す行。例: quality=low viewonly=1


def content_box(img):
    """黒でない画素の外枠(間引いて探す)"""
    w, h, b = img
    xs, ys = [], []
    for y in range(0, h, 4):
        row = b[y * w * 4:(y + 1) * w * 4]
        for x in range(0, w, 4):
            i = x * 4
            if row[i] > 8 or row[i + 1] > 8 or row[i + 2] > 8:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


def expected(cw, ch):
    s = min(cw / 1920, ch / 1080)
    dw, dh = round(1920 * s), round(1080 * s)
    x, y = (cw - dw) // 2, (ch - dh) // 2
    return x, y, x + dw - 1, y + dh - 1


def check(render):
    cini = os.path.join(TEST, f'max-{render}.ini')
    open(cini, 'w', encoding='utf-8', newline='\n').write(f'log=1\nfit=1\nrender={render}\n' + EXTRA)
    p = subprocess.Popen([rc.CLIENT, '127.0.0.1::5998', '-ini', cini, '-idleexit', '600000'])
    ok = True
    try:
        h = None
        for _ in range(50):
            h = u.FindWindowW('iivnc.Client.View', None)
            if h:
                break
            time.sleep(0.1)
        time.sleep(2)
        for name, cmd in (('最初', None), ('最大化', 0xF030), ('元に戻す', 0xF120), ('もう一度最大化', 0xF030)):
            if cmd:
                u.PostMessageW(h, 0x112, cmd, 0)          # WM_SYSCOMMAND
                time.sleep(1.5)
            img = rc.grab(h)
            box = content_box(img)
            want = expected(img[0], img[1])
            good = box is not None and all(abs(a - b) <= 6 for a, b in zip(box, want))
            ok &= good
            print(f'  {"OK" if good else "NG"}  {render} {name:7s} 窓 {img[0]}x{img[1]}  絵の外枠 {box}  期待 {want}')
        u.PostMessageW(h, 0x10, 0, 0)
        p.wait(10)
    finally:
        if p.poll() is None:
            p.kill()
    return ok


def main():
    sini = os.path.join(TEST, 'rc-server.ini')
    open(sini, 'w', encoding='utf-8', newline='\n').write('port=5998\nlisten=127.0.0.1\nnotify=0\n')
    subprocess.Popen([rc.SEXE, '-ini', sini, '-testsrc', 'static'])
    time.sleep(1.5)
    try:
        ok = check('gdi') & check('gpu')
    finally:
        subprocess.run([rc.SEXE, '-ini', sini, '-exit'])
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
