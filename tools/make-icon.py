"""iivnc-client のアイコンを作る。

    python tools/make-icon.py

src/iivnc-client.ico   画面と、こちらへ来る矢印
src/iivnc-client.png   README 用
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def draw(size):
    s = 1024
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([60, 140, 964, 760], radius=90, fill=(40, 44, 52, 255))
    d.rounded_rectangle([120, 200, 904, 700], radius=40, fill=(13, 148, 136, 255))
    d.rectangle([452, 760, 572, 860], fill=(40, 44, 52, 255))
    d.rounded_rectangle([300, 840, 724, 920], radius=40, fill=(40, 44, 52, 255))
    # こちらへ来る矢印(左下へ)
    d.polygon([(464, 600), (224, 600), (224, 360), (304, 440), (504, 240), (584, 320), (384, 520)], fill=(255, 255, 255, 255))
    return im.resize((size, size), Image.LANCZOS)


imgs = [draw(n) for n in SIZES]
imgs[-1].save(os.path.join(ROOT, 'src', 'iivnc-client.ico'), format='ICO', sizes=[(n, n) for n in SIZES], append_images=imgs[:-1])
imgs[-1].resize((128, 128), Image.LANCZOS).save(os.path.join(ROOT, 'src', 'iivnc-client.png'))
print('ok')
