# 質感の評価: 粒（ミュージカルノイズ）と上下（パキパキ）を測る（仕様書 5.5 の測り方を作り直したもの）
#   python texture_eval.py strength=3 ...   （設定を並べる。なければ初期値）
#   --kind pink|white|fan でノイズの種類、--level <dB> でノイズの大きさ（初期値 -45）
#
# 粒: ノイズだけを通し、残ったノイズの区画ごとの大きさの尖度を、入力のノイズの尖度で割った値（log10）。
#     区画ごとに時間の平均で割ってから全体の尖度を求める（ノイズの周波数の形に左右されないように）。0 なら粒が増えていない。
# 上下: 声＋ノイズを通し、区画ごとの「出力 ÷ 入力」（dB）の、5.3ms ごとの変化の絶対値の平均。
import sys

import numpy as np
from scipy.signal import stft

import signals as sg
from denoise_eval import run, db


def spec(x, n, hop):
    return np.abs(stft(x, sg.SR, nperseg=n, noverlap=n - hop, boundary=None, padded=False)[2]) ** 2


def kurtosis(p):
    p = p / (np.mean(p, axis=1, keepdims=True) + 1e-30)
    v = p.ravel()
    m = v.mean()
    return np.mean((v - m) ** 4) / np.mean((v - m) ** 2) ** 2


def band(n, lo, hi):
    f = np.arange(n // 2 + 1) * sg.SR / n
    return (f >= lo) & (f < hi)


def evaluate(sets, label='', kind='pink', noise_db=-45):
    rng = np.random.default_rng(1)
    clean, _ = sg.tts_speech()
    nz = sg.noise(rng, kind, len(clean), noise_db)
    y = run(clean + nz, sets, 'tex_mix')
    yn = run(nz, sets, 'tex_noise')

    # 粒（4 秒より後。100Hz〜10kHz、500Hz より上だけも出す）
    s = int(4 * sg.SR)
    N, Yn = spec(nz[s:], 1024, 256), spec(yn[s:], 1024, 256)
    grain = {}
    for name, (lo, hi) in {'全体': (100, 10000), '500Hz〜': (500, 10000)}.items():
        b = band(1024, lo, hi)
        grain[name] = np.log10(kurtosis(Yn[b]) / kurtosis(N[b]))

    # 上下（3 秒より後。入力が十分に大きい区画だけ）
    s = int(3 * sg.SR)
    X, Y = spec((clean + nz)[s:], 256, 256), spec(y[s:], 256, 256)
    b = band(256, 300, 10000)
    r = db(Y[b]) - db(X[b])
    ok = (db(X[b]) > db(np.median(X[b])) - 20)
    d = np.abs(np.diff(r, axis=1))[ok[:, 1:] & ok[:, :-1]]
    print(f'{label:24s} 粒 {grain["全体"]:6.3f}（500Hz〜 {grain["500Hz〜"]:6.3f}）  上下 {np.mean(d):5.2f}')


if __name__ == '__main__':
    args = sys.argv[1:]
    kind, level = 'pink', -45.0
    if '--kind' in args:
        kind = args[args.index('--kind') + 1]
        del args[args.index('--kind'):args.index('--kind') + 2]
    if '--level' in args:
        level = float(args[args.index('--level') + 1])
        del args[args.index('--level'):args.index('--level') + 2]
    sets = [a for a in args if '=' in a]
    evaluate(sets, ' '.join(sets) or '初期値', kind=kind, noise_db=level)
