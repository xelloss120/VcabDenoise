# 評価で使う、入力の音の作り方と vcabdn_render の呼び出し（VoiChanAlgoBox の signals.py から、使う部分を移したもの）
import os
import subprocess

import numpy as np
from scipy.io import wavfile
from scipy.signal import lfilter

SR = 48000
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
WORK = os.path.join(REPO, 'build', 'regression')                      # 入力・結果の置き場（git の外）
EXE = os.path.join(REPO, 'build', 'vcabdn_render_artefacts', 'Release', 'vcabdn_render.exe')
TTS = os.path.join(WORK, 'tts.wav')                                     # make_tts.ps1 で作る（Windows の音声合成）


def render(src, out, sets, extra_args=()):
    """vcabdn_render で src を処理して out に書く。sets は 'key=value' の並び"""
    args = [EXE, '--in', src, '--out', out, *extra_args]
    for s in sets:
        args += ['--set', s]
    return subprocess.run(args, check=True, capture_output=True, text=True, encoding='utf-8', errors='replace')


def noise(rng, kind, n, db):
    """白色 / ピンク / 低めのこもった雑音（fan）/ ファン＋狭帯域ノイズ（tones）。RMS を db dBFS に合わせる（tones は広い帯域の分）"""
    w = rng.standard_normal(n)
    if kind == 'pink':
        w = lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400], w)
    elif kind in ('fan', 'tones'):
        w = lfilter([1], [1, -0.97], w)
    w = w * 10 ** (db / 20) / np.sqrt(np.mean(w ** 2))
    if kind == 'tones':
        w = w + tones(n)
    return w


def tones(n):
    """狭帯域の持続音: 55.7Hz（-45dBFS の振幅）と、1.6kHz → 8 秒で 1.95kHz に飛ぶ音（-55dBFS の振幅）。
    -50dBFS の fan のノイズより、どちらも 20dB ほど突き出る（実際の録音の 55.7Hz の音と同じくらい）"""
    t = np.arange(n) / SR
    low = 10 ** (-45 / 20) * np.sin(2 * np.pi * 55.7 * t)
    f = np.where(t < 8.0, 1600.0, 1950.0)
    phase = 2 * np.pi * np.cumsum(f) / SR
    high = 10 ** (-55 / 20) * np.sin(phase)
    return low + high


def tts_speech():
    """音声合成の声（前に 2 秒、後に 0.5 秒の無音）と、声の区間（10ms の音量が最大から 45dB 以内）"""
    if not os.path.exists(TTS):
        raise FileNotFoundError(f'{TTS} がありません。make_tts.ps1 で作ってください')
    _, x = wavfile.read(TTS)
    x = x.astype(float) / 32768.0
    x = np.concatenate([np.zeros(2 * SR), x, np.zeros(SR // 2)])
    x *= 0.3 / np.max(np.abs(x))
    e = np.convolve(x ** 2, np.ones(480) / 480, 'same')
    return x, e > np.max(e) * 10 ** (-45 / 10)
