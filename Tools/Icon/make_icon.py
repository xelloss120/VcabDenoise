# アプリのアイコン（Resources/Icon.png と Resources/IconSmall.png）を描く。
# 図柄: VoiChanAlgoBox のアイコン（紺の角丸の土台に、枠付きの箱。箱の中に、元の声色の山〈青〉と、右へ動いた声色の山〈黄〉）
# そのままに、箱の上に緑の帯を重ね、白で「DENOISE」と書く。帯は箱の枠の外側（上と左右）にそろえる。
# 緑は、画面のモニターのノイズ推定の線と同じ色。色はどれもアプリの画面の色。
# 文字は Noto Sans Bold（SIL Open Font License。Windows に入っている C:\Windows\Fonts\NotoSans-Bold.ttf）。
# 必要なもの: Python 3、numpy、matplotlib、Pillow。使い方: python make_icon.py [Noto Sans Bold のファイル]
import os
import sys

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.font_manager import FontProperties
from matplotlib.patches import FancyBboxPatch
from PIL import Image

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(REPO, 'Resources')
FONT = sys.argv[1] if len(sys.argv) > 1 else r'C:\Windows\Fonts\NotoSans-Bold.ttf'

BG = '#17202e'       # ウィンドウの背景
PANEL = '#273550'    # 枠の中
BEFORE = '#60c0ff'   # 元の声色（青）
AFTER = '#ffd060'    # 動かした声色（黄）
EDGE = '#4a90e0'     # 箱の枠
GREEN = '#60d080'    # モニターのノイズ推定の線（帯）


def envelope(x, shift):
    return (0.30 + 0.30 * np.exp(-((x - 0.36 * shift) / 0.06) ** 2) + 0.17 * np.exp(-((x - 0.55 * shift) / 0.055) ** 2)
            + 0.08 * np.exp(-((x - 0.72 * shift) / 0.045) ** 2))


def draw(path, small):
    """small: 16〜48px 用（背景の横線を省き、線と文字を太く大きくする）"""
    fig = plt.figure(figsize=(10.24, 10.24), dpi=100)
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.axis('off')
    fig.patch.set_alpha(0)
    ax.add_patch(FancyBboxPatch((0.04, 0.04), 0.92, 0.92, boxstyle='round,pad=0,rounding_size=0.2', fc=BG, ec=BG))
    inset = 0.14 if small else 0.18
    lw = 30 if small else 14
    ax.add_patch(FancyBboxPatch((inset, inset), 1 - 2 * inset, 1 - 2 * inset, boxstyle='round,pad=0,rounding_size=0.08',
                                fc=PANEL, ec=EDGE, lw=lw))
    if not small:
        for k in range(4):
            ax.plot([0.26, 0.48], [0.30 + k * 0.08] * 2, color=BEFORE, lw=26, solid_capstyle='round', alpha=0.45 * (1 - 0.15 * k))
            ax.plot([0.54, 0.74], [0.36 + k * 0.13] * 2, color=AFTER, lw=26, solid_capstyle='round', alpha=0.45 * (1 - 0.15 * k))
    x = np.linspace(0.22, 0.78, 500)
    ax.plot(x, envelope(x, 1.0), color=BEFORE, lw=34 if small else 18, solid_capstyle='round')
    ax.plot(x, envelope(x, 1.14), color=AFTER, lw=44 if small else 24, solid_capstyle='round')

    # 帯: 箱の枠の線の外側（上と左右）にそろえる
    half = lw / 72 * 100 / 1024 / 2  # 枠の線の太さの半分（図の座標）
    height = 0.20 if small else 0.16
    top = 1 - inset + half
    ax.add_patch(FancyBboxPatch((inset - half, top - height), 1 - 2 * inset + 2 * half, height,
                                boxstyle='round,pad=0,rounding_size=' + ('0.06' if small else '0.08'), fc=GREEN, ec='none',
                                zorder=3))  # 箱の中の線より手前
    ax.text(0.5, top - height / 2 + 0.003, 'DENOISE', fontproperties=FontProperties(fname=FONT), fontsize=86 if small else 78,
            color='white', ha='center', va='center', zorder=4)
    fig.savefig(path, dpi=100, transparent=True)
    plt.close(fig)
    print(path)


def main():
    os.makedirs(OUT, exist_ok=True)
    draw(os.path.join(OUT, 'Icon.png'), small=False)
    small = os.path.join(OUT, 'IconSmall.png')
    draw(small, small=True)
    Image.open(small).resize((256, 256), Image.LANCZOS).save(small, optimize=True)


if __name__ == '__main__':
    main()
