// 画面（案 A）。ふだんは、上の段・2 行の操作部・大きなスペクトルだけを出す。
// 細かい設定は「詳細」ボタンで、スペクトルの上に重ねた枠に出す（閉じれば元の小さい画面に戻る）。
// 大きさは一般的な VST プラグインに合わせる（800 × 540。RX Voice De-noise を主な目安に、ユーザーと調整する）。
// 1 行分の操作（ParamRow）の作りは VoiChanAlgoBox から移したもの。
#pragma once

#include "LookAndFeel.h"
#include "MonitorView.h"
#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

// 1行分の操作（名前 + つまみ / スイッチ / 選択肢）
struct ParamRow
{
    vcab::P id;
    std::unique_ptr<juce::Label> label;
    std::unique_ptr<juce::Slider> slider;
    std::unique_ptr<juce::ToggleButton> toggle;
    std::unique_ptr<juce::ComboBox> combo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;

    juce::Component* control() const
    {
        if (slider) return slider.get();
        if (toggle) return toggle.get();
        return combo.get();
    }
    bool inlineToggle = false; // 名前をスイッチの横に書く行（名前の欄は使わない）
    juce::String baseTooltip;  // 選択欄: 全体の説明（選んでいる項目の説明は、その後ろに足す）
    int tooltipIndex = -1;     // 選択欄: ツールチップを、どの項目に合わせて作ったか
    void setVisible (bool v) const { label->setVisible (v && ! inlineToggle); control()->setVisible (v); }
    void setEnabled (bool e) const { label->setEnabled (e); control()->setEnabled (e); }
};

// 詳細の枠（スペクトルの上に重ねる）。枠の見出しと背景を描く
class DetailsPanel : public juce::Component
{
public:
    struct Box
    {
        juce::Rectangle<int> bounds;
        juce::String title;
    };
    std::vector<Box> boxes;
    void paint (juce::Graphics&) override;
};

class VcabDenoiseEditor : public juce::AudioProcessorEditor,
                          private juce::Timer
{
public:
    explicit VcabDenoiseEditor (VcabDenoiseProcessor&);
    ~VcabDenoiseEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    ParamRow& addRow (vcab::P id, juce::Component& parent);
    ParamRow& row (vcab::P id);
    // 詳細の枠の中に、見出しと項目を並べる。項目の下の余り（extraRows 行分）を返す
    juce::Rectangle<int> layoutDetailBox (juce::Rectangle<int> area, const char* title, const std::vector<vcab::P>& ids, int extraRows, int labelWidth);
    void layoutDetails();
    void setDetailsOpen (bool open);
    // 確認の窓を出し、okText の側が押されたら onOk を呼ぶ（その前に画面が閉じられていたら呼ばない）
    void confirmThen (const char* title, const char* message, const char* okText, std::function<void()> onOk);
    void updateEnabled();
    void updateStatus();
    void updateChoiceTooltips();
    void drawMeters (juce::Graphics&);

    VcabDenoiseProcessor& processor;
    VCLookAndFeel lookAndFeel;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow; // マウスを乗せて 0.4 秒で説明を出す

    std::vector<std::unique_ptr<ParamRow>> rows;
    juce::TextButton detailsButton, resetButton, audioSettingsButton, learnButton, clearLearnedButton;
    juce::Label learnStatus, reductionLabel, estimateLabel;
    int shownInputChannel = -1;
    bool shownLearning = false;
    juce::String shownStatus; // 学習の状態の文字（変わったときだけ書き換える）
    juce::Rectangle<int> headerArea, stripArea, meterArea;

    MonitorView monitor;
    DetailsPanel details;
};
