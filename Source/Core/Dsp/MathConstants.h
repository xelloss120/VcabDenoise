// 計算で使う定数（円周率と、振幅の dB と自然対数の換算）
#pragma once

namespace vcab
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kLn10 = 2.302585092994046;
constexpr double kDbToLn = kLn10 / 20.0; // 振幅の dB → 自然対数
constexpr double kLnToDb = 20.0 / kLn10; // 自然対数 → 振幅の dB

} // namespace vcab
