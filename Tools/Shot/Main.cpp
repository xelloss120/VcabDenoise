// vcabdn_shot: 開発用の画面の撮影ツール（README の画像を作るため）
//
// プラグインの画面（エディタ）を画面の外に作り、WAV をプラグインに流しながら、指定した時刻の画面を PNG に書き出す。
// 画面の更新（タイマー 30Hz）が実際の時間で動くので、音も実際の時間の速さで流す（10 秒の音なら約 10 秒かかる）。
//
// 使い方の例:
//   vcabdn_shot --in voice_noise.wav --out screenshot.png --at 6
//   vcabdn_shot --in voice_noise.wav --out details.png --at 6 --details --set max_reduction=18
#include "../../Source/Plugin/PluginEditor.h"
#include "../../Source/Plugin/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

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
        "  --in <file.wav>    流す音（2ch 以上なら 1ch 目を使う）\n"
        "  --out <file.png>   書き出す画像\n"
        "  --at <秒>          撮る時刻（初期値 6）\n"
        "  --details          詳細の枠を開いて撮る\n"
        "  --set <key>=<値>   パラメータを指定（複数可。vcabdn_render と同じ）\n";
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

} // namespace

int main (int argc, char* argv[])
{
#if JUCE_WINDOWS
    SetConsoleOutputCP (65001); // UTF-8
#endif
    juce::String inPath, outPath;
    double at = 6.0;
    bool openDetails = false;
    juce::StringArray sets;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a { juce::CharPointer_UTF8 (argv[i]) };
        auto next = [&]() -> juce::String { return i + 1 < argc ? juce::String (juce::CharPointer_UTF8 (argv[++i])) : juce::String(); };
        if (a == "--in") inPath = next();
        else if (a == "--out") outPath = next();
        else if (a == "--at") at = next().getDoubleValue();
        else if (a == "--details") openDetails = true;
        else if (a == "--set") sets.add (next());
        else { printUsage(); return a == "--help" ? 0 : 1; }
    }
    if (inPath.isEmpty() || outPath.isEmpty())
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
    const int total = juce::jmin (static_cast<int> (reader->lengthInSamples), static_cast<int> (at * sr));
    juce::AudioBuffer<float> source (static_cast<int> (reader->numChannels), total);
    reader->read (&source, 0, total, 0, true, true);

    constexpr int block = 128;
    VcabDenoiseProcessor processor;
    processor.setPlayConfigDetails (2, 2, sr, block);
    processor.setNonRealtime (true); // 声の判定をその場で計算する（結果が毎回同じ）
    for (const auto& kv : sets)
    {
        auto* param = processor.state().getParameter (kv.upToFirstOccurrenceOf ("=", false, false));
        if (param == nullptr)
        {
            std::cerr << "知らないパラメータ: " << kv << "\n";
            return 1;
        }
        param->setValueNotifyingHost (param->convertTo0to1 (kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
    }
    processor.prepareToPlay (sr, block);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    editor->setTopLeftPosition (-10000, -10000); // 画面の外（画面の更新は、表示されているときだけ動くので、デスクトップには置く）
    editor->addToDesktop (juce::ComponentPeer::windowIsTemporary);
    editor->setVisible (true);
    if (openDetails)
        if (auto* b = findButton (*editor, juce::String (juce::CharPointer_UTF8 ("詳細"))))
            b->triggerClick();

    // 画面のタイマー（30Hz）1 回分の音を流してから、メッセージを処理する
    auto* mm = juce::MessageManager::getInstance();
    const int perTick = static_cast<int> (sr / 30.0);
    juce::AudioBuffer<float> buffer (2, block);
    juce::MidiBuffer midi;
    int sinceTick = 0;
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = juce::jmin (block, total - pos);
        for (int ch = 0; ch < 2; ++ch)
            buffer.copyFrom (ch, 0, source, 0, pos, n);
        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, n);
        processor.processBlock (view, midi);
        if ((sinceTick += n) >= perTick)
        {
            sinceTick -= perTick;
            mm->runDispatchLoopUntil (33);
        }
    }
    mm->runDispatchLoopUntil (100);

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    juce::File outFile (outPath);
    outFile.deleteFile();
    juce::FileOutputStream stream (outFile);
    juce::PNGImageFormat png;
    if (! stream.openedOk() || ! png.writeImageToStream (image, stream))
    {
        std::cerr << "書き出せません: " << outPath << "\n";
        return 1;
    }
    editor->removeFromDesktop();
    editor.reset();
    std::cout << "書き出した: " << outPath << "（" << image.getWidth() << " × " << image.getHeight() << "）\n";
    return 0;
}
