#include "PluginEditor.h"

#include "EditorStyle.h"

using namespace editor;

namespace
{

// 画面の大きさ（一般的な VST プラグインに合わせる。RX Voice De-noise を主な目安に、ユーザーと調整する）
constexpr int kWindowWidth = 800, kWindowHeight = 540; // 高さは、詳細の狭帯域ノイズの枠（14 行）が収まるように
constexpr int kHeader = 40;
constexpr int kStripRow = 30;
constexpr int kPanelRow = 27; // 詳細の枠の 1 行

// つまみに刻みを付ける（パラメータ自体は刻みなしでホストに伝えているため、画面の側でだけ付ける）
void withStep (juce::Slider& slider, vcab::P id)
{
    const auto& s = vcab::spec (id);
    if (s.step <= 0)
        return;
    auto range = slider.getNormalisableRange();
    range.interval = s.step;
    slider.setNormalisableRange (range);
}

using vcab::P;

const std::vector<vcab::P> kReductionDetails { P::strength, P::attackMs, P::releaseMs, P::onsetProtect, P::cepstrumOn, P::presenceOn };
const std::vector<vcab::P> kNoiseDetails { P::rnnThreshold, P::saveLearned, P::showNoiseEstimate };
const std::vector<vcab::P> kTonalDetails { P::tonalSensitivity, P::tonalCount, P::tonalMaxDepth, P::tonalLowHz, P::tonalHighHz, P::tonalNeighborHz,
                                           P::tonalActivateSec, P::tonalReleaseSec, P::tonalJumpSec, P::tonalJumpRange, P::tonalWidth, P::tonalLowWidth, P::tonalQuietDb };

// 学習ボタンの説明（後ろに今の状態を足して出す）
constexpr const char* kLearnTip = "押してから、もう一度押すまでのノイズを覚えます。その間は静かにしていて下さい（押してから約 1 秒は覚えずに待ちます）。"
                                  "ノイズ推定が「学習固定」のときに、覚えた値で抑制します。";

// ラベルの見た目（文字の大きさと色）をそろえる
void styleLabel (juce::Label& l)
{
    l.setFont (juce::FontOptions (kFont));
    l.setColour (juce::Label::textColourId, kText);
    l.setMinimumHorizontalScale (1.0f);
}

bool inDetails (vcab::P id)
{
    for (const auto* list : { &kReductionDetails, &kNoiseDetails, &kTonalDetails })
        for (auto x : *list)
            if (x == id)
                return true;
    return false;
}

} // namespace

void DetailsPanel::paint (juce::Graphics& g)
{
    g.fillAll (kPanel.withAlpha (0.97f));
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRect (getLocalBounds(), 1);
    for (const auto& b : boxes)
    {
        g.setColour (kBox);
        g.fillRoundedRectangle (b.bounds.toFloat(), 5.0f);
        g.setColour (kText);
        g.setFont (juce::FontOptions (kFont, juce::Font::bold));
        g.drawText (b.title, b.bounds.reduced (8, 3).removeFromTop (kPanelRow), juce::Justification::centredLeft);
    }
}

VcabDenoiseEditor::VcabDenoiseEditor (VcabDenoiseProcessor& p)
    : AudioProcessorEditor (&p), processor (p), monitor (p)
{
    setLookAndFeel (&lookAndFeel);
    tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 400);

    addAndMakeVisible (monitor);
    addChildComponent (details);

    for (const auto& s : vcab::paramSpecs())
        addRow (s.id, inDetails (s.id) ? static_cast<juce::Component&> (details) : static_cast<juce::Component&> (*this));

    auto setupButton = [this] (juce::TextButton& b, const char* text, const char* tip, juce::Component& parent)
    {
        b.setButtonText (utf8 (text));
        b.setTooltip (utf8 (tip));
        parent.addAndMakeVisible (b);
    };

    setupButton (detailsButton, "詳細", "強度や時間、ノイズ推定、狭帯域ノイズの細かい設定を、スペクトルの上に開きます。もう一度押すと閉じます。", *this);
    detailsButton.setClickingTogglesState (true);
    detailsButton.onClick = [this] { setDetailsOpen (detailsButton.getToggleState()); };

    setupButton (resetButton, "設定値初期化", "すべての設定を初期値に戻します（入力チャンネルと、学習したノイズは戻しません）。押すと確認が出ます。", *this);
    resetButton.onClick = [this]
    {
        confirmThen ("設定値初期化", "すべての設定を初期値に戻します。よろしいですか？\n（入力チャンネル、学習したノイズ、オーディオデバイスの設定は変わりません）",
                     "初期化", [this] { processor.resetAllParameters(); });
    };

    // EXE 版だけ、オーディオの設定画面を開くボタン（「オプション」の中と同じ画面）
    audioSettingsButton.setButtonText (utf8 ("オーディオ設定"));
    audioSettingsButton.setTooltip (utf8 ("オーディオの設定画面（入出力の機器、サンプルレート、バッファの大きさ）を開きます。左上の「オプション」の中と同じ画面です。"));
    audioSettingsButton.onClick = [this]
    {
        if (processor.openAudioSettings)
            processor.openAudioSettings();
    };
    if (processor.openAudioSettings)
        addAndMakeVisible (audioSettingsButton);

    setupButton (learnButton, "学習開始", kLearnTip, *this);
    learnButton.onClick = [this] { processor.requestLearning (! processor.meters().learning.load()); };
    setupButton (clearLearnedButton, "学習ノイズ削除", "覚えたノイズを消します。消した後、ノイズ推定が「学習固定」なら、学習し直すまで自動追従で抑制します。押すと確認が出ます。", details);
    clearLearnedButton.onClick = [this]
    {
        confirmThen ("学習ノイズ削除", "覚えたノイズを消します。よろしいですか？", "削除", [this] { processor.requestClearLearned(); });
    };

    auto setupLabel = [] (juce::Label& l, juce::Component& parent, const char* text, const char* tip)
    {
        styleLabel (l);
        if (text != nullptr)
            l.setText (utf8 (text), juce::dontSendNotification);
        l.setTooltip (utf8 (tip));
        parent.addAndMakeVisible (l);
    };
    setupLabel (learnStatus, details, nullptr, "学習の状態。");
    setupLabel (reductionLabel, *this, "抑制量上限", vcab::paramDescription (P::maxReduction));
    setupLabel (estimateLabel, *this, "ノイズ推定", vcab::paramDescription (P::noiseMode));

    // 上の段と操作部の行は、いつも出す（ふだんの画面に出す主な項目）
    for (auto id : { P::inputChannel, P::bypass, P::noiseOnly, P::maxReduction, P::noiseMode, P::tonalOn })
        row (id).setVisible (true);
    row (P::maxReduction).label->setVisible (false); // 名前は reductionLabel で出す
    row (P::noiseMode).label->setVisible (false);    // 名前は estimateLabel で出す
    // 詳細の枠の項目は、枠と一緒に出し入れする（枠の子なので、ここではいつも見せておく）
    for (auto& r : rows)
        if (inDetails (r->id))
            r->setVisible (true);

    setSize (kWindowWidth, kWindowHeight);
    startTimerHz (30);
}

VcabDenoiseEditor::~VcabDenoiseEditor()
{
    setLookAndFeel (nullptr);
}

ParamRow& VcabDenoiseEditor::addRow (vcab::P id, juce::Component& parent)
{
    const auto& s = vcab::spec (id);
    auto r = std::make_unique<ParamRow>();
    r->id = id;
    const auto text = utf8 (s.name);
    const auto description = utf8 (vcab::paramDescription (id));

    r->label = std::make_unique<juce::Label> (juce::String(), text);
    r->label->setTooltip (description);
    styleLabel (*r->label);
    parent.addChildComponent (*r->label);

    auto& apvts = processor.state();
    switch (s.type)
    {
        case vcab::ParamType::floating:
            // 狭い所に置くので、スライダーと値を 1 本のバーにまとめる（バー全体をドラッグして値を変え、値はバーの中に出す）
            r->slider = std::make_unique<juce::Slider> (juce::Slider::LinearBar, juce::Slider::TextBoxLeft);
            r->slider->setTooltip (description);
            parent.addChildComponent (*r->slider);
            r->sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, s.key, *r->slider);
            withStep (*r->slider, id);
            break;
        case vcab::ParamType::toggle:
            r->inlineToggle = true; // 名前はスイッチの横に書く
            r->toggle = std::make_unique<juce::ToggleButton> (text);
            r->toggle->setTooltip (description);
            parent.addChildComponent (*r->toggle);
            r->buttonAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, s.key, *r->toggle);
            break;
        case vcab::ParamType::choice:
            r->combo = std::make_unique<juce::ComboBox>();
            for (int index = 0; index < static_cast<int> (s.choices.size()); ++index)
                r->combo->addItem (utf8 (s.choices[static_cast<size_t> (index)]), index + 1);
            r->comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, s.key, *r->combo);
            r->combo->setTooltip (description);
            r->baseTooltip = description;
            parent.addChildComponent (*r->combo);
            break;
    }
    rows.push_back (std::move (r));
    return *rows.back();
}

ParamRow& VcabDenoiseEditor::row (vcab::P id)
{
    for (auto& r : rows)
        if (r->id == id)
            return *r;
    jassertfalse;
    return *rows.front();
}

void VcabDenoiseEditor::resized()
{
    auto r = getLocalBounds().reduced (kMargin, 0);

    // 上の段: 名前、入力チャンネル、（右から）詳細、設定値初期化、オーディオ設定（ボタンはどれも同じ幅）
    headerArea = r.removeFromTop (kHeader);
    {
        auto h = headerArea.reduced (0, 5);
        h.removeFromLeft (130); // 名前（paint で描く）
        auto& input = row (P::inputChannel);
        input.label->setBounds (h.removeFromLeft (96));
        constexpr int buttonWidth = 120;
        detailsButton.setBounds (h.removeFromRight (buttonWidth));
        h.removeFromRight (6);
        resetButton.setBounds (h.removeFromRight (buttonWidth));
        h.removeFromRight (6);
        if (audioSettingsButton.isVisible())
        {
            audioSettingsButton.setBounds (h.removeFromRight (buttonWidth));
            h.removeFromRight (6);
        }
        // 入力チャンネルの選択欄は、ボタンまでの残り（広すぎないように 200 まで）
        h.removeFromRight (4);
        input.combo->setBounds (h.removeFromLeft (juce::jmin (200, h.getWidth())));
    }

    // 操作部（2 行）: メーター（縦に 3 本） ｜ 抑制量上限・ノイズ推定 ｜ バイパス・抑制ノイズ試聴・狭帯域抑制
    // メーターとチェックの列は高さを全部使う（3 行）。真ん中の列は 2 行を上下の真ん中に置く
    stripArea = r.removeFromTop (kStripRow * 2 + 14);
    {
        auto s = stripArea.reduced (6, 4);
        // 右の端にチェックの列を中身の幅で置き、残りをメーターと真ん中の列で分ける
        auto checks = s.removeFromRight (116);
        s.removeFromRight (10);
        meterArea = s.removeFromLeft ((s.getWidth() - 10) * 2 / 5);
        s.removeFromLeft (10);
        auto col2 = s.withSizeKeepingCentre (s.getWidth(), kStripRow * 2);

        const int checkRow = checks.getHeight() / 3;
        row (P::bypass).toggle->setBounds (checks.removeFromTop (checkRow));
        row (P::noiseOnly).toggle->setBounds (checks.removeFromTop (checkRow));
        row (P::tonalOn).toggle->setBounds (checks);
        auto line = col2.removeFromTop (kStripRow);
        reductionLabel.setBounds (line.removeFromLeft (100));
        row (P::maxReduction).slider->setBounds (line.reduced (0, 3));
        // ノイズ推定の選択欄と学習のボタンは、上の抑制量上限のバーを半分ずつに分けた幅にそろえる
        line = col2.removeFromTop (kStripRow);
        estimateLabel.setBounds (line.removeFromLeft (100));
        const int half = (line.getWidth() - 6) / 2;
        row (P::noiseMode).combo->setBounds (line.removeFromLeft (half).reduced (0, 2));
        learnButton.setBounds (line.removeFromRight (half).reduced (0, 2));
    }

    // スペクトル（残り全部）。詳細の枠は、同じ場所に重ねる
    r.removeFromTop (6);
    auto spectrum = r.removeFromTop (r.getHeight() - kMargin);
    monitor.setBounds (spectrum);
    details.setBounds (spectrum);
    layoutDetails();
    repaint();
}

juce::Rectangle<int> VcabDenoiseEditor::layoutDetailBox (juce::Rectangle<int> area, const char* title, const std::vector<vcab::P>& ids, int extraRows, int labelWidth)
{
    const int height = kPanelRow + static_cast<int> (ids.size() + static_cast<size_t> (extraRows)) * kPanelRow + 10;
    auto box = area.removeFromTop (height);
    details.boxes.push_back ({ box, utf8 (title) });
    auto inner = box.reduced (8, 5);
    inner.removeFromTop (kPanelRow);
    for (auto id : ids)
    {
        auto& r = row (id);
        auto line = inner.removeFromTop (kPanelRow);
        if (r.inlineToggle)
        {
            r.control()->setBounds (line.reduced (0, 1));
            continue;
        }
        r.label->setBounds (line.removeFromLeft (labelWidth));
        r.control()->setBounds (line.reduced (0, 2));
    }
    return inner;
}

void VcabDenoiseEditor::layoutDetails()
{
    details.boxes.clear();
    auto area = details.getLocalBounds().reduced (6);
    auto col1 = area.removeFromLeft (232);
    area.removeFromLeft (6);
    auto col2 = area.removeFromLeft (232);
    area.removeFromLeft (6);
    auto col3 = area;

    layoutDetailBox (col1, "抑制", kReductionDetails, 0, 82);
    {
        auto extra = layoutDetailBox (col2, "ノイズ推定", kNoiseDetails, 2, 122);
        clearLearnedButton.setBounds (extra.removeFromTop (kPanelRow).reduced (0, 2).withWidth (110));
        learnStatus.setBounds (extra.removeFromTop (kPanelRow));
    }
    layoutDetailBox (col3, "狭帯域", kTonalDetails, 0, 150);
    details.repaint();
}

void VcabDenoiseEditor::confirmThen (const char* title, const char* message, const char* okText, std::function<void()> onOk)
{
    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::QuestionIcon)
                       .withTitle (utf8 (title))
                       .withMessage (utf8 (message))
                       .withButton (utf8 (okText))
                       .withButton (utf8 ("やめる"))
                       .withAssociatedComponent (this);
    juce::Component::SafePointer<VcabDenoiseEditor> safeThis (this);
    juce::AlertWindow::showAsync (options, [safeThis, onOk = std::move (onOk)] (int result)
    {
        if (result == 1 && safeThis != nullptr)
            onOk();
    });
}

void VcabDenoiseEditor::setDetailsOpen (bool open)
{
    detailsButton.setToggleState (open, juce::dontSendNotification);
    details.setVisible (open);
    if (open)
        details.toFront (false);
}

void VcabDenoiseEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
    g.setColour (kText);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("VcabDenoise", headerArea.withWidth (130), juce::Justification::centredLeft);

    g.setColour (kPanel);
    g.fillRoundedRectangle (stripArea.toFloat(), 5.0f);
    drawMeters (g);
}

void VcabDenoiseEditor::drawMeters (juce::Graphics& g)
{
    auto& m = processor.meters();
    g.setFont (juce::FontOptions (kFont));
    // 入力・出力（-80〜0 dBFS）と、抑制している量（0〜30dB）の小さなバー（縦に 3 本）
    auto area = meterArea;
    const int rowHeight = area.getHeight() / 3;
    auto bar = [&] (const char* name, float value, float lo, float hi, juce::Colour c)
    {
        auto part = area.removeFromTop (rowHeight);
        g.setColour (kText);
        g.drawText (utf8 (name), part.removeFromLeft (20), juce::Justification::centredLeft);
        auto track = part.withSizeKeepingCentre (part.getWidth(), 6);
        g.setColour (kGraph);
        g.fillRect (track);
        const float t = juce::jlimit (0.0f, 1.0f, (value - lo) / (hi - lo));
        g.setColour (c);
        g.fillRect (track.toFloat().withWidth (static_cast<float> (track.getWidth()) * t));
    };
    bar ("入", m.inputDb.load(), -80.0f, 0.0f, kInputColour);
    bar ("出", m.outputDb.load(), -80.0f, 0.0f, kOutputColour);
    bar ("抑", m.reductionDb.load(), 0.0f, 30.0f, kNoiseColour);
}

void VcabDenoiseEditor::updateEnabled()
{
    const auto p = processor.currentParams();
    for (auto& r : rows)
        r->setEnabled (vcab::paramEnabled (r->id, p));
}

void VcabDenoiseEditor::updateStatus()
{
    auto& m = processor.meters();
    const auto p = processor.currentParams();
    const bool learning = m.learning.load();
    if (learning != shownLearning)
    {
        shownLearning = learning;
        learnButton.setButtonText (utf8 (learning ? "学習終了" : "学習開始"));
        learnButton.setColour (juce::TextButton::buttonColourId, learning ? juce::Colour (0xff803030) : getLookAndFeel().findColour (juce::TextButton::buttonColourId));
    }
    const bool learnedMode = static_cast<vcab::NoiseMode> (p.index (P::noiseMode)) == vcab::NoiseMode::learned;
    juce::String text;
    if (learning)
        text = utf8 ("学習中");
    else if (m.learned.load())
        text = utf8 (learnedMode ? "学習ノイズで抑制中" : "学習済み");
    else
        text = utf8 (learnedMode ? "未学習（自動追従で抑制）" : "未学習");
    if (text != shownStatus)
    {
        shownStatus = text;
        learnStatus.setText (text, juce::dontSendNotification);
        learnButton.setTooltip (utf8 (kLearnTip) + utf8 ("今の状態: ") + text);
    }

    // 入力チャンネルが自動のときは、今どちらを使っているかを、選択欄の「自動」の項目の文字に出す（VoiChanAlgoBox と同じ）
    {
        const int used = processor.usedInputChannel.load() + 1;
        const auto autoText = utf8 ("自動（") + juce::String (used) + utf8 ("ch を使用中）");
        auto* combo = row (P::inputChannel).combo.get();
        if (used != shownInputChannel)
        {
            shownInputChannel = used;
            combo->changeItemText (1, autoText);
        }
        // 選択欄は、表示の文字が項目の文字と一致しないと「何も選んでいない」とみなすので、設定値で判断して選び直す
        if (p.index (P::inputChannel) == 0 && combo->getText() != autoText)
            combo->setSelectedId (1, juce::dontSendNotification);
    }
}

void VcabDenoiseEditor::updateChoiceTooltips()
{
    for (auto& r : rows)
    {
        if (r->combo == nullptr)
            continue;
        const int index = r->combo->getSelectedId() - 1;
        if (index == r->tooltipIndex)
            continue;
        r->tooltipIndex = index;
        auto text = r->baseTooltip;
        const auto& choices = vcab::spec (r->id).choices;
        if (const char* d = vcab::choiceDescription (r->id, index); d != nullptr && index < static_cast<int> (choices.size()))
            text << "\n" << utf8 ("「") << utf8 (choices[static_cast<size_t> (index)]) << utf8 ("」") << utf8 (d);
        r->combo->setTooltip (text);
        r->label->setTooltip (text);
        if (r->id == P::noiseMode)
            estimateLabel.setTooltip (text);
    }
}

void VcabDenoiseEditor::timerCallback()
{
    updateEnabled();
    updateStatus();
    updateChoiceTooltips();
    monitor.update();
    repaint (meterArea);
}
