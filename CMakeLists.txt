cmake_minimum_required(VERSION 3.22)

project(VcabDenoise VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Visual C++ のランタイムを EXE と VST3 の中に組み込む（利用者の PC に Visual C++ 再頒布可能パッケージがなくても動くように）
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ソースは UTF-8（日本語の文字列を含むため）
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_subdirectory(external/JUCE)

# ---- RNNoise（声の判定に使う。Xiph、BSD ライセンス） ----
# 学習済みの重み（ソースの形で約 78MB）はリポジトリに含まれないので、公式の配布元から取得し、
# SHA-256 を照合してからビルドの作業フォルダに展開する（初回の設定のときだけ。インターネットが必要）
file(READ external/rnnoise/model_version RNNOISE_MODEL_HASH)
string(STRIP "${RNNOISE_MODEL_HASH}" RNNOISE_MODEL_HASH)
set(RNNOISE_MODEL_DIR ${CMAKE_BINARY_DIR}/rnnoise_model)
if(NOT EXISTS ${RNNOISE_MODEL_DIR}/src/rnnoise_data.c)
    message(STATUS "RNNoise の学習済みの重みを取得します")
    file(DOWNLOAD https://media.xiph.org/rnnoise/models/rnnoise_data-${RNNOISE_MODEL_HASH}.tar.gz
         ${RNNOISE_MODEL_DIR}/rnnoise_data.tar.gz
         EXPECTED_HASH SHA256=${RNNOISE_MODEL_HASH}
         STATUS RNNOISE_DOWNLOAD_STATUS)
    list(GET RNNOISE_DOWNLOAD_STATUS 0 RNNOISE_DOWNLOAD_CODE)
    if(NOT RNNOISE_DOWNLOAD_CODE EQUAL 0)
        message(FATAL_ERROR "RNNoise の重みを取得できませんでした: ${RNNOISE_DOWNLOAD_STATUS}")
    endif()
    file(ARCHIVE_EXTRACT INPUT ${RNNOISE_MODEL_DIR}/rnnoise_data.tar.gz DESTINATION ${RNNOISE_MODEL_DIR}
         PATTERNS src/rnnoise_data.c src/rnnoise_data.h)
    file(REMOVE ${RNNOISE_MODEL_DIR}/rnnoise_data.tar.gz)
endif()
add_library(RNNoise STATIC
    external/rnnoise/src/denoise.c
    external/rnnoise/src/rnn.c
    external/rnnoise/src/pitch.c
    external/rnnoise/src/kiss_fft.c
    external/rnnoise/src/celt_lpc.c
    external/rnnoise/src/nnet.c
    external/rnnoise/src/nnet_default.c
    external/rnnoise/src/parse_lpcnet_weights.c
    external/rnnoise/src/rnnoise_tables.c
    external/rnnoise/src/x86/x86_dnn_map.c
    external/rnnoise/src/x86/x86cpu.c
    external/rnnoise/src/x86/nnet_sse4_1.c
    external/rnnoise/src/x86/nnet_avx2.c
    ${RNNOISE_MODEL_DIR}/src/rnnoise_data.c
)
target_include_directories(RNNoise PUBLIC external/rnnoise/include PRIVATE ${RNNOISE_MODEL_DIR}/src external/rnnoise/src)
target_compile_definitions(RNNoise PUBLIC RNNOISE_EXPORT=)
# CPU を実行時に調べて、SSE4.1 / AVX2 の版を使う（RNNoise の RTCD。古い CPU では普通の版で動く）。
# AVX2 の版のファイルだけ /arch:AVX2 でビルドする（声の判定の計算が速くなる）
target_compile_definitions(RNNoise PRIVATE RNN_ENABLE_X86_RTCD OPUS_X86_MAY_HAVE_SSE OPUS_X86_MAY_HAVE_SSE2)
# SSE4.1 の定義は、その版のファイルだけに付ける（ほかのファイルでは、同梱されていない SSE4.1 の LPC を読み込もうとするため）
set_source_files_properties(external/rnnoise/src/x86/nnet_sse4_1.c PROPERTIES COMPILE_DEFINITIONS OPUS_X86_MAY_HAVE_SSE4_1)
if(MSVC)
    set_source_files_properties(external/rnnoise/src/x86/nnet_avx2.c PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
else()
    set_source_files_properties(external/rnnoise/src/x86/nnet_sse4_1.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
    set_source_files_properties(external/rnnoise/src/x86/nnet_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
endif()
set_target_properties(RNNoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(RNNoise PRIVATE /W0)
endif()

# FFT（pffft。単精度の SSE 版だけを使う。x64 の CPU ならどれでも動く。倍精度の AVX 版は、AVX のない CPU で動かないので使わない）
add_library(PFFFT STATIC
    external/pffft/src/pffft.c
    external/pffft/src/pffft_common.c
)
target_include_directories(PFFFT PUBLIC external/pffft/include/pffft)
target_compile_definitions(PFFFT PRIVATE _USE_MATH_DEFINES PUBLIC PFFFT_STATIC_DEFINE)
set_target_properties(PFFFT PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(PFFFT PRIVATE /W0)
endif()

# ---- 処理の部品（JUCE に依存しない） ----
add_library(VcabDenoiseCore STATIC
    Source/Core/Params.cpp
    Source/Core/Engine.cpp
    Source/Core/Dsp/FadingFir.cpp
    Source/Core/Dsp/GainRule.cpp
    Source/Core/Dsp/MinPhase.cpp
    Source/Core/Dsp/NoiseEstimator.cpp
    Source/Core/Dsp/TonalTracker.cpp
    Source/Core/Dsp/VoiceDetector.cpp
)
target_include_directories(VcabDenoiseCore PUBLIC Source/Core)
target_link_libraries(VcabDenoiseCore PRIVATE RNNoise PUBLIC PFFFT)
set_target_properties(VcabDenoiseCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(VcabDenoiseCore PRIVATE /W4)
endif()

# 使わない形式（音声の FLAC・Ogg Vorbis・Opus、画像の WebP）を JUCE から組み込まない（WAV と PNG だけ使う。
# 組み込むと、そのライブラリのライセンス文も配布物に添える必要がある）
set(VCABDN_UNUSED_FORMATS JUCE_USE_FLAC=0 JUCE_USE_OGGVORBIS=0 JUCE_USE_OPUS=0 JUCE_USE_WEBP=0)

# ---- プラグイン（VST3）と単体アプリ（EXE） ----
juce_add_plugin(VcabDenoise
    COMPANY_NAME "VcabDenoise"
    PRODUCT_NAME "VcabDenoise"
    PLUGIN_MANUFACTURER_CODE Vcha
    PLUGIN_CODE Vcdn
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx
    ICON_BIG Resources/Icon.png
    ICON_SMALL Resources/IconSmall.png
)

target_sources(VcabDenoise PRIVATE
    Source/Plugin/PluginProcessor.cpp
    Source/Plugin/PluginEditor.cpp
    Source/Plugin/LookAndFeel.cpp
    Source/Plugin/MonitorView.cpp
    Source/Standalone/VCStandaloneApp.cpp
)

target_compile_definitions(VcabDenoise PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_ASIO=1
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1  # EXE 版の外枠は Source/Standalone の自作のもの
    ${VCABDN_UNUSED_FORMATS}
)

target_link_libraries(VcabDenoise
    PRIVATE
        VcabDenoiseCore
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ---- 開発用の WAV 処理ツール ----
juce_add_console_app(vcabdn_render PRODUCT_NAME "vcabdn_render")

target_sources(vcabdn_render PRIVATE Tools/Render/Main.cpp)

target_compile_definitions(vcabdn_render PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    ${VCABDN_UNUSED_FORMATS}
)

target_link_libraries(vcabdn_render
    PRIVATE
        VcabDenoiseCore
        juce::juce_audio_formats
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ---- 開発用の画面の撮影ツール（README の画像を作るため。プラグインの画面のコードを、そのまま使う） ----
juce_add_console_app(vcabdn_shot PRODUCT_NAME "vcabdn_shot")

target_sources(vcabdn_shot PRIVATE
    Tools/Shot/Main.cpp
    Source/Plugin/PluginProcessor.cpp
    Source/Plugin/PluginEditor.cpp
    Source/Plugin/LookAndFeel.cpp
    Source/Plugin/MonitorView.cpp
)

target_compile_definitions(vcabdn_shot PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1  # 画面のタイマーを回すため（runDispatchLoopUntil）
    JucePlugin_Name="VcabDenoise"
    ${VCABDN_UNUSED_FORMATS}
)

target_link_libraries(vcabdn_shot
    PRIVATE
        VcabDenoiseCore
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
