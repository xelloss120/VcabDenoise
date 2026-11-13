# 回帰テストと評価

`vcabdn_render`（WAV を処理するツール）で決まった入力を処理し、音がビット単位で変わっていないかを確かめます。整理（リファクタリング）の前後や、ほかの処理を直したときに使います。必要なもの: Python 3、numpy、scipy。

1. `powershell -File make_tts.ps1` で、テスト用の声（Windows の日本語の音声合成）を `build/regression/tts.wav` に作る（一度だけ）
2. 変更の前に `python regress.py make` で基準を作る（`build/regression/ref`）
3. 変更してビルドしたら `python regress.py check`（`ずれ 0 件` なら同じ音）

音が変わるのが意図どおりなら、`make` で基準を作り直します。名前の一部を並べると、その場合だけを処理します（例: `python regress.py check tones_`）。
基準は、作ったのと同じ PC・同じコンパイラーで比べます（浮動小数点の計算の順番が変わると、ビット単位では一致しないため）。

`denoise_eval.py` は、デノイズの効き方を測るツールです。ノイズの下がり方と、声・語頭の削れ方を dB で出します（例: `python denoise_eval.py max_reduction=18`、`--kind tones` で狭帯域ノイズ入り）。

## 2 つの設定を広い条件で比べる（wide_eval.py）

`wide_eval.py` は、2 つの設定（A と B）を、声 6 種 × ノイズ 5 種 × SNR 3 段 × 乱数 2 通り（180 条件）で比べます。
`vcabdn_render --shadow` で出力を「声の分」と「ノイズの分」に正確に分け、声の削れ、抑制の揺れ（速さ別）とギザギザ、残ったノイズの揺れ、粒（ミュージカルノイズ）、語頭・語尾、品質の点数（PESQ、STOI、DNSMOS）を出します。測るものの詳しい中身は、スクリプトの先頭に書いてあります。

1. `powershell -File make_voices.ps1` で、声（日本語と英語の音声合成）を `build/regression/voices/` に作る（一度だけ）。低い声とささやき声は、スクリプトがこの 2 つから作ります
2. `python wide_eval.py --a max_reduction=20 --b max_reduction=24`（約 10 分。`--quick` で 18 条件・約 1 分、`--with strength=3` で両方に設定を足す、`--no-quality` で品質の点数を省く）

品質の点数には、`pip install pesq pystoi onnxruntime`（pesq は Windows ではビルドが要るので、Visual Studio の `vcvars64.bat` を通した環境で入れる）と、
DNSMOS のモデル（[DNS-Challenge](https://github.com/microsoft/DNS-Challenge) の `DNSMOS/DNSMOS/sig_bak_ovr.onnx` を `build/regression/dnsmos/` に置く）が要ります。ないときは、その点数だけ省きます。
処理した WAV は `build/regression/wide/` に置き、同じ設定なら作り直しません（ビルドし直したときは、このフォルダを消してください）。

## 狭帯域抑制を測る（tonal_eval.py）

`tonal_eval.py` は、狭帯域抑制の設定を並べて比べます（声 6 種 × 飛ぶ持続音 / ハム / 持続音なし。1 つの設定で約 15 秒）。
持続音の消え方、飛んだ後に追いつくまでの時間、狭帯域抑制による声の削れ（平均と上位 1%）、持続音のない所に出た谷の合計（dB・秒）を出します。
声は `wide_eval.py` と同じものを使います（例: `python tonal_eval.py deep:tonal_max_depth=30 slow:tonal_activate=2`）。
