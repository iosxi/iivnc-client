"""ほかの VNC サーバー(既定は WSL の TigerVNC Xvnc、ポート 5911)に iivnc-client をつなぎ、
エンコーディングごとに受け取った絵を、rfbcheck(独立に書いた受け手)が Raw で受けた絵と比べる。

    python tools/interop.py [--port 5911]
"""
import argparse, os, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(os.path.dirname(ROOT), 'iivnc-server', 'tools'))
import rfbcheck  # noqa: E402

CLIENT = os.path.join(ROOT, 'iivnc-client.exe')
TEST = os.path.join(ROOT, 'build', 'test')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=5911)
    a = ap.parse_args()
    os.makedirs(TEST, exist_ok=True)
    ini = os.path.join(TEST, 'interop.ini')
    open(ini, 'w', encoding='utf-8').write('log=1\n')
    # カーソルを別に受け取る(サーバーが絵に描き込まない)ようにして正解を取る
    ref = rfbcheck.reference(a.port, None, (0, -239))
    ok = True
    for enc, q, need in (('raw', 'lossless', 99), ('rre', 'lossless', 99), ('hextile', 'lossless', 99),
                         ('zrle', 'lossless', 99), ('tight', 'lossless', 99), ('tight', 'high', 35), ('tight', 'low', 22)):
        out = os.path.join(TEST, f'interop-{enc}-{q}.bmp')
        if os.path.exists(out):
            os.remove(out)
        subprocess.run([CLIENT, f'127.0.0.1::{a.port}', '-ini', ini, '-encoding', enc, '-quality', q,
                        '-dump', out, '-idleexit', '1500'], timeout=60)
        img = np.asarray(Image.open(out).convert('RGB')) if os.path.exists(out) else None
        p = rfbcheck.psnr(ref, img) if img is not None and img.shape == ref.shape else 0
        good = p >= need
        ok &= good
        print(f'  {enc:8s} {q:9s} PSNR {p:5.1f}  {"OK" if good else "NG"}')
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
