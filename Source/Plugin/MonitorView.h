// モニター: 入力と出力のスペクトル（重ねて表示）と、ノイズ推定の線、抑制している狭帯域ノイズ（▼ と周波数、谷の幅の薄い帯）。
// 下の端の帯には、ここ数秒の声の判定を、右から左へ流して出す。
// VoiChanAlgoBox の MonitorView のうち、スペクトルの部分を移したもの。
#pragma once

#include "PluginProcessor.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

class MonitorView : public juce::Component
{
public:
    explicit MonitorView (VcabDenoiseProcessor& p);

    // 画面のタイマーから呼ぶ。溜まった音を分析して描き直す
    void update();
    void paint (juce::Graphics&) override;

private:
    static constexpr int fftOrder = 12;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int hopSize = 1024;
    static constexpr float minDb = -110.0f;
    static constexpr float maxDb = 0.0f;
    static constexpr float minHz = 20.0f;
    static constexpr float maxHz = 20000.0f;
    static constexpr int voiceSteps = 150; // 声の帯の長さ（画面のタイマー 30Hz で 5 秒）
    static constexpr int voiceBand = 14;   // 声の帯の高さ

    struct Channel
    {
        std::vector<float> history = std::vector<float> (fftSize, 0.0f); // 直近 fftSize サンプル（リングバッファ）
        int writePos = 0;
        int sinceLastFrame = 0;
        std::vector<float> spectrumDb = std::vector<float> (fftSize / 2 + 1, minDb); // なめらかにしたスペクトル
    };

    void analyse (Channel& ch, const std::vector<float>& incoming);
    void computeFrame (Channel& ch);
    float xForHz (float hz, juce::Rectangle<float> area) const;
    float yForDb (float db, juce::Rectangle<float> area) const;

    VcabDenoiseProcessor& processor;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> window, fftBuffer, pulled;
    Channel input, output;
    std::vector<bool> voiceHistory = std::vector<bool> (voiceSteps, false); // 声の判定（リングバッファ）
    int voicePos = 0;
};
