// パラメータの定義。プラグイン（JUCE の APVTS）と WAV 処理ツールの両方がこの表を使う。
// JUCE には依存しない。作りは VoiChanAlgoBox の Params と同じ。
#pragma once

#include <array>
#include <string>
#include <vector>

namespace vcab
{

// パラメータの番号（ホストのオートメーションの一覧もこの順番）。Params.cpp の表もこの順番に並べる。
enum class P : int
{
    // 全体
    bypass,
    noiseOnly,

    // 広い帯域（短い側と中くらいの側）の抑制
    maxReduction,
    strength,
    attackMs,
    releaseMs,
    presenceOn,
    cepstrumOn,
    onsetProtect,

    // ノイズ推定
    noiseMode,
    rnnThreshold,
    saveLearned,

    // 狭帯域ノイズ（長い側）
    tonalOn,
    tonalSensitivity,
    tonalCount,
    tonalMaxDepth,
    tonalLowHz,
    tonalHighHz,
    tonalNeighborHz,
    tonalActivateSec,
    tonalReleaseSec,
    tonalJumpSec,
    tonalJumpRange,
    tonalWidth,
    tonalLowWidth,
    tonalQuietDb,

    // 画面と入力（音の処理の設定ではないもの）
    inputChannel,
    showNoiseEstimate,

    count
};

constexpr int kNumParams = static_cast<int> (P::count);

enum class ParamType
{
    floating,
    toggle,
    choice
};

struct ParamSpec
{
    P id;
    const char* key;   // 保存やツールの引数で使う名前（英数字）
    const char* name;  // 画面に出す名前（日本語、UTF-8）
    ParamType type;
    float minValue;
    float maxValue;
    float defaultValue;
    float step;        // 0 なら連続
    float skewCentre;  // 0 なら直線。値を入れると、その値がつまみの中央に来る
    const char* unit;
    std::vector<const char*> choices; // choice のときの選択肢
};

// ノイズ推定の方式（noise_mode の選択肢の番号）
enum class NoiseMode : int
{
    automatic, // 自動追従（RNNoise の声の判定が「声なし」の間だけ追いかける）
    learned    // 学習固定
};

struct ParamValues;
const std::vector<ParamSpec>& paramSpecs();
const char* paramDescription (P id); // 画面でマウスを乗せたときに出す説明（UTF-8）
const char* choiceDescription (P id, int index); // 選択肢ごとの説明。ないときは nullptr
const ParamSpec& spec (P id);
int findParamByKey (const std::string& key); // 見つからなければ -1

bool isUiSetting (P id); // 画面と入力の設定（オートメーションの対象にしない）
// 今の設定で、その項目が意味を持つか（持たないときは画面で灰色にする）
bool paramEnabled (P id, const ParamValues& p);

// 実行中の値の一式。オーディオ処理はブロックごとにこれを受け取る。
struct ParamValues
{
    std::array<float, kNumParams> v {};

    ParamValues();  // 初期値で埋める

    float get (P id) const noexcept { return v[static_cast<size_t> (id)]; }
    void set (P id, float value) noexcept { v[static_cast<size_t> (id)] = value; }
    bool on (P id) const noexcept { return get (id) >= 0.5f; }
    int index (P id) const noexcept { return static_cast<int> (get (id) + 0.5f); }
};

} // namespace vcab
