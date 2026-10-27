#include "MonitorView.h"

#include "EditorStyle.h"

#include <algorithm>

using namespace editor;

namespace
{
constexpr float kNotchFullDepthDb = 24.0f; // 狭帯域ノイズの帯を一番濃くする深さ（深さ上限の初期値）
} // namespace

MonitorView::MonitorView (VcabDenoiseProcessor& p)
    : processor (p)
{
    // 背景は自分で全部塗るので、描き直すときに後ろの画面まで描き直さなくてよい（軽くするため）
    setOpaque (true);
    window.resize (fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[static_cast<size_t> (i)] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * static_cast<float> (i) / fftSize);
    fftBuffer.resize (2 * fftSize);
}

void MonitorView::update()
{
    // 非表示のときは、分析も描画もしない（溜まった音は捨てる）
    const bool shown = isShowing();
    voiceHistory[static_cast<size_t> (voicePos)] = processor.meters().voice.load();
    voicePos = (voicePos + 1) % voiceSteps;
    processor.inputAnalysis.pull (pulled);
    if (shown)
        analyse (input, pulled);
    processor.outputAnalysis.pull (pulled);
    if (shown)
    {
        analyse (output, pulled);
        repaint();
    }
}

void MonitorView::analyse (Channel& ch, const std::vector<float>& incoming)
{
    for (float s : incoming)
    {
        ch.history[static_cast<size_t> (ch.writePos)] = s;
        ch.writePos = (ch.writePos + 1) % fftSize;
        if (++ch.sinceLastFrame >= hopSize)
        {
            ch.sinceLastFrame = 0;
            computeFrame (ch);
        }
    }
}

void MonitorView::computeFrame (Channel& ch)
{
    std::fill (fftBuffer.begin(), fftBuffer.end(), 0.0f);
    for (int i = 0; i < fftSize; ++i)
        fftBuffer[static_cast<size_t> (i)] = ch.history[static_cast<size_t> ((ch.writePos + i) % fftSize)] * window[static_cast<size_t> (i)];
    fft.performFrequencyOnlyForwardTransform (fftBuffer.data(), true);

    // 正弦波の振幅が 0dB になるように正規化（ハン窓の平均 0.5 と、片側スペクトルの 2倍）
    const float norm = 2.0f / (fftSize * 0.5f);
    for (int k = 0; k <= fftSize / 2; ++k)
    {
        const float db = juce::Decibels::gainToDecibels (fftBuffer[static_cast<size_t> (k)] * norm, minDb);
        // スペクトルの線は、上がるときは速く、下がるときはゆっくり
        auto& s = ch.spectrumDb[static_cast<size_t> (k)];
        s = db > s ? db : s + (db - s) * 0.15f;
    }
}

float MonitorView::xForHz (float hz, juce::Rectangle<float> area) const
{
    const float t = std::log (hz / minHz) / std::log (maxHz / minHz);
    return area.getX() + t * area.getWidth();
}

float MonitorView::yForDb (float db, juce::Rectangle<float> area) const
{
    const float t = (db - minDb) / (maxDb - minDb);
    return area.getBottom() - t * area.getHeight();
}

void MonitorView::paint (juce::Graphics& g)
{
    g.fillAll (kGraph);
    auto area = getLocalBounds().toFloat();
    const auto band = area.removeFromBottom (static_cast<float> (voiceBand));
    g.setColour (juce::Colour (0xff15202f));
    g.fillRect (area);

    // 声の帯（右の端が今。1 秒ごとに目盛り）
    {
        const float step = band.getWidth() / voiceSteps;
        g.setColour (kVoiceColour.withAlpha (0.85f));
        for (int i = 0; i < voiceSteps; ++i)
            if (voiceHistory[static_cast<size_t> ((voicePos + i) % voiceSteps)])
                g.fillRect (band.getX() + step * static_cast<float> (i), band.getY() + 3, step + 0.5f, band.getHeight() - 5);
        g.setColour (juce::Colours::white.withAlpha (0.15f));
        for (int s = 1; s < voiceSteps / 30; ++s)
            g.drawVerticalLine (static_cast<int> (band.getRight() - step * 30.0f * static_cast<float> (s)), band.getY() + 3, band.getBottom() - 2);
    }
    g.setFont (juce::FontOptions (17.0f));

    // 目盛り
    for (float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xForHz (hz, area);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawVerticalLine (static_cast<int> (x), area.getY(), area.getBottom());
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        const auto text = hz >= 1000 ? juce::String (hz / 1000, 0) + "k" : juce::String (static_cast<int> (hz));
        g.drawText (text, juce::Rectangle<float> (x + 2, area.getBottom() - 22, 50, 20), juce::Justification::left);
    }
    for (float db : { -100.0f, -80.0f, -60.0f, -40.0f, -20.0f })
    {
        const float y = yForDb (db, area);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawHorizontalLine (static_cast<int> (y), area.getX(), area.getRight());
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.drawText (juce::String (static_cast<int> (db)), juce::Rectangle<float> (area.getX() + 2, y - 20, 44, 20), juce::Justification::left);
    }

    // 抑制している狭帯域ノイズの谷の幅（半値幅）を、薄い帯で出す（スペクトルの線の後ろ）。深く抑制しているほど濃くする
    {
        auto& m = processor.meters();
        const int count = m.notchCount.load();
        for (int i = 0; i < count; ++i)
        {
            const auto k = static_cast<size_t> (i);
            const float hz = m.notchHz[k].load(), half = 0.5f * m.notchWidthHz[k].load();
            float x0 = xForHz (std::max (minHz, hz - half), area), x1 = xForHz (hz + half, area);
            if (x1 - x0 < 2.0f) // 細すぎて見えないときは 2 ピクセルにする
            {
                const float x = xForHz (hz, area);
                x0 = x - 1.0f;
                x1 = x + 1.0f;
            }
            const float depth = juce::jlimit (0.0f, 1.0f, m.notchDepthDb[k].load() / kNotchFullDepthDb);
            g.setColour (kNotchColour.withAlpha (0.08f + 0.17f * depth));
            g.fillRect (juce::Rectangle<float> (x0, area.getY(), x1 - x0, area.getHeight()));
        }
    }

    const double sr = processor.currentSampleRate.load();
    auto drawCurve = [&] (const Channel& ch, juce::Colour colour)
    {
        juce::Path path;
        const int w = static_cast<int> (area.getWidth());
        for (int px = 0; px < w; px += 2)
        {
            const float t = static_cast<float> (px) / static_cast<float> (w);
            const float hz = minHz * std::pow (maxHz / minHz, t);
            const int bin = juce::jlimit (1, fftSize / 2, static_cast<int> (hz / sr * fftSize + 0.5));
            const float y = yForDb (ch.spectrumDb[static_cast<size_t> (bin)], area);
            const float x = area.getX() + static_cast<float> (px);
            if (px == 0) path.startNewSubPath (x, y);
            else path.lineTo (x, y);
        }
        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (1.5f));
    };
    drawCurve (input, kInputColour);
    drawCurve (output, kOutputColour);

    const auto p = processor.currentParams();
    auto& m = processor.meters();

    // ノイズ推定（緑の点線）。見積もりは「白色雑音に直したときの 1 サンプルあたりの 2 乗」なので、
    // この表示（ハン窓の fftSize 点、大きさ × 4 / fftSize）に直す: 白色雑音の分散 σ² は 10log10(σ² × 6 / fftSize) dB になる。
    // 表示のスペクトルは dB でならしているので、ノイズは 2 乗の平均より約 2.5dB 低く見える。その分も引く（VoiChanAlgoBox と同じ）
    if (p.on (vcab::P::showNoiseEstimate) && m.noiseCurveValid.load())
    {
        const float offset = 10.0f * std::log10 (6.0f / fftSize) - 2.5f;
        juce::Path path;
        for (int i = 0; i < vcab::EngineMeters::kNoiseCurvePoints; ++i)
        {
            const float x = xForHz (vcab::EngineMeters::noiseCurveHz (i), area);
            const float y = yForDb (m.noiseCurveDb[static_cast<size_t> (i)].load() + offset, area);
            if (i == 0) path.startNewSubPath (x, y);
            else path.lineTo (x, y);
        }
        // 自動追従のとき、声ありの間は推定を止めているので、線をグレーにする
        // 学習固定でも、まだ学習していないときは自動追従の値で抑制している
        const bool learnedMode = static_cast<vcab::NoiseMode> (p.index (vcab::P::noiseMode)) == vcab::NoiseMode::learned;
        const bool learned = learnedMode && m.learned.load();
        const bool paused = ! learned && m.voice.load();
        juce::Path dashed;
        const float dashes[] = { 6.0f, 4.0f };
        juce::PathStrokeType (2.0f).createDashedStroke (dashed, path, dashes, 2);
        g.setColour (paused ? kPausedColour : kNoiseColour);
        g.fillPath (dashed);
        const char* title = learned ? "ノイズ推定（学習固定）" : learnedMode ? "ノイズ推定（未学習のため自動追従）" : "ノイズ推定（自動追従）";
        g.drawText (utf8 (title), juce::Rectangle<float> (area.getX() + 92, area.getY() + 3, 280, 20), juce::Justification::left);
    }

    // 抑制している狭帯域ノイズ（▼ と周波数）。凡例の下の段に出す。周波数の文字は ▼ の右に書き、
    // 次の ▼ や前の文字と重なるときは省く（近い音が並ぶときは、いちばん右の音にだけ書く）
    g.setColour (kNotchColour);
    const int count = m.notchCount.load();
    std::array<float, vcab::TonalTracker::kMaxNotches> xs {};
    for (int i = 0; i < count; ++i)
        xs[static_cast<size_t> (i)] = xForHz (m.notchHz[static_cast<size_t> (i)].load(), area);
    std::array<int, vcab::TonalTracker::kMaxNotches> order {};
    for (int i = 0; i < count; ++i)
        order[static_cast<size_t> (i)] = i;
    std::sort (order.begin(), order.begin() + count, [&xs] (int a, int b) { return xs[static_cast<size_t> (a)] < xs[static_cast<size_t> (b)]; });
    const float top = area.getY() + 28;
    float textEnd = -1.0e9f;
    for (int j = 0; j < count; ++j)
    {
        const int i = order[static_cast<size_t> (j)];
        const float x = xs[static_cast<size_t> (i)];
        juce::Path tri;
        tri.addTriangle (x - 6, top, x + 6, top, x, top + 10);
        g.fillPath (tri);
        const auto text = juce::String (juce::roundToInt (m.notchHz[static_cast<size_t> (i)].load())) + "Hz";
        const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 2.0f;
        const float nextX = j + 1 < count ? xs[static_cast<size_t> (order[static_cast<size_t> (j + 1)])] - 6.0f : area.getRight();
        if (x + 8 > textEnd && x + 8 + w < nextX)
        {
            g.drawText (text, juce::Rectangle<float> (x + 8, top - 5, w, 20), juce::Justification::left);
            textEnd = x + 8 + w;
        }
    }

    // 凡例（左から 狭帯域ノイズ・ノイズ推定、真ん中に 入力・出力、右に 声あり判定）
    const float cx = area.getCentreX();
    g.setColour (kInputColour);
    g.drawText (utf8 ("入力"), juce::Rectangle<float> (cx - 58, area.getY() + 3, 54, 20), juce::Justification::right);
    g.setColour (kOutputColour);
    g.drawText (utf8 ("出力"), juce::Rectangle<float> (cx + 4, area.getY() + 3, 54, 20), juce::Justification::left);
    g.setColour (kNotchColour);
    g.drawText (utf8 ("▼ 狭帯域"), juce::Rectangle<float> (area.getX() + 8, area.getY() + 3, 80, 20), juce::Justification::left);
    g.setColour (kVoiceColour);
    g.drawText (utf8 ("声あり判定"), juce::Rectangle<float> (area.getRight() - 128, area.getY() + 3, 120, 20), juce::Justification::right);

    // 学習中は、グラフの真ん中に大きく出す（学習の間は静かにしていてほしいので、見落とさないように）
    if (m.learning.load())
    {
        const auto box = area.withSizeKeepingCentre (320, 96);
        g.setColour (kGraph.withAlpha (0.85f));
        g.fillRoundedRectangle (box, 8.0f);
        g.setColour (kNoiseColour);
        g.drawRoundedRectangle (box, 8.0f, 1.5f);
        auto inner = box.reduced (8, 10);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
        g.drawText (utf8 ("学習中"), inner.removeFromTop (inner.getHeight() * 0.55f), juce::Justification::centred);
        g.setFont (juce::FontOptions (20.0f));
        g.drawText (utf8 ("お静かに……"), inner, juce::Justification::centred);
    }
}
