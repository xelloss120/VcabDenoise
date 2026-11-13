# 狭帯域抑制の評価（2026-11-13）: 声 6 種 × ノイズ 3 種（飛ぶ持続音 / ハム / 持続音なし）。1 つの設定で約 15 秒
#   python tonal_eval.py name:key=val,key=val name2:...     （名前:設定。初期値の行 "default" は自動で入る）
#   例: python tonal_eval.py deep:tonal_max_depth=30 slow:tonal_activate=2
# ノイズ: ファン（声より 20dB 小さい）に、whine は 55.7Hz と、1.6k → 1.95k → 1.55k → 1.8kHz と 4 秒ごとに飛ぶ音、
#   hum は 50Hz の 8 倍まで、none は何も足さない。
# 先に make_voices.ps1 で声を作っておく。入力は build/regression/tonal/ に置き、処理した WAV は測ったらすぐ消す。
# 指標:
#   残り      持続音の位置の、ノイズの分の下がり（dB。大きいほど消えている）。飛んだ直後 1.5 秒は除く
#   追従      飛んだ後、新しい周波数で 10dB 下がるまでの秒（3 回の平均。2 秒で打ち切り）
#   声削れ    狭帯域抑制オフとの比で、声の分の 60〜4000Hz の区画ごとの削れ。平均と、上位 1% の深さ（dB）
#   誤谷      --notches の一覧で、持続音から ±4% 以内にない谷の深さの合計（dB・秒、声 6 種の平均）
import itertools
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from scipy.io import wavfile
from scipy.signal import lfilter

import signals as sg
import wide_eval as we

SR = 48000
W = os.path.join(sg.WORK, 'tonal')
EXE = sg.EXE
JUMPS = [6.0, 10.0, 14.0]
STEPS = [1600.0, 1950.0, 1550.0, 1800.0]


def whine_freq(t):
    return np.select([t < JUMPS[0], t < JUMPS[1], t < JUMPS[2]], STEPS[:3], STEPS[3])


def tones_at(kind, t):
    """その時刻の持続音の周波数の一覧"""
    if kind == 'whine':
        return [55.7, float(whine_freq(np.array(t)))]
    if kind == 'hum':
        return [50.0 * k for k in range(1, 9)]
    return []


def make(kind, n, fan_rms, seed):
    rng = np.random.default_rng(seed)
    w = lfilter([1], [1, -0.97], rng.standard_normal(n))
    nz = w * fan_rms / np.sqrt(np.mean(w ** 2))
    t = np.arange(n) / SR
    a = fan_rms * np.sqrt(2)
    if kind == 'whine':
        nz += a * 10 ** (5 / 20) * np.sin(2 * np.pi * 55.7 * t)
        nz += a * 10 ** (-5 / 20) * np.sin(2 * np.pi * np.cumsum(whine_freq(t)) / SR)
    elif kind == 'hum':
        for k in range(1, 9):
            nz += a * 10 ** ((3 - (0 if k % 2 else 8) - 2 * k) / 20) * np.sin(2 * np.pi * 50 * k * t + k)
    return nz


def spec(x):
    S = we.spec(x, 4096, 512)
    return np.fft.rfftfreq(4096, 1 / SR), (np.arange(S.shape[1]) * 512 + 2048) / SR, S


def render(fx, fc, fn, tag, sets):
    o = [os.path.join(W, f'{tag}_{k}.wav') for k in 'ycn']
    args = [EXE, '--in', fx, '--out', o[0], '--shadow', fc, o[1], '--shadow', fn, o[2], '--notches']
    for s in sets:
        args += ['--set', s]
    r = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace', check=True)
    rd = lambda p: wavfile.read(p)[1].astype(float)
    out = [rd(p) for p in o]
    for p in o:
        os.remove(p)
    notches = []
    for line in r.stdout.splitlines():
        m = re.match(r't=(\d+)s .*狭帯域ノイズ:(.*)', line)
        if m:
            sec = int(m.group(1))
            for hz, d in re.findall(r'(\d+)Hz/([\d.]+)dB', m.group(2)):
                notches.append((sec, float(hz), float(d)))
    return out, notches


def measure(kind, c, mask, nz, out, ref, notches):
    f, tt, N0 = spec(nz)
    _, _, N1 = spec(out[2])
    m = {}
    if kind != 'none':
        # 残り
        red = []
        for i, t in enumerate(tt):
            if t < 2.5 or any(0 <= t - j < 1.5 for j in JUMPS):
                continue
            for hz in tones_at(kind, t):
                k = int(round(hz / (SR / 4096)))
                red.append(10 * np.log10(N0[k - 1:k + 2, i].sum() / max(N1[k - 1:k + 2, i].sum(), 1e-30)))
        m['残り'] = np.mean(red)
    if kind == 'whine':
        lat = []
        for j, hz in zip(JUMPS, STEPS[1:]):
            k = int(round(hz / (SR / 4096)))
            got = 2.0
            for i, t in enumerate(tt):
                if j <= t < j + 2.0:
                    r = 10 * np.log10(N0[k - 1:k + 2, i].sum() / max(N1[k - 1:k + 2, i].sum(), 1e-30))
                    if r >= 10:
                        got = t - j
                        break
            lat.append(got)
        m['追従'] = np.mean(lat)
    # 声削れ（オフとの比）
    _, _, C1 = spec(out[1])
    _, _, C0 = spec(ref)
    _, _, Cc = spec(c)
    band = (f >= 60) & (f <= 4000)
    fr = we.frames(mask, 4096, 512, C1.shape[1], lambda v: v > 0.9)
    sel = Cc[band][:, fr]
    strong = sel > sel.max(0, keepdims=True) * 10 ** (-3.0)
    loss = 10 * np.log10(np.maximum(C0[band][:, fr], 1e-30) / np.maximum(C1[band][:, fr], 1e-30))[strong]
    m['声削れ'] = np.mean(loss)
    m['声削れ1%'] = np.percentile(loss, 99)
    # 誤谷
    bad = 0.0
    for sec, hz, d in notches:
        if not any(abs(hz - th) <= 0.04 * th for th in tones_at(kind, sec - 0.5) + tones_at(kind, sec)):
            bad += d
    m['誤谷'] = bad
    return m


def main():
    variants = {'default': []}
    for a in sys.argv[1:]:
        name, kv = a.split(':', 1)
        variants[name] = [x for x in kv.split(',') if x]
    os.makedirs(W, exist_ok=True)
    voices = we.load_voices(False)
    kinds = ['whine', 'hum', 'none']
    conds = []
    for (vn, (c, mask)), kind in itertools.product(voices.items(), kinds):
        fan = np.sqrt(np.mean(c[mask] ** 2)) * 10 ** (-20 / 20)
        nz = make(kind, len(c), fan, 7)
        tag = f'{vn}_{kind}'
        fx, fc, fn = (os.path.join(W, f'in_{tag}_{k}.wav') for k in 'xcn')
        for p, x in ((fx, c + nz), (fc, c), (fn, nz)):
            if not os.path.exists(p):
                wavfile.write(p, SR, x.astype(np.float32))
        conds.append((vn, kind, c, mask, nz, fx, fc, fn))

    def job(args):
        (vn, kind, c, mask, nz, fx, fc, fn), (name, sets) = args
        ref, _ = render(fx, fc, fn, f'{name}_{vn}_{kind}_ref', sets + ['tonal_on=0'])
        out, notches = render(fx, fc, fn, f'{name}_{vn}_{kind}', sets)
        return (name, kind, vn), measure(kind, c, mask, nz, out, ref[1], notches)
    with ThreadPoolExecutor(8) as ex:
        R = dict(ex.map(job, itertools.product(conds, variants.items())))
    cols = [('whine', '残り'), ('whine', '追従'), ('whine', '声削れ'), ('whine', '声削れ1%'), ('whine', '誤谷'),
            ('hum', '残り'), ('hum', '声削れ1%'), ('hum', '誤谷'), ('none', '声削れ'), ('none', '声削れ1%'), ('none', '誤谷')]
    print(f'{"":24s}' + ''.join(f'{k[:2]}{m:>7s}' for k, m in cols))
    for name in variants:
        row = []
        for k, m in cols:
            vals = [R[(name, k, vn)][m] for vn in voices]
            row.append(np.mean(vals))
        print(f'{name:24s}' + ''.join(f'{v:9.2f}' for v in row))


if __name__ == '__main__':
    main()
