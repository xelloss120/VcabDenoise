// vcabdn_render: 開発用の WAV 処理ツール
//
// WAV を読み込み、本体と同じ Engine で小さな塊ずつ処理して、WAV で書き出す（VoiChanAlgoBox の vcab_render と同じ考え方）。
//
// 使い方の例:
//   vcabdn_render --in noisy.wav --out out.wav --set max_reduction=18
//   vcabdn_render --in noisy.wav --out out.wav --set noise_mode=1 --learn 0:3
//   vcabdn_render --list   （パラメータの一覧）
#include "../../Source/Core/Engine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#if JUCE_WINDOWS
extern "C" __declspec (dllimport) int __stdcall SetConsoleOutputCP (unsigned int);
#endif

namespace
{

void printUsage()
{
    std::cout <<
        "vcabdn_render - VcabDenoise の開発用 WAV 処理ツール\n"
        "\n"
        "  --in <file.wav>        入力の WAV（2ch 以上なら 1ch 目を使う）\n"
        "  --out <file.wav>       出力の WAV\n"
        "  --set <key>=<値>       パラメータを指定（複数可）。choice と on/off は番号（0, 1, ...）\n"
        "  --block <サンプル数>   処理する塊の大きさ（初期値 128）\n"
        "  --learn <始め>:<終わり> この間（秒）、学習ボタンを押したことにする\n"
        "  --save-learned <file>  処理の後に、学習した値をファイルに書き出す（保存の確認用）\n"
        "  --load-learned <file>  処理の前に、学習した値をファイルから読み込む（保存の確認用）\n"
        "  --noise-curve          処理の最後の、ノイズ推定（表示用の値）を出す\n"
        "  --notches              1 秒ごとに、抑制している狭帯域ノイズ（周波数と深さ）を出す\n"
        "  --realtime             実際の時間の速さで処理する（声の判定の作業スレッドが間に合うかを確かめる）\n"
        "  --list                 パラメータの一覧を表示\n"
        "  --selftest             分割して畳み込む FIR を、そのまま畳み込んだ結果と比べる\n";
}

void printParams()
{
    for (const auto& s : vcab::paramSpecs())
    {
        std::cout << s.key << "  [" << s.minValue << " .. " << s.maxValue << "]  初期値 " << s.defaultValue;
        if (! s.choices.empty())
        {
            std::cout << "  {";
            for (size_t i = 0; i < s.choices.size(); ++i)
                std::cout << (i ? ", " : "") << i << ":" << s.choices[i];
            std::cout << "}";
        }
        std::cout << "  " << s.name << "\n";
    }
}

// 分割して畳み込む FIR（FadingFir）を、そのまま畳み込んだ結果と比べる。フィルタは途中で何度も替える
// （フェードの間は、新旧のフィルタの出力を直線で混ぜたものと比べる）
int selfTest()
{
    juce::Random rng (7);
    int failures = 0;
    for (const int taps : { 128, 512, 2048, 4096 })
    {
        const int block = 128, fade = std::max (block, taps / 4); // フェードは塊の倍数（Engine と同じ）
        vcab::FadingFir fir;
        fir.prepare (taps, block, fade);
        const int total = taps * 12;
        std::vector<float> x (static_cast<size_t> (total));
        for (auto& v : x)
            v = rng.nextFloat() * 2.0f - 1.0f;
        std::vector<std::vector<float>> filters;
        std::vector<float> identity (static_cast<size_t> (taps), 0.0f);
        identity[0] = 1.0f;
        std::vector<float> prev = identity, cur = identity;
        int fadeLeft = 0;
        double maxErr = 0, maxRef = 0;
        for (int t = 0; t < total; ++t)
        {
            // フィルタを替える（間隔 fade ごと。ときどき素通し）
            if (t % fade == 0 && t > 0)
            {
                std::vector<float> h (static_cast<size_t> (taps));
                const bool makeIdentity = (t / fade) % 7 == 3;
                for (int i = 0; i < taps; ++i)
                    h[static_cast<size_t> (i)] = makeIdentity ? (i == 0 ? 1.0f : 0.0f)
                                                              : (rng.nextFloat() * 2.0f - 1.0f) * std::exp (-4.0f * i / taps) * 0.2f;
                fir.setFilter (h.data());
                const bool bothIdentity = makeIdentity && cur == identity;
                if (! bothIdentity)
                {
                    prev = cur;
                    fadeLeft = fade;
                }
                cur = h;
            }
            auto conv = [&] (const std::vector<float>& h)
            {
                double y = 0;
                for (int i = 0; i < taps && i <= t; ++i)
                    y += static_cast<double> (h[static_cast<size_t> (i)]) * x[static_cast<size_t> (t - i)];
                return y;
            };
            double ref;
            if (fadeLeft > 0)
            {
                const double w = static_cast<double> (fade - fadeLeft + 1) / fade;
                ref = conv (cur) * w + conv (prev) * (1.0 - w);
                --fadeLeft;
            }
            else
            {
                ref = conv (cur);
            }
            const double y = fir.process (x[static_cast<size_t> (t)]);
            maxErr = std::max (maxErr, std::abs (y - ref));
            maxRef = std::max (maxRef, std::abs (ref));
        }
        const bool ok = maxErr < 1.0e-4 * std::max (1.0, maxRef);
        std::cout << "FIR " << taps << " タップ: 最大の誤差 " << maxErr << "（最大の値 " << maxRef << "） " << (ok ? "一致" : "不一致") << "\n";
        failures += ok ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int main (int argc, char* argv[])
{
#if JUCE_WINDOWS
    SetConsoleOutputCP (65001); // UTF-8
#endif
    juce::String inPath, outPath, saveLearnedPath, loadLearnedPath;
    int block = 128;
    double learnFrom = -1, learnTo = -1;
    bool printNoiseCurve = false, printNotches = false, realtime = false;
    vcab::ParamValues params;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a { juce::CharPointer_UTF8 (argv[i]) };
        auto next = [&]() -> juce::String { return i + 1 < argc ? juce::String (juce::CharPointer_UTF8 (argv[++i])) : juce::String(); };

        if (a == "--in") inPath = next();
        else if (a == "--out") outPath = next();
        else if (a == "--block") block = juce::jmax (1, next().getIntValue());
        else if (a == "--learn")
        {
            const auto v = next();
            learnFrom = v.upToFirstOccurrenceOf (":", false, false).getDoubleValue();
            learnTo = v.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
        }
        else if (a == "--save-learned") saveLearnedPath = next();
        else if (a == "--load-learned") loadLearnedPath = next();
        else if (a == "--noise-curve") printNoiseCurve = true;
        else if (a == "--notches") printNotches = true;
        else if (a == "--realtime") realtime = true;
        else if (a == "--list") { printParams(); return 0; }
        else if (a == "--selftest") return selfTest();
        else if (a == "--set")
        {
            const auto kv = next();
            const auto key = kv.upToFirstOccurrenceOf ("=", false, false).toStdString();
            const int index = vcab::findParamByKey (key);
            if (index < 0)
            {
                std::cerr << "知らないパラメータ: " << key << "（--list で一覧）\n";
                return 1;
            }
            params.v[static_cast<size_t> (index)] = kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
        }
        else { printUsage(); return a == "--help" ? 0 : 1; }
    }
    if (inPath.isEmpty() || outPath.isEmpty())
    {
        printUsage();
        return 1;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (inPath)));
    if (reader == nullptr)
    {
        std::cerr << "読み込めません: " << inPath << "\n";
        return 1;
    }
    const double sr = reader->sampleRate;
    const int total = static_cast<int> (reader->lengthInSamples);
    juce::AudioBuffer<float> buf (static_cast<int> (reader->numChannels), total);
    reader->read (&buf, 0, total, 0, true, true);
    std::vector<float> data (buf.getReadPointer (0), buf.getReadPointer (0) + total);

    vcab::Engine engine;
    engine.prepare (sr, block);
    engine.setRealtime (realtime);

    if (loadLearnedPath.isNotEmpty())
    {
        juce::MemoryBlock mb;
        juce::File (loadLearnedPath).loadFileAsData (mb);
        std::vector<float> curve (mb.getSize() / sizeof (float));
        std::memcpy (curve.data(), mb.getData(), curve.size() * sizeof (float));
        engine.importLearned (std::move (curve));
    }

    const auto start = std::chrono::steady_clock::now();
    const int learnStart = learnFrom >= 0 ? static_cast<int> (learnFrom * sr) : -1;
    const int learnStop = learnTo >= 0 ? static_cast<int> (learnTo * sr) : -1;
    int nextReport = static_cast<int> (sr);
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min (block, total - pos);
        if (learnStart >= 0 && pos <= learnStart && learnStart < pos + n)
            engine.requestLearning (true);
        if (learnStop >= 0 && pos <= learnStop && learnStop < pos + n)
            engine.requestLearning (false);
        engine.process (data.data() + pos, n, params);
        if (realtime)
            std::this_thread::sleep_for (std::chrono::microseconds (static_cast<int> (n * 1.0e6 / sr)));
        if (printNotches && pos + n >= nextReport)
        {
            nextReport += static_cast<int> (sr);
            std::cout << "t=" << (pos + n) / sr << "s 声" << (engine.meters.voice.load() ? "あり" : "なし") << " 狭帯域ノイズ:";
            for (int i = 0; i < engine.meters.notchCount.load(); ++i)
                std::cout << " " << juce::roundToInt (engine.meters.notchHz[static_cast<size_t> (i)].load()) << "Hz/"
                          << juce::String (engine.meters.notchDepthDb[static_cast<size_t> (i)].load(), 1) << "dB";
            std::cout << "\n";
        }
    }
    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    std::cout << "処理: " << total / sr << " 秒の音を " << seconds << " 秒で（実時間の " << juce::String (100.0 * seconds / (total / sr), 2) << "%）\n";

    if (printNoiseCurve)
        for (int i = 0; i < vcab::EngineMeters::kNoiseCurvePoints; i += 8)
            std::cout << juce::roundToInt (vcab::EngineMeters::noiseCurveHz (i)) << "Hz " << juce::String (engine.meters.noiseCurveDb[static_cast<size_t> (i)].load(), 1) << "dB\n";

    if (saveLearnedPath.isNotEmpty())
    {
        engine.process (nullptr, 0, params); // 終わりの要求（学習の停止など）を受け取らせる
        const auto curve = engine.exportLearned();
        juce::File (saveLearnedPath).replaceWithData (curve.data(), curve.size() * sizeof (float));
        std::cout << "学習した値: " << (curve.empty() ? 0 : static_cast<int> (curve[1])) << " 点\n";
    }

    juce::AudioBuffer<float> out (1, total);
    out.copyFrom (0, 0, data.data(), total);
    juce::File outFile (outPath);
    outFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (outFile.createOutputStream());
    auto options = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (1).withBitsPerSample (32)
                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream, options));
    if (writer == nullptr)
    {
        std::cerr << "書き出せません: " << outPath << "\n";
        return 1;
    }
    writer->writeFromAudioSampleBuffer (out, 0, total);
    return 0;
}
