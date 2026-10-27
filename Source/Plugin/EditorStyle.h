// エディタで使う、見た目の値と小さな道具
#pragma once

#include "LookAndFeel.h"

namespace editor
{

inline juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }

// 画面の地の色
inline const juce::Colour kBackground { 0xff17202e };
inline const juce::Colour kPanel { 0xff1e2a3c };
inline const juce::Colour kBox { 0xff273550 };
inline const juce::Colour kText { 0xffe2e8f2 };
inline const juce::Colour kGraph { 0xff0f1724 }; // スペクトルとメーターの下地

// 線と印の色（メーターとスペクトルで同じ色を使う）
inline const juce::Colour kInputColour { 0xff4fa3e0 };  // 入力: 青
inline const juce::Colour kOutputColour { 0xfff0a040 }; // 出力: 橙
inline const juce::Colour kNoiseColour { 0xff60d080 };  // ノイズ推定と抑制量: 緑
inline const juce::Colour kPausedColour { 0xff7a8494 }; // 推定を止めている間: グレー
inline const juce::Colour kNotchColour { 0xffe0c040 };  // 狭帯域ノイズ: 黄
inline const juce::Colour kVoiceColour { 0xffc8a0f0 };  // 声あり判定: 薄紫

inline constexpr float kFont = VCLookAndFeel::kMinFontSize; // 文字の大きさの下限（ツールチップと同じ）
inline constexpr int kMargin = 8;

} // namespace editor
