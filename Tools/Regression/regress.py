# 回帰テスト: 処理の音がビット単位で変わっていないかを確かめる（VoiChanAlgoBox の regress.py と同じ形）
#   python regress.py make    今のビルドで基準を作る（build/regression/ref）
#   python regress.py check   今のビルドの音を基準と比べる（結果は build/regression/out）
#   python regress.py check 名前の一部 ...   名前にその文字列を含むものだけ
# 先に make_tts.ps1 で build/regression/tts.wav を作っておく。
import os
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

import signals as sg


# ---- 入力 ----
def make_inputs():
    os.makedirs(os.path.join(sg.WORK, 'in'), exist_ok=True)
    x, _ = sg.tts_speech()
    tones = x + sg.noise(np.random.default_rng(1), 'tones', len(x), -50)
    inputs = {
        'clean': (x, sg.SR),                                                            # 声だけ
        'pink': (x + sg.noise(np.random.default_rng(1), 'pink', len(x), -45), sg.SR),    # 声 + ピンクノイズ -45dBFS
        'tones': (tones, sg.SR),                                                        # 声 + ファンのノイズ -50dBFS + 狭帯域ノイズ（8 秒で飛ぶ音を含む）
        'tones96': (resample_poly(tones, 2, 1), 2 * sg.SR),                              # 同じ音の 96kHz（FFT と FIR が 2 倍の大きさになる）
    }
    paths = {}
    for name, (sig, sr) in inputs.items():
        paths[name] = os.path.join(sg.WORK, 'in', name + '.wav')
        wavfile.write(paths[name], sr, sig.astype(np.float32))
    return paths


# ---- 場合 ----（名前, 入力, 設定, ツールに渡すほかの引数）
CASES = []
def case(name, inp, sets=(), extra=()):
    CASES.append((name, inp, list(sets), list(extra)))

case('clean_default', 'clean')
case('pink_default', 'pink')
case('pink_strong', 'pink', ['max_reduction=30', 'strength=9'])
case('pink_presence', 'pink', ['presence_on=1'])
case('pink_cepstrum', 'pink', ['cepstrum_on=1'])
case('pink_presence_cepstrum', 'pink', ['presence_on=1', 'cepstrum_on=1'])
case('pink_no_onset', 'pink', ['onset_protect=0'])
case('pink_fast', 'pink', ['attack=2', 'release=30'])
case('pink_rnn_low', 'pink', ['rnn_threshold=30'])
case('pink_learned', 'pink', ['noise_mode=1'], ['--learn', '0.2:1.8'])
case('pink_learned_none', 'pink', ['noise_mode=1'])  # 学習していないときは自動追従の値を使う
case('pink_block480', 'pink', [], ['--block', '480'])
case('tones_default', 'tones')
case('tones_noise_only', 'tones', ['noise_only=1'])
case('tones_bypass', 'tones', ['bypass=1'])
case('tones_off', 'tones', ['tonal_on=0'])
case('tones_sensitive', 'tones', ['tonal_sensitivity=9', 'tonal_count=8', 'tonal_max_depth=40'])
case('tones_no_jump', 'tones', ['tonal_jump_time=0'])
case('tones_width', 'tones', ['tonal_width=140', 'tonal_low_width=40', 'tonal_neighbor=350'])
case('tones_slow', 'tones', ['tonal_activate=2', 'tonal_release=4'])
case('tones96_default', 'tones96')


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'check'
    filters = sys.argv[2:]
    inputs = make_inputs()
    target = os.path.join(sg.WORK, 'ref' if mode == 'make' else 'out')
    os.makedirs(target, exist_ok=True)
    bad = 0
    for name, inp, sets, extra in CASES:
        if filters and not any(f in name for f in filters):
            continue
        out = os.path.join(target, name + '.wav')
        sg.render(inputs[inp], out, sets, extra)
        if mode == 'check':
            _, a = wavfile.read(os.path.join(sg.WORK, 'ref', name + '.wav'))
            _, b = wavfile.read(out)
            d = np.max(np.abs(a.astype(float) - b.astype(float))) if a.shape == b.shape else float('inf')
            if d != 0:
                bad += 1
                print(f'{name:24s} 違う（最大の差 {d:.2e}）')
    print('作った' if mode == 'make' else f'ずれ {bad} 件')


if __name__ == '__main__':
    main()
