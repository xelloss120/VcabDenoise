#include "Params.h"

#include <cassert>

namespace vcab
{

namespace
{
std::vector<ParamSpec> makeSpecs()
{
    using T = ParamType;

    std::vector<ParamSpec> s {
        // id                     key                    name                          type        min      max      default  step   skew    unit

        // 全体
        { P::bypass,              "bypass",              "バイパス",                   T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::noiseOnly,           "noise_only",          "抑制ノイズ試聴",             T::toggle,   0,       1,       0,       1,     0,      "",   {} },

        // 広い帯域の抑制
        { P::maxReduction,        "max_reduction",       "抑制量上限",                 T::floating, 0,       30,      20,      0.5f,  0,      "dB", {} },
        { P::strength,            "strength",            "強度",                       T::floating, 0,       12,      6,       0.5f,  0,      "dB", {} },
        { P::attackMs,            "attack",              "アタック",                   T::floating, 0.5f,    50,      10,      0.1f,  5,      "ms", {} },
        { P::releaseMs,           "release",             "リリース",                   T::floating, 10,      1000,    100,     1,     150,    "ms", {} },
        { P::onsetProtect,        "onset_protect",       "立ち上がり保護",             T::toggle,   0,       1,       1,       1,     0,      "",   {} },

        // ノイズ推定
        { P::noiseMode,           "noise_mode",          "ノイズ推定",                 T::choice,   0,       1,       0,       1,     0,      "",   { "自動追従", "学習固定" } },
        { P::rnnThreshold,        "rnn_threshold",       "声判定しきい値",             T::floating, 10,      90,      50,      1,     0,      "%",  {} },

        // 狭帯域ノイズ
        { P::tonalOn,             "tonal_on",            "狭帯域抑制",                 T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::tonalSensitivity,    "tonal_sensitivity",   "感度",                       T::floating, 3,       30,      12,      0.5f,  0,      "dB", {} },
        { P::tonalCount,          "tonal_count",         "本数上限",                   T::floating, 1,       8,       4,       1,     0,      "本", {} },
        { P::tonalMaxDepth,       "tonal_max_depth",     "深さ上限",                   T::floating, 0,       40,      24,      0.5f,  0,      "dB", {} },
        { P::tonalActivateSec,    "tonal_activate",      "持続判定時間",               T::floating, 0.1f,    5,       1,       0.05f, 1,      "秒", {} },

        // 画面と入力
        { P::inputChannel,        "input_channel",       "入力チャンネル",             T::choice,   0,       3,       0,       1,     0,      "",   { "自動", "1ch", "2ch", "平均" } },
        { P::showNoiseEstimate,   "show_noise_estimate", "ノイズ推定表示",             T::toggle,   0,       1,       1,       1,     0,      "",   {} },
    };

    assert (s.size() == static_cast<size_t> (kNumParams));
    for (size_t i = 0; i < s.size(); ++i)
        assert (static_cast<size_t> (s[i].id) == i); // 表の順番は P と同じ
    return s;
}
} // namespace

const std::vector<ParamSpec>& paramSpecs()
{
    static const std::vector<ParamSpec> specs = makeSpecs();
    return specs;
}

const ParamSpec& spec (P id)
{
    return paramSpecs()[static_cast<size_t> (id)];
}

int findParamByKey (const std::string& key)
{
    for (const auto& s : paramSpecs())
        if (key == s.key)
            return static_cast<int> (s.id);
    return -1;
}

const char* paramDescription (P id)
{
    switch (id)
    {
        case P::bypass:              return "処理をせず、入力をそのまま出します（処理なしの音と比べるため）。中の分析は続けるので、戻してすぐに効きます。";
        case P::noiseOnly:           return "抑制した音だけを出します。声が混ざって聞こえたら、抑制しすぎです（抑制量上限や強度を下げます）。";

        case P::maxReduction:        return "ノイズを下げる量の上限。完全に消そうとすると、声が水っぽく、ロボットのようになるので、ノイズを少し残します。";
        case P::strength:            return "ノイズ推定を、何 dB 大きく見るか。大きくするとノイズがよく消えますが、小さな声や語尾も抑制されやすくなります。";
        case P::attackMs:            return "声が出て、下げるのをやめるまでの時間。短いほど語頭が欠けにくくなります。";
        case P::releaseMs:           return "声が止まって、下げきるまでの時間。長いほど語尾が自然に消えますが、ノイズが残る時間も長くなります。";
        case P::onsetProtect:        return "急に大きくなった帯域をすぐ開けて、話し始めの子音（カ行、タ行など）が抑制されるのを防ぎます。";

        case P::noiseMode:           return "ノイズの大きさを推定する方法。";
        case P::rnnThreshold:        return "AI（RNNoise）が出す声の確率が、これを超えたら声とみなします。ささやき声を使うなら下げます。";

        case P::tonalOn:             return "「ふぃーん」「キーン」のような狭帯域の持続音を見つけて、その周波数だけを狭く抑制します。周波数が動いても追いかけます。";
        case P::tonalSensitivity:    return "周りより何 dB 突き出たら、狭帯域ノイズとみなすか。小さくすると弱い音も拾いますが、声の倍音を誤って拾いやすくなります。";
        case P::tonalCount:          return "同時に抑制する狭帯域ノイズの数（突き出た量の大きい順）。";
        case P::tonalMaxDepth:       return "1 本の谷で抑制する量の上限。抑制する量は、周りより突き出た分だけ（周りの高さにそろえる）です。";
        case P::tonalActivateSec:    return "これだけ居続けたら抑制し始めます。短くすると、伸ばした母音や歌を誤って抑制しやすくなります。";

        case P::inputChannel:        return "マイクの入力チャンネル。自動は、1ch と 2ch の大きいほうを使います（もう片方が数秒続けて 6dB 以上大きいと切り替え）。";
        case P::showNoiseEstimate:   return "モニターに、ノイズ推定（学習固定の値、または自動追従で追いかけている値）の線を重ねます。";

        case P::count:               break;
    }
    return "";
}

const char* choiceDescription (P id, int index)
{
    auto pick = [index] (std::initializer_list<const char*> list) -> const char*
    {
        if (index < 0 || index >= static_cast<int> (list.size()))
            return nullptr;
        return list.begin()[index];
    };
    switch (id)
    {
        case P::noiseMode: return pick ({ "AI（RNNoise）が声なしと判定した間だけ、ノイズを追いかけます。声を抑制しにくい。",
                                          "学習ボタンを押してから、もう一度押すまでのノイズを覚え、その値で抑制します。ノイズが変わる環境では自動追従を使います。" });
        default: return nullptr;
    }
}

bool isUiSetting (P id)
{
    switch (id)
    {
        case P::inputChannel: case P::showNoiseEstimate:
            return true;
        default:
            return false;
    }
}

bool paramEnabled (P id, const ParamValues& p)
{
    switch (id)
    {
        case P::tonalSensitivity: case P::tonalCount: case P::tonalMaxDepth: case P::tonalActivateSec:
            return p.on (P::tonalOn);
        case P::noiseOnly:
            return ! p.on (P::bypass); // バイパスのときは、抑制ノイズ試聴は効かない
        default:
            return true;
    }
}

ParamValues::ParamValues()
{
    for (const auto& p : paramSpecs())
        v[static_cast<size_t> (p.id)] = p.defaultValue;
}

} // namespace vcab
