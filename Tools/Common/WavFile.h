// 開発用のツール（vcabdn_render、vcabdn_shot）で共通の、WAV の書き出し
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>

namespace tools
{

// モノラルの 32 ビット浮動小数点の WAV を書き出す（前のファイルは消す）。書けなければ false
inline bool writeMonoWav (const juce::File& file, const float* samples, int count, double sampleRate)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto options = juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (32)
                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream, options));
    if (writer == nullptr)
        return false;
    const float* channels[] = { samples };
    return writer->writeFromFloatArrays (channels, 1, count);
}

} // namespace tools
