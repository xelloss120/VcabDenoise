#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
juce::String utf8 (const char* s) { return juce::String (juce::CharPointer_UTF8 (s)); }
}

juce::AudioProcessorValueTreeState::ParameterLayout VcabDenoiseProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (const auto& s : vcab::paramSpecs())
    {
        const juce::ParameterID id { s.key, 1 };
        const auto name = utf8 (s.name);
        const auto unit = utf8 (s.unit);

        switch (s.type)
        {
            case vcab::ParamType::floating:
            {
                // ホストには刻みのない「連続した値」として伝える（刻みは画面のつまみの側でだけ付ける。VoiChanAlgoBox と同じ理由）
                juce::NormalisableRange<float> range (s.minValue, s.maxValue);
                if (s.skewCentre > 0)
                    range.setSkewForCentre (s.skewCentre);

                const bool wholeSteps = s.step >= 1.0f;
                const bool fine = s.step > 0 && s.step < 0.1f;
                auto attributes = juce::AudioParameterFloatAttributes()
                                      .withLabel (unit)
                                      .withValueFromStringFunction ([] (const juce::String& text) { return text.getFloatValue(); })
                                      .withStringFromValueFunction ([unit, wholeSteps, fine] (float v, int)
                                      {
                                          // 100 以上と、1 刻みのものは小数点なし。0.05 刻みのもの（秒）は小数 2 桁
                                          const auto number = wholeSteps || std::abs (v) >= 100.0f ? juce::String (juce::roundToInt (v))
                                                                                                    : juce::String (v, fine ? 2 : 1);
                                          return number + " " + unit;
                                      });
                layout.add (std::make_unique<juce::AudioParameterFloat> (id, name, range, s.defaultValue, attributes));
                break;
            }
            case vcab::ParamType::toggle:
            {
                auto attributes = juce::AudioParameterBoolAttributes().withAutomatable (! vcab::isUiSetting (s.id));
                layout.add (std::make_unique<juce::AudioParameterBool> (id, name, s.defaultValue >= 0.5f, attributes));
                break;
            }
            case vcab::ParamType::choice:
            {
                juce::StringArray items;
                for (auto* c : s.choices)
                    items.add (utf8 (c));
                // 選択肢が 1 つだけの選択式は作らない（JUCE の中で NaN になり、ホストが止まらなくなる。VoiChanAlgoBox で見つけた問題）
                jassert (items.size() >= 2);
                auto attributes = juce::AudioParameterChoiceAttributes().withAutomatable (! vcab::isUiSetting (s.id));
                layout.add (std::make_unique<juce::AudioParameterChoice> (id, name, items, static_cast<int> (s.defaultValue), attributes));
                break;
            }
        }
    }

    return layout;
}

VcabDenoiseProcessor::VcabDenoiseProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VcabDenoise", createLayout())
{
    for (const auto& s : vcab::paramSpecs())
        raw[static_cast<size_t> (s.id)] = apvts.getRawParameterValue (s.key);
    bypassParam = apvts.getParameter (vcab::spec (vcab::P::bypass).key);
}

bool VcabDenoiseProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    // 入出力はモノラルとステレオ（ステレオの入力は、入力チャンネルの設定で選ぶ）
    const bool inOk = in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
    const bool outOk = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    return inOk && outOk;
}

vcab::ParamValues VcabDenoiseProcessor::currentParams() const
{
    vcab::ParamValues p;
    for (size_t i = 0; i < raw.size(); ++i)
        p.v[i] = raw[i]->load();
    return p;
}

void VcabDenoiseProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate.store (sampleRate);
    engine.prepare (sampleRate, samplesPerBlock);
    mono.assign (static_cast<size_t> (juce::jmax (samplesPerBlock, 1)), 0.0f);
    setLatencySamples (0); // 遅延 0（分析は FFT、音には最小位相の FIR をかける）
}

void VcabDenoiseProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int total = buffer.getNumSamples();
    if (total == 0 || mono.empty())
        return;

    const auto params = currentParams();
    engine.setRealtime (! isNonRealtime()); // ホストが書き出し中なら、声の判定をその場で計算する

    // prepareToPlay で申告されたより大きな塊が来たときは、mono の大きさごとに区切る（音の処理のスレッドで確保しないように）
    const int step = static_cast<int> (mono.size());
    for (int start = 0; start < total; start += step)
    {
        const int n = std::min (step, total - start);
        mixToMono (buffer, start, n, params.index (vcab::P::inputChannel));

        inputAnalysis.push (mono.data(), n);
        engine.process (mono.data(), n, params);
        outputAnalysis.push (mono.data(), n);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.copyFrom (ch, start, mono.data(), n);
    }
}

void VcabDenoiseProcessor::mixToMono (const juce::AudioBuffer<float>& buffer, int start, int n, int choice)
{
    // 入力チャンネルを選ぶ（マイクが 2ch 側に入っているオーディオインターフェースもあるため。VoiChanAlgoBox と同じ）
    // choice: 0 = 自動、1 = 1ch、2 = 2ch、3 = 平均
    const int numIn = getTotalNumInputChannels();
    auto* out = mono.data();
    if (numIn <= 0)
    {
        std::fill (out, out + n, 0.0f);
        return;
    }
    const float* in[2] = { buffer.getReadPointer (0, start), buffer.getReadPointer (numIn >= 2 ? 1 : 0, start) };
    if (numIn < 2 || choice == 1)
    {
        std::copy (in[0], in[0] + n, out);
        return;
    }
    if (choice == 2)
    {
        std::copy (in[1], in[1] + n, out);
        return;
    }
    if (choice == 3)
    {
        for (int i = 0; i < n; ++i)
            out[i] = 0.5f * (in[0][i] + in[1][i]);
        return;
    }

    // 自動: 大きいほうを使う。各チャンネルの音量を 0.3 秒でならし、もう片方が 2 秒続けて 6dB 以上大きいと、10ms でつないで切り替える
    // （VcabShift と同じ形）
    const double fs = getSampleRate();
    const float a = static_cast<float> (std::exp (-n / (kAutoSmoothSeconds * fs)));
    for (int c = 0; c < 2; ++c)
    {
        double e = 0.0;
        for (int i = 0; i < n; ++i)
            e += static_cast<double> (in[c][i]) * in[c][i];
        autoPower[c] = a * autoPower[c] + (1.0f - a) * static_cast<float> (e / n);
    }
    const int other = 1 - autoChannel;
    if (autoPower[other] > autoPower[autoChannel] * kAutoSwitchRatio && autoPower[other] > 1.0e-10f)
    {
        otherLouderFor += n;
        if (otherLouderFor >= kAutoSwitchSeconds * fs)
        {
            autoPrev = autoChannel;
            autoChannel = other;
            autoFade = 0.0f;
            otherLouderFor = 0;
        }
    }
    else
        otherLouderFor = 0;

    const float fadeStep = static_cast<float> (1.0 / (kAutoFadeSeconds * fs));
    for (int i = 0; i < n; ++i)
    {
        if (autoFade < 1.0f)
        {
            out[i] = autoFade * in[autoChannel][i] + (1.0f - autoFade) * in[autoPrev][i];
            autoFade = std::min (1.0f, autoFade + fadeStep);
        }
        else
            out[i] = in[autoChannel][i];
    }
    usedInputChannel.store (autoChannel);
}

juce::AudioProcessorEditor* VcabDenoiseProcessor::createEditor()
{
    return new VcabDenoiseEditor (*this);
}

void VcabDenoiseProcessor::resetAllParameters()
{
    // 入力チャンネルは環境（オーディオインターフェース）に合わせた設定なので、戻さない。学習したノイズも消さない
    for (auto* param : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            if (ranged->getParameterID() != vcab::spec (vcab::P::inputChannel).key)
            {
                // ホストに 1 回の操作として伝える（オートメーションの書き込みや元に戻すで、値が飛ばないように）
                ranged->beginChangeGesture();
                ranged->setValueNotifyingHost (ranged->getDefaultValue());
                ranged->endChangeGesture();
            }
}

void VcabDenoiseProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
    {
        // 学習した値（常に保存する。消したいときは「学習ノイズ削除」）。周波数の曲線なので、サンプルレートが変わっても使える
        xml->removeAttribute ("learnedNoise");
        const auto learned = engine.exportLearned();
        if (! learned.empty())
        {
            juce::MemoryBlock block (learned.data(), learned.size() * sizeof (float));
            xml->setAttribute ("learnedNoise", block.toBase64Encoding());
        }
        copyXmlToBinary (*xml, destData);
    }
}

void VcabDenoiseProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            // 学習した値。入っていない設定を読み込んだときは、空を渡して消す（前に覚えた値が残らないように）
            std::vector<float> learned;
            juce::MemoryBlock block;
            if (xml->hasAttribute ("learnedNoise") && block.fromBase64Encoding (xml->getStringAttribute ("learnedNoise")))
            {
                learned.resize (block.getSize() / sizeof (float));
                std::memcpy (learned.data(), block.getData(), learned.size() * sizeof (float));
            }
            engine.importLearned (std::move (learned));
            xml->removeAttribute ("learnedNoise"); // 設定値の木には入れない
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VcabDenoiseProcessor();
}
