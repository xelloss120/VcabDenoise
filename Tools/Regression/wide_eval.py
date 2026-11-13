# 2 つの設定（A と B）を、声 6 種 × ノイズ 5 種 × SNR 3 段 × 乱数 2 通り（180 条件）で比べる（2026-11-10）
#   python wide_eval.py --a max_reduction=20 --b max_reduction=24      （設定はいくつでも並べられる。キー=値）
#   python wide_eval.py --a max_reduction=20 --b max_reduction=24 --with strength=3   （両方に足す設定）
#   --quick で 18 条件（声 3 × ノイズ 3 × SNR 2 × 乱数 1）、--no-quality で PESQ・STOI・DNSMOS を省く
#
# vcabdn_render の --shadow で、出力を「声の分」と「ノイズの分」に正確に分けて測る（FIR は線形なので）。
# 先に make_tts.ps1 と make_voices.ps1 で声を作っておく。処理した WAV は build/regression/wide/ に置き、同じ設定なら作り直さない。
#
# 測るもの（声の区間・合間などは、元の声の大きさから決める）:
#   声の分     削れ（dB）。抑制量（出力÷入力、dB）の揺れを、速さで 3 つ（速い: 約 11ms より細かい、中: 11〜85ms、遅い: 85〜340ms）と、
#              周波数のギザギザ（隣の区画との 2 階差）に分ける。500Hz〜10kHz、声の区間
#   ノイズの分 合間と声の間の下がり（dB）。1/3 オクターブの帯域での揺れ（同じく 3 つ）。そのうち声に隠されない所
#              （残ったノイズが、同じ帯域の声の分から 12dB 以内）だけの揺れ
#   粒         ノイズの分の区画ごとの大きさを、近くの平均（周波数 ±3 区画、時間 ±16 フレーム）で割り、入力のノイズと比べた
#              尖度（log10 の比）と、6dB・9dB 以上飛び出た点の数の増え方（‰）。声に合わせたゆっくりした変化は消え、粒だけが残る
#              （声の倍音の位置を通り抜けるノイズも粒に似た形をしているので、絶対量ではなく、A と B の差を見る）
#   境目       語頭 20ms と語尾 50ms の声の削れ、話し終わりの 0〜100ms・100〜300ms のノイズの下がり
#   品質       PESQ（広帯域）、STOI、DNSMOS（P.835）。pesq・pystoi・onnxruntime と、DNSMOS のモデル
#              （build/regression/dnsmos/sig_bak_ovr.onnx。README を参照）があるときだけ
import hashlib
import itertools
import os
import sys
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from scipy.io import wavfile
from scipy.linalg import solve_toeplitz
from scipy.ndimage import uniform_filter, uniform_filter1d
from scipy.signal import lfilter, resample_poly, stft

import signals as sg

SR = sg.SR
VDIR = os.path.join(sg.WORK, 'voices')
WORK = os.path.join(sg.WORK, 'wide')
DNSMOS = os.path.join(sg.WORK, 'dnsmos', 'sig_bak_ovr.onnx')


# ---------------- 引数 ----------------
def parse(args):
    opt = {'a': [], 'b': [], 'with': [], 'quick': False, 'quality': True}
    cur = None
    for a in args:
        if a in ('--a', '--b', '--with'):
            cur = a[2:]
        elif a == '--quick':
            opt['quick'] = True
        elif a == '--no-quality':
            opt['quality'] = False
        elif '=' in a and cur:
            opt[cur].append(a)
        else:
            sys.exit(f'知らない引数: {a}')
    return opt


# ---------------- 声 ----------------
def whisper(x):
    """LPC で響き（声道）を残し、声帯の振動を雑音に置き換える（ささやき声）"""
    n, hop, order = 960, 480, 48
    w = np.hanning(n)
    rng = np.random.default_rng(5)
    y = np.zeros(len(x) + n)
    for s in range(0, len(x) - n, hop):
        f = x[s:s + n] * w
        r = np.correlate(f, f, 'full')[n - 1:n + order]
        if r[0] < 1e-9:
            continue
        r[0] *= 1.0001
        a = solve_toeplitz(r[:order], -r[1:order + 1])
        err = max(r[0] + np.dot(a, r[1:order + 1]), 0)
        e = rng.standard_normal(n) * np.sqrt(err / np.sum(w ** 2))
        y[s:s + n] += lfilter([1], np.concatenate([[1], a]), e) * w
    return y[:len(x)]


def load_voices(quick):
    def rd16(name):
        p = os.path.join(VDIR, name + '.wav')
        if not os.path.exists(p):
            sys.exit(f'{p} がありません。make_voices.ps1 で作ってください')
        return wavfile.read(p)[1].astype(float) / 32768.0
    h, z = rd16('haruka'), rd16('zira')
    v = {
        'haruka': h,                            # 日本語の女声（約 230Hz）
        'haruka_low': resample_poly(h, 4, 3),   # 遅く再生して 0.75 倍の高さに（約 170Hz）
        'haruka_vlow': resample_poly(h, 5, 3),  # 0.6 倍（約 140Hz）
        'zira': z,                              # 英語の女声（約 180Hz）
        'zira_low': resample_poly(z, 3, 2),     # 0.67 倍（約 120Hz）
        'whisper': whisper(h),                  # ささやき声
    }
    if quick:
        v = {k: v[k] for k in ('haruka', 'haruka_vlow', 'zira_low')}
    out = {}
    for k, x in v.items():
        x = np.concatenate([np.zeros(2 * SR), x, np.zeros(SR // 2)])
        x *= 0.3 / np.max(np.abs(x))
        e = np.convolve(x ** 2, np.ones(480) / 480, 'same')
        out[k] = (x, e > np.max(e) * 10 ** (-45 / 10))
    return out


# ---------------- ノイズ ----------------
def make_noise(kind, n, rms, seed):
    """白色・ピンク・ファン、ファン＋狭帯域の音（tones）、ゆっくり揺れるファン（fan_mod）。RMS を rms に合わせる"""
    rng = np.random.default_rng(1000 + seed)
    if kind == 'fan_mod':
        w = sg.noise(rng, 'fan', n, 0)
        t = np.arange(n) / SR
        slow = uniform_filter1d(rng.standard_normal(n // 480 + 1), 50)
        slow = np.repeat(slow / (np.std(slow) + 1e-12), 480)[:n]
        w = w * 10 ** ((3 * np.sin(2 * np.pi * 0.3 * t) + 2 * slow) / 20)   # 0.3Hz の ±3dB と、ならした乱数の ±2dB
    elif kind == 'tones':
        w = sg.noise(rng, 'fan', n, 0)
        w = w / np.sqrt(np.mean(w ** 2)) + sg.tones(n) * 10 ** (50 / 20)     # tones は -50dBFS の fan に対しての大きさ
    else:
        w = sg.noise(rng, kind, n, 0)
    return w * rms / np.sqrt(np.mean(w ** 2))


# ---------------- 測る ----------------
def db(p):
    return 10 * np.log10(p + 1e-20)


def spec(x, n, hop):
    return np.abs(stft(x, SR, nperseg=n, noverlap=n - hop, boundary=None, padded=False)[2]) ** 2


def frames(mask, n, hop, nfr, cond):
    v = np.array([cond(mask[i * hop:i * hop + n].mean()) for i in range(nfr)])
    v[:3 * SR // hop + 1] = False   # 最初の 3 秒（ノイズ推定が落ち着くまで）は数えない
    return v


def wobble(G, ok):
    a4, a32, a128 = (uniform_filter1d(G, k, axis=1) for k in (4, 32, 128))
    r = lambda d: np.sqrt(np.mean((d ** 2)[ok])) if ok.any() else np.nan
    return r(G - a4), r(a4 - a32), r(a32 - a128)


def fmask(n):
    f = np.arange(n // 2 + 1) * SR / n
    return (f >= 500) & (f < 10000)


F512 = np.arange(257) * SR / 512
EDGES = 500 * 2 ** (np.arange(0, 14.1) / 3)
BANDS = [(F512 >= lo) & (F512 < hi) for lo, hi in zip(EDGES[:-1], EDGES[1:]) if hi <= 10000]


def kurt(q, sel):
    v = q[sel]
    m = v.mean()
    return np.mean((v - m) ** 4) / np.mean((v - m) ** 2) ** 2


def local(p):
    return p / (uniform_filter(p, size=(7, 33), mode='nearest') + 1e-30)


def measure(c, mask, n, y, yc, yn):
    m = {}
    speech = mask.copy(); speech[:3 * SR] = False
    gaps = ~mask; gaps[:3 * SR] = False

    # 声の分
    m['声の削れ'] = db(np.mean(c[speech] ** 2)) - db(np.mean(yc[speech] ** 2))
    b = fmask(512)
    C, Yc = spec(c, 512, 128)[b], spec(yc, 512, 128)[b]
    v = frames(mask, 512, 128, C.shape[1], lambda r: r > 0.5)
    G = np.clip(db(Yc) - db(C), -30, 6)
    ok = (db(C) > db(C.max()) - 50) & v[None, :]
    m['声_速い揺れ'], m['声_中の揺れ'], m['声_遅い揺れ'] = wobble(G, ok)
    b1 = fmask(1024)
    C1, Yc1 = spec(c, 1024, 256)[b1], spec(yc, 1024, 256)[b1]
    v1 = frames(mask, 1024, 256, C1.shape[1], lambda r: r > 0.5)
    G1 = np.clip(db(Yc1) - db(C1), -30, 6)
    ok1 = (db(C1) > db(C1.max()) - 50) & v1[None, :]
    okf = ok1[2:] & ok1[1:-1] & ok1[:-2]
    m['声_周波ギザ'] = np.mean(np.abs(G1[2:] - 2 * G1[1:-1] + G1[:-2])[okf])

    # ノイズの分
    m['合間の下がり'] = db(np.mean(n[gaps] ** 2)) - db(np.mean(yn[gaps] ** 2))
    m['声の間のノイズの下がり'] = db(np.mean(n[speech] ** 2)) - db(np.mean(yn[speech] ** 2))
    N, Yn, Ycs = spec(n, 512, 128), spec(yn, 512, 128), spec(yc, 512, 128)
    Nb, Ynb, Ycb = (np.array([X[bb].sum(0) for bb in BANDS]) for X in (N, Yn, Ycs))
    Gn = np.clip(db(Ynb) - db(Nb), -40, 6)
    okn = np.broadcast_to(v[None, :], Gn.shape)
    m['ノイズ_速い揺れ'], m['ノイズ_中の揺れ'], m['ノイズ_遅い揺れ'] = wobble(Gn, okn)
    aud = okn & (db(Ynb) > db(Ycb) - 12)
    m['聞こえるノイズ_速い揺れ'], m['聞こえるノイズ_中の揺れ'], m['聞こえるノイズ_遅い揺れ'] = wobble(Gn, aud)

    # 粒
    for nfft, hop in ((512, 128), (1024, 256)):
        bb = fmask(nfft)
        Nn, Yy, Cc = (spec(a, nfft, hop)[bb] for a in (n, yn, yc))
        vs = np.broadcast_to(frames(mask, nfft, hop, Nn.shape[1], lambda r: r > 0.5)[None, :], Nn.shape)
        vg = np.broadcast_to(frames(mask, nfft, hop, Nn.shape[1], lambda r: r == 0)[None, :], Nn.shape)
        au = vs & (db(Yy) > db(Cc) - 12)
        qn, qy = local(Nn), local(Yy)
        hz = f'{SR // nfft}Hz'
        if au.sum() > 200:
            m[f'粒_尖度_聞こえる所_{hz}'] = np.log10(kurt(qy, au) / kurt(qn, au))
            m[f'粒_6dB超_聞こえる所_{hz}'] = (np.mean(qy[au] > 4) - np.mean(qn[au] > 4)) * 1000
            m[f'粒_9dB超_聞こえる所_{hz}'] = (np.mean(qy[au] > 8) - np.mean(qn[au] > 8)) * 1000
        else:
            for k in ('尖度', '6dB超', '9dB超'):
                m[f'粒_{k}_聞こえる所_{hz}'] = np.nan
        if nfft == 512:
            m['粒_尖度_合間'] = np.log10(kurt(qy, vg) / kurt(qn, vg))

    # 境目
    on_, off_, a1, a2 = (np.zeros(len(c), bool) for _ in range(4))
    d = np.diff(mask.astype(int))
    for st in np.flatnonzero(d == 1) + 1:
        if st > 3 * SR:
            on_[st:st + int(0.02 * SR)] = True
    for en in np.flatnonzero(d == -1) + 1:
        if en > 3 * SR:
            off_[en - int(0.05 * SR):en] = True
            a1[en:en + int(0.1 * SR)] = True
            a2[en + int(0.1 * SR):en + int(0.3 * SR)] = True
    a1 &= ~mask; a2 &= ~mask
    m['語頭の削れ'] = db(np.mean(c[on_] ** 2)) - db(np.mean(yc[on_] ** 2))
    m['語尾の削れ'] = db(np.mean(c[off_] ** 2)) - db(np.mean(yc[off_] ** 2))
    m['話し終わり0-100msのノイズの下がり'] = db(np.mean(n[a1] ** 2)) - db(np.mean(yn[a1] ** 2))
    m['話し終わり100-300msのノイズの下がり'] = db(np.mean(n[a2] ** 2)) - db(np.mean(yn[a2] ** 2))
    return m


# 大きいほど良い指標（それ以外は小さいほど良い）
HIGHER = ('下がり', 'PESQ', 'STOI', 'DNSMOS')


def better(k, d):
    return d > 0 if any(h in k for h in HIGHER) else d < 0


# ---------------- 品質 ----------------
def quality_scorer():
    try:
        import onnxruntime as ort
        from pesq import pesq
        from pystoi import stoi
    except ImportError as e:
        print(f'品質の点数は省く（{e.name} がない）')
        return None
    if not os.path.exists(DNSMOS):
        print(f'品質の点数は省く（{DNSMOS} がない）')
        return None
    sess = ort.InferenceSession(DNSMOS)
    PS, PB, PO = (np.poly1d(p) for p in ([-0.08397278, 1.22083953, 0.0052439], [-0.13166888, 1.60915514, -0.39604546],
                                         [-0.06766283, 1.11546468, 0.04602535]))

    def dnsmos(x16):
        L, a = 144160, x16
        while len(a) < L:
            a = np.concatenate([a, x16])
        r = []
        for i in range(int(np.floor(len(a) / 16000) - 9.01) + 1):
            seg = a[i * 16000:i * 16000 + L]
            if len(seg) == L:
                s_, b_, o_ = sess.run(None, {'input_1': seg[None].astype(np.float32)})[0][0]
                r.append((PS(s_), PB(b_), PO(o_)))
        return np.mean(r, axis=0)

    def score(c, y):
        c16, y16 = resample_poly(c, 1, 3), resample_poly(y, 1, 3)
        n = min(len(c16), len(y16))
        sig, bak, ovr = dnsmos(y16[:n])
        try:
            pq = pesq(16000, c16[:n], y16[:n], 'wb')
        except Exception:
            pq = np.nan
        return {'PESQ': pq, 'STOI': stoi(c, y[:len(c)], SR), 'DNSMOS_SIG': sig, 'DNSMOS_BAK': bak, 'DNSMOS_OVR': ovr}
    return score


# ---------------- 本体 ----------------
def main():
    import warnings
    warnings.filterwarnings('ignore')
    opt = parse(sys.argv[1:])
    sets = {'A': opt['a'] + opt['with'], 'B': opt['b'] + opt['with']}
    tag = {k: hashlib.md5(' '.join(sorted(v)).encode()).hexdigest()[:10] for k, v in sets.items()}
    voices = load_voices(opt['quick'])
    noises = ['pink', 'fan', 'fan_mod'] if opt['quick'] else ['white', 'pink', 'fan', 'tones', 'fan_mod']
    snrs = [25, 15] if opt['quick'] else [25, 15, 5]
    seeds = [0] if opt['quick'] else [0, 1]
    os.makedirs(WORK, exist_ok=True)
    P = lambda *a: os.path.join(WORK, '_'.join(str(x) for x in a) + '.wav')

    conds, jobs = [], []
    for (vn, (c, mask)), kind, snr, seed in itertools.product(voices.items(), noises, snrs, seeds):
        key = (vn, kind, snr, seed)
        conds.append(key)
        fx, fc, fn = P('in', *key, 'x'), P('in', *key, 'c'), P('in', *key, 'n')
        if not os.path.exists(fn):
            nz = make_noise(kind, len(c), np.sqrt(np.mean(c[mask] ** 2)) * 10 ** (-snr / 20), seed)
            for p, x in ((fx, c + nz), (fc, c), (fn, nz)):
                wavfile.write(p, SR, x.astype(np.float32))
        for s in sets:
            o = [P(tag[s], *key, t) for t in 'ycn']
            if not os.path.exists(o[2]):
                jobs.append((fx, fc, fn, o, sets[s]))
    print(f'A: {" ".join(sets["A"]) or "初期値"}\nB: {" ".join(sets["B"]) or "初期値"}\n{len(conds)} 条件。処理: {len(jobs)} 本', flush=True)

    def render(j):
        fx, fc, fn, o, s = j
        sg.render(fx, o[0], s, ['--shadow', fc, o[1], '--shadow', fn, o[2]])
    with ThreadPoolExecutor(8) as ex:
        list(ex.map(render, jobs))

    rd = lambda p: wavfile.read(p)[1].astype(float)
    score = quality_scorer() if opt['quality'] else None

    def task(t):
        key, s = t
        c, mask = voices[key[0]]
        y, yc, yn = (rd(P(tag[s], *key, k)) for k in 'ycn')
        m = measure(c, mask, rd(P('in', *key, 'n')), y, yc, yn)
        if score is not None:
            m.update(score(c, y))
        return m
    tasks = [(k, s) for k in conds for s in sets]
    with ThreadPoolExecutor(8) as ex:
        R = dict(zip(tasks, ex.map(task, tasks)))

    keys = list(R[tasks[0]].keys())
    A = {k: np.array([R[(c, 'A')][k] for c in conds]) for k in keys}
    B = {k: np.array([R[(c, 'B')][k] for c in conds]) for k in keys}
    print(f'\n{"指標":34s}{"A":>9s}{"B":>9s}{"B−A":>9s}{"変化":>8s}{"B が良い条件":>12s}')
    for k in keys:
        ok = np.isfinite(A[k]) & np.isfinite(B[k])
        d = (B[k] - A[k])[ok]
        a = np.mean(A[k][ok])
        rel = f'{np.mean(d) / abs(a) * 100:+7.1f}%' if abs(a) > 1e-6 and '粒' not in k else ''
        print(f'{k:34s}{a:9.3f}{np.mean(B[k][ok]):9.3f}{np.mean(d):+9.3f}{rel:>8s}{np.mean(better(k, d)) * 100:10.0f}%')

    show = ['声_速い揺れ', '声_遅い揺れ', '声_周波ギザ', 'ノイズ_速い揺れ', '聞こえるノイズ_遅い揺れ', f'粒_6dB超_聞こえる所_{SR // 1024}Hz',
            '声の削れ', '声の間のノイズの下がり'] + (['DNSMOS_OVR'] if score is not None else [])
    for idx, title in ((2, 'SNR（dB）'), (1, 'ノイズ')):
        names = list(dict.fromkeys(c[idx] for c in conds))
        print(f'\nB−A を{title}ごとに' + ' ' * (24 - len(title)) + ''.join(f'{str(n):>9s}' for n in names))
        for k in show:
            row = ''.join(f'{np.nanmean([R[(c, "B")][k] - R[(c, "A")][k] for c in conds if c[idx] == nm]):+9.3f}' for nm in names)
            print(f'{k:34s}{row}')


if __name__ == '__main__':
    main()
