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
        { P::maxReduction,        "max_reduction",       "抑制量上限",                 T::floating, 0,       30,      24,      0.5f,  0,      "dB", {} },
        { P::strength,            "strength",            "強度",                       T::floating, 0,       12,      6,       0.5f,  0,      "dB", {} },
        { P::attackMs,            "attack",              "アタック",                   T::floating, 0.5f,    50,      10,      0.1f,  5,      "ms", {} },
        { P::releaseMs,           "release",             "リリース",                   T::floating, 10,      1000,    100,     1,     150,    "ms", {} },
        { P::presenceOn,          "presence_on",         "音声存在確率（OM-LSA）",     T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::cepstrumOn,          "cepstrum_on",         "ケプストラム平滑化",         T::toggle,   0,       1,       0,       1,     0,      "",   {} },
        { P::onsetProtect,        "onset_protect",       "立ち上がり保護",             T::toggle,   0,       1,       1,       1,     0,      "",   {} },

        // ノイズ推定
        { P::noiseMode,           "noise_mode",          "ノイズ推定",                 T::choice,   0,       1,       0,       1,     0,      "",   { "自動追従", "学習固定" } },
        { P::rnnThreshold,        "rnn_threshold",       "声判定しきい値",             T::floating, 10,      90,      50,      1,     0,      "%",  {} },
        { P::saveLearned,         "save_learned",        "学習ノイズ保存",             T::toggle,   0,       1,       1,       1,     0,      "",   {} },

        // 狭帯域ノイズ
        { P::tonalOn,             "tonal_on",            "狭帯域抑制",                 T::toggle,   0,       1,       1,       1,     0,      "",   {} },
        { P::tonalSensitivity,    "tonal_sensitivity",   "感度",                       T::floating, 3,       30,      12,      0.5f,  0,      "dB", {} },
        { P::tonalCount,          "tonal_count",         "本数上限",                   T::floating, 1,       8,       4,       1,     0,      "本", {} },
        { P::tonalMaxDepth,       "tonal_max_depth",     "深さ上限",                   T::floating, 0,       40,      24,      0.5f,  0,      "dB", {} },
        { P::tonalLowHz,          "tonal_low",           "探索下限",                   T::floating, 20,      1000,    20,      1,     100,    "Hz", {} },
        { P::tonalHighHz,         "tonal_high",          "探索上限",                   T::floating, 1000,    20000,   16000,   10,    6000,   "Hz", {} },
        { P::tonalNeighborHz,     "tonal_neighbor",      "周囲比較幅",                 T::floating, 50,      1000,    190,     1,     250,    "Hz", {} },
        { P::tonalActivateSec,    "tonal_activate",      "持続判定時間",               T::floating, 0.1f,    5,       1,       0.05f, 1,      "秒", {} },
        { P::tonalReleaseSec,     "tonal_release",       "解除時間",                   T::floating, 0.1f,    10,      2,       0.1f,  2,      "秒", {} },
        { P::tonalJumpSec,        "tonal_jump_time",     "跳躍判定時間",               T::floating, 0,       2,       0.5f,    0.05f, 0,      "秒", {} },
        { P::tonalJumpRange,      "tonal_jump_range",    "跳躍判定範囲",               T::floating, 0,       100,     30,      1,     0,      "%",  {} },
        { P::tonalWidth,          "tonal_width",         "谷幅（2kHz）",               T::floating, 10,      300,     70,      1,     70,     "Hz", {} },
        { P::tonalLowWidth,       "tonal_low_width",     "谷幅（80Hz 未満）",          T::floating, 5,       60,      20,      0.5f,  0,      "Hz", {} },
        { P::tonalQuietDb,        "tonal_quiet",         "最小検出レベル",             T::floating, -120,    -40,     -96,     1,     0,      "dBFS", {} },

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
        case P::presenceOn:          return "区画ごとに「声がある確率」を求め、声がなさそうな所は上限まで下げます（OM-LSA）。言葉の合間が静かになります。";
        case P::cepstrumOn:          return "下げる量を、声の高さの並びを残してならします。ピロピロした音が減ります。オフのときは時間でならします。";
        case P::onsetProtect:        return "急に大きくなった帯域をすぐ開けて、話し始めの子音（カ行、タ行など）が抑制されるのを防ぎます。";

        case P::noiseMode:           return "ノイズの大きさを推定する方法。";
        case P::rnnThreshold:        return "AI（RNNoise）が出す声の確率が、これを超えたら声とみなします。ささやき声を使うなら下げます。";
        case P::saveLearned:         return "学習で覚えたノイズを設定と一緒に保存し、次に開いたときも使います（EXE は閉じたとき、DAW はプロジェクトに）。";

        case P::tonalOn:             return "「ふぃーん」「キーン」のような狭帯域の持続音を見つけて、その周波数だけを狭く抑制します。周波数が動いても追いかけます。";
        case P::tonalSensitivity:    return "周りより何 dB 突き出たら、狭帯域ノイズとみなすか。小さくすると弱い音も拾いますが、声の倍音を誤って拾いやすくなります。";
        case P::tonalCount:          return "同時に抑制する狭帯域ノイズの数（突き出た量の大きい順）。";
        case P::tonalMaxDepth:       return "1 本の谷で抑制する量の上限。抑制する量は、周りより突き出た分だけ（周りの高さにそろえる）です。";
        case P::tonalLowHz:          return "狭帯域ノイズを探す範囲の下限。PC のファンなどの低い音も消すなら下げます。";
        case P::tonalHighHz:         return "狭帯域ノイズを探す範囲の上限。";
        case P::tonalNeighborHz:     return "狭帯域ノイズかどうかを、前後この幅の平均の高さと比べて判断します。";
        case P::tonalActivateSec:    return "これだけ居続けたら抑制し始めます。短くすると、伸ばした母音や歌を誤って抑制しやすくなります。";
        case P::tonalReleaseSec:     return "狭帯域ノイズが消えてから、この時間をかけて抑制をやめます。";
        case P::tonalJumpSec:        return "抑制している音が消えて、この時間のうちに近くに新しい音が出たら、同じ音が飛んだとみなし、すぐに抑制します。";
        case P::tonalJumpRange:      return "跳躍判定時間のうちに出た新しい音を、同じ音が飛んだとみなす周波数の範囲（消えた音の周波数に対する割合）。";
        case P::tonalWidth:          return "谷の幅（2kHz のとき。周波数に比例して変わります）。広いと動く音を逃しにくく、狭いと声への影響が小さくなります。";
        case P::tonalLowWidth:       return "80Hz より低い音の谷の幅。この高さには声がないので、太めにしてまとめて抑制します。";
        case P::tonalQuietDb:        return "これより小さい狭帯域ノイズは、聞こえないものとして追いかけません。";

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
        case P::tonalSensitivity: case P::tonalCount: case P::tonalMaxDepth: case P::tonalLowHz: case P::tonalHighHz:
        case P::tonalNeighborHz: case P::tonalActivateSec: case P::tonalReleaseSec: case P::tonalJumpSec: case P::tonalJumpRange:
        case P::tonalWidth: case P::tonalLowWidth: case P::tonalQuietDb:
            return p.on (P::tonalOn);
        case P::noiseOnly:
            return ! p.on (P::bypass); // バイパスのときは、抑制ノイズ試聴は効かない
        case P::attackMs: case P::releaseMs:
            return ! p.on (P::cepstrumOn); // ケプストラム平滑化のときは、時間のならしを使わない
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
