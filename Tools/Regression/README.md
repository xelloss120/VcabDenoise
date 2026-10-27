# 回帰テストと評価

`vcabdn_render`（WAV を処理するツール）で決まった入力を処理し、音がビット単位で変わっていないかを確かめます。整理（リファクタリング）の前後や、ほかの処理を直したときに使います。必要なもの: Python 3、numpy、scipy。

1. `powershell -File make_tts.ps1` で、テスト用の声（Windows の日本語の音声合成）を `build/regression/tts.wav` に作る（一度だけ）
2. 変更の前に `python regress.py make` で基準を作る（`build/regression/ref`）
3. 変更してビルドしたら `python regress.py check`（`ずれ 0 件` なら同じ音）

音が変わるのが意図どおりなら、`make` で基準を作り直します。名前の一部を並べると、その場合だけを処理します（例: `python regress.py check tones_`）。
基準は、作ったのと同じ PC・同じコンパイラーで比べます（浮動小数点の計算の順番が変わると、ビット単位では一致しないため）。

`denoise_eval.py` は、デノイズの効き方を測るツールです。ノイズの下がり方と、声・語頭の削れ方を dB で出します（例: `python denoise_eval.py max_reduction=18`、`--kind tones` で狭帯域ノイズ入り）。
