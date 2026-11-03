// vcabdn_shot: 開発用の画面の撮影ツール（README の画像や、紹介動画の映像を作るため）
//
// プラグインの画面（エディタ）を画面の外に作り、WAV をプラグインに流しながら、画面を PNG に書き出す。
// 画面の更新（タイマー 30Hz）が実際の時間で動くので、音も実際の時間の速さで流す（10 秒の音なら 10 秒以上かかる）。
//   1 枚だけ: 指定した時刻（--at）の画面を --out に書き出す
//   動画用: 画面のタイマー 1 回分（1/30 秒）の音を流すごとに、画面を --frames のフォルダに連番で書き出す（30 枚/秒）。
//           プラグインの出力の音は --audio-out に書き出す（画面の連番と時刻がそろう）
//
// 使い方の例:
//   vcabdn_shot --in voice_noise.wav --out screenshot.png --at 6
//   vcabdn_shot --in voice_noise.wav --out details.png --at 6 --details --set max_reduction=18
//   vcabdn_shot --in demo.wav --frames frames --audio-out demo_out.wav --scale 2 --event 11:bypass=0 --event 30.3:noise_only=1 --set bypass=1
#include "../../Source/Plugin/PluginEditor.h"
#include "../../Source/Plugin/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <iostream>

#if JUCE_WINDOWS
extern "C" __declspec (dllimport) int __stdcall SetConsoleOutputCP (unsigned int);
#endif

namespace
{

void printUsage()
{
    std::cout <<
        "vcabdn_shot - VcabDenoise の開発用の画面の撮影ツール\n"
        "\n"
        "  --in <file.wav>            流す音（2ch 以上なら 1ch 目を使う）\n"
        "  --out <file.png>           1 枚だけ書き出す画像\n"
        "  --at <秒>                  --out で撮る時刻（初期値 6）\n"
        "  --frames <フォルダ>        1/30 秒ごとの画面を連番（00000.png …）で書き出す（音の終わりまで）\n"
        "  --audio-out <file.wav>     プラグインの出力の音を書き出す（--frames と一緒に使う）\n"
        "  --scale <倍率>             画像の倍率（初期値 1）\n"
        "  --details                  詳細の枠を開いて撮る\n"
        "  --set <key>=<値>           パラメータを指定（複数可。vcabdn_render と同じ）\n"
        "  --event <秒>:<key>=<値>    その時刻にパラメータを変える（複数可。画面のスイッチも動く）\n";
}

// 名前が text のボタンを、子の中から探す
juce::Button* findButton (juce::Component& parent, const juce::String& text)
{
    for (auto* c : parent.getChildren())
    {
        if (auto* b = dynamic_cast<juce::Button*> (c); b != nullptr && b->getButtonText() == text)
            return b;
        if (auto* found = findButton (*c, text))
            return found;
    }
    return nullptr;
}

// "key=値" をパラメータに入れる（知らない名前なら false）
bool applySetting (VcabDenoiseProcessor& processor, const juce::String& kv)
{
    auto* param = processor.state().getParameter (kv.upToFirstOccurrenceOf ("=", false, false));
    if (param == nullptr)
    {
        std::cerr << "知らないパラメータ: " << kv << "\n";
        return false;
    }
    param->setValueNotifyingHost (param->convertTo0to1 (kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
    return true;
}

bool writePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    juce::PNGImageFormat png;
    return stream.openedOk() && png.writeImageToStream (image, stream);
}

struct Event
{
    int sample;
    juce::String setting;
};

} // namespace

int main (int argc, char* argv[])
{
#if JUCE_WINDOWS
    SetConsoleOutputCP (65001); // UTF-8
#endif
    juce::String inPath, outPath, framesPath, audioOutPath;
    double at = 6.0;
    float scale = 1.0f;
    bool openDetails = false;
    juce::StringArray sets, eventArgs;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a { juce::CharPointer_UTF8 (argv[i]) };
        auto next = [&]() -> juce::String { return i + 1 < argc ? juce::String (juce::CharPointer_UTF8 (argv[++i])) : juce::String(); };
        if (a == "--in") inPath = next();
        else if (a == "--out") outPath = next();
        else if (a == "--at") at = next().getDoubleValue();
        else if (a == "--frames") framesPath = next();
        else if (a == "--audio-out") audioOutPath = next();
        else if (a == "--scale") scale = juce::jmax (0.25f, next().getFloatValue());
        else if (a == "--details") openDetails = true;
        else if (a == "--set") sets.add (next());
        else if (a == "--event") eventArgs.add (next());
        else { printUsage(); return a == "--help" ? 0 : 1; }
    }
    const bool frameMode = framesPath.isNotEmpty();
    if (inPath.isEmpty() || (outPath.isEmpty() && ! frameMode))
    {
        printUsage();
        return 1;
    }

    juce::ScopedJuceInitialiser_GUI gui;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (inPath)));
    if (reader == nullptr)
    {
        std::cerr << "読み込めません: " << inPath << "\n";
        return 1;
    }
    const double sr = reader->sampleRate;
    const int length = static_cast<int> (reader->lengthInSamples);
    const int total = frameMode ? length : juce::jmin (length, static_cast<int> (at * sr));
    juce::AudioBuffer<float> source (static_cast<int> (reader->numChannels), total);
    reader->read (&source, 0, total, 0, true, true);

    std::vector<Event> events;
    for (const auto& e : eventArgs)
        events.push_back ({ static_cast<int> (e.upToFirstOccurrenceOf (":", false, false).getDoubleValue() * sr),
                            e.fromFirstOccurrenceOf (":", false, false) });
    std::sort (events.begin(), events.end(), [] (const Event& a, const Event& b) { return a.sample < b.sample; });

    constexpr int block = 128;
    VcabDenoiseProcessor processor;
    processor.setPlayConfigDetails (2, 2, sr, block);
    processor.setNonRealtime (true); // 声の判定をその場で計算する（結果が毎回同じ）
    for (const auto& kv : sets)
        if (! applySetting (processor, kv))
            return 1;
    processor.prepareToPlay (sr, block);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setTopLeftPosition (-10000, -10000); // 画面の外（画面の更新は、表示されているときだけ動くので、デスクトップには置く）
    editor->addToDesktop (juce::ComponentPeer::windowIsTemporary);
    editor->setVisible (true);
    if (openDetails)
        if (auto* b = findButton (*editor, juce::String (juce::CharPointer_UTF8 ("詳細"))))
            b->triggerClick();

    juce::File framesDir (framesPath);
    if (frameMode)
        framesDir.createDirectory();
    auto snapshot = [&] { return editor->createComponentSnapshot (editor->getLocalBounds(), true, scale); };

    // 画面のタイマー（30Hz）1 回分の音を流してから、メッセージを処理する（動画用は、そのあと画面を書き出す）
    auto* mm = juce::MessageManager::getInstance();
    const int perTick = static_cast<int> (sr / 30.0);
    juce::AudioBuffer<float> buffer (2, block);
    juce::AudioBuffer<float> output (1, frameMode ? total : 0);
    juce::MidiBuffer midi;
    size_t nextEvent = 0;
    int sinceTick = 0, frameIndex = 0;
    for (int pos = 0; pos < total; pos += block)
    {
        for (; nextEvent < events.size() && events[nextEvent].sample <= pos; ++nextEvent)
            if (! applySetting (processor, events[nextEvent].setting))
                return 1;
        const int n = juce::jmin (block, total - pos);
        for (int ch = 0; ch < 2; ++ch)
            buffer.copyFrom (ch, 0, source, 0, pos, n);
        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, n);
        processor.processBlock (view, midi);
        if (frameMode)
            output.copyFrom (0, pos, view, 0, 0, n);
        if ((sinceTick += n) >= perTick)
        {
            sinceTick -= perTick;
            mm->runDispatchLoopUntil (33);
            if (frameMode && ! writePng (snapshot(), framesDir.getChildFile (juce::String (frameIndex++).paddedLeft ('0', 5) + ".png")))
            {
                std::cerr << "書き出せません: " << framesPath << "\n";
                return 1;
            }
        }
    }
    mm->runDispatchLoopUntil (100);

    if (frameMode)
    {
        if (audioOutPath.isNotEmpty())
        {
            juce::File outFile (audioOutPath);
            outFile.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream (outFile.createOutputStream());
            auto options = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (1).withBitsPerSample (32)
                               .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
            std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream, options));
            if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (output, 0, total))
            {
                std::cerr << "書き出せません: " << audioOutPath << "\n";
                return 1;
            }
        }
        std::cout << "書き出した: " << framesPath << "（" << frameIndex << " 枚）\n";
    }
    else
    {
        const auto image = snapshot();
        if (! writePng (image, juce::File (outPath)))
        {
            std::cerr << "書き出せません: " << outPath << "\n";
            return 1;
        }
        std::cout << "書き出した: " << outPath << "（" << image.getWidth() << " × " << image.getHeight() << "）\n";
    }
    editor->removeFromDesktop();
    editor.reset();
    return 0;
}
