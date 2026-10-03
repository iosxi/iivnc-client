"""iivnc-client を iivnc-server(-testsrc)につなぎ、受け取った絵を比べる。

    python tools/clientcheck.py

サーバーは ../iivnc-server の exe を検証用の ini と -testsrc で動かす(画面は写さない・入力は再現しない)。
クライアントは -dump で受け取った絵を BMP に書いて終わる。窓は前面に出さない(SW_SHOWNOACTIVATE)。
正解は rfbcheck.py(独立に書いた受け手)が Raw で受けた絵。
"""
import os, subprocess, sys, time
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SERVER = os.path.join(os.path.dirname(ROOT), 'iivnc-server')
sys.path.insert(0, os.path.join(SERVER, 'tools'))
import rfbcheck  # noqa: E402
import test as srvtest  # noqa: E402

CLIENT = os.path.join(ROOT, 'iivnc-client.exe')
TEST = os.path.join(ROOT, 'build', 'test')
PORT = srvtest.PORT


def run_client(quality, extra=()):
    os.makedirs(TEST, exist_ok=True)
    out = os.path.join(TEST, f'dump-{quality}.bmp')
    if os.path.exists(out):
        os.remove(out)
    ini = os.path.join(TEST, 'client.ini')
    with open(ini, 'w', encoding='utf-8', newline='\n') as f:
        f.write('log=1\n')
    t0 = time.perf_counter()
    r = subprocess.run([CLIENT, f'127.0.0.1::{PORT}', '-ini', ini, '-quality', quality, '-dump', out,
                        '-idleexit', '1500', *extra], timeout=120)
    dt = time.perf_counter() - t0
    img = np.asarray(Image.open(out).convert('RGB')) if os.path.exists(out) else None
    return img, dt


def main():
    ok = True
    print('[止まった絵]')
    srvtest.start(['-testsrc', 'static'])
    try:
        ref = rfbcheck.reference(PORT, None)
        for q, need in (('lossless', 99), ('high', 40), ('normal', 28), ('low', 22)):
            img, dt = run_client(q)
            if img is None:
                print(f'  {q:9s} 絵が書かれていない NG'); ok = False; continue
            p = rfbcheck.psnr(ref, img)
            good = p >= need
            ok &= good
            print(f'  {q:9s} PSNR {p:5.1f}  {"OK" if good else "NG"}')
    finally:
        srvtest.stop()

    print('[動く絵 300 フレームを追い、止まったところで比べる]')
    for q, need in (('lossless', 99), ('high', 38)):
        srvtest.start(['-testsrc', '-testframes', '300'])
        try:
            img, dt = run_client(q)
            ref = rfbcheck.reference(PORT, None)
            p = rfbcheck.psnr(ref, img) if img is not None else 0
            good = p >= need
            ok &= good
            print(f'  {q:9s} PSNR {p:5.1f}  {"OK" if good else "NG"}')
        finally:
            srvtest.stop()
    print('[途中で画面の大きさが変わる(100 フレーム目で 1280x720 へ)]')
    srvtest.start(['-testsrc', '-testframes', '200', '-testresize', '100'])
    try:
        img, dt = run_client('lossless')
        ref = rfbcheck.reference(PORT, None)
        good = img is not None and img.shape == ref.shape and rfbcheck.psnr(ref, img) >= 99
        ok &= good
        print(f'  受け取った絵 {None if img is None else img.shape[1::-1]}、正解 {ref.shape[1::-1]}  {"OK" if good else "NG"}')
    finally:
        srvtest.stop()
    log = os.path.join(TEST, 'client.log')
    if os.path.exists(log):
        print('--- client.log(終わりの部分)')
        print(''.join(open(log, encoding='utf-8').readlines()[-8:]))
    print('ALL OK' if ok else 'SOME NG')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
