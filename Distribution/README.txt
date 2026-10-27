VcabDenoise 1.0.0
=================

声のためのノイズ除去（デノイズ）プラグインです（遅延 0）。
PC のファンのような定常的なノイズと、「ふぃーん」「キーン」のような
狭帯域の持続音を下げます。
対応: Windows（64bit）

■ 中身
  VcabDenoise.exe   単体で動く EXE 版
  VcabDenoise.vst3  VST3 プラグイン

■ 使い方
  EXE 版: VcabDenoise.exe を実行し、「オーディオ設定」ボタンで
          入力と出力のデバイスを選びます。
  VST3 版: VcabDenoise.vst3 をフォルダごと
          C:\Program Files\Common Files\VST3 にコピーし、DAW で読み込みます。

■ 注意
  ・ヘッドフォンで使ってください。スピーカーだとハウリング（大きな音）が
    起きることがあります。EXE 版は起動してすぐ音が出ます。
  ・署名をしていないため、初回に「PC が保護されました」と出たら
    「詳細情報」→「実行」を押してください。
  ・EXE 版の設定は %APPDATA%\VcabDenoise に保存されます。

■ 画面
  ふだんは主な操作だけを出します。細かい設定は「詳細」ボタンで開きます。
  詳しい説明は、つまみや項目にマウスを乗せると出ます。

■ ライセンス
  Copyright (C) 2026 x120
  AGPLv3（LICENSE.txt）。使っているライブラリのライセンスは
  THIRD_PARTY_LICENSES.md にあります（GPLv3 の本文は GPL-3.0.txt）。
  この版のソースコード:
    https://github.com/xelloss120/VcabDenoise/releases/tag/v1.0.0
  VST is a trademark of Steinberg Media Technologies GmbH.
