# wide_eval.py で使う声（build/regression/voices/haruka.wav と zira.wav）を、Windows の音声合成で作る
# 日本語の声（ja-JP の最初の声。make_tts.ps1 と同じ文）と、英語の声（en-US の最初の声）。低い声とささやき声は、wide_eval.py がこの 2 つから作る
Add-Type -AssemblyName System.Speech
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$dir = Join-Path $repo 'build\regression\voices'
New-Item -ItemType Directory -Force $dir | Out-Null
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo 48000, ([System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen), ([System.Speech.AudioFormat.AudioChannel]::Mono)

function Speak($culture, $file, $text) {
    $s = New-Object System.Speech.Synthesis.SpeechSynthesizer
    $voice = $s.GetInstalledVoices() | Where-Object { $_.Enabled -and $_.VoiceInfo.Culture.Name -eq $culture } | Select-Object -First 1
    if ($null -eq $voice) { throw "$culture の音声合成の声が入っていません" }
    $s.SelectVoice($voice.VoiceInfo.Name)
    $out = Join-Path $dir $file
    $s.SetOutputToWaveFile($out, $fmt)
    $s.Speak($text)
    $s.Dispose()
    "作った: $out（$($voice.VoiceInfo.Name)）"
}

Speak 'ja-JP' 'haruka.wav' 'こんにちは。今日はボイスチェンジャーのデノイズを試しています。サ行の、さしすせそ、と、息の音も確かめます。ノイズが消えて、声だけが残れば成功です。'
Speak 'en-US' 'zira.wav' 'Hello. Today I am testing the noise reduction of my voice changer. I will also check the sibilants, she sells sea shells, and the sound of breath. If the noise disappears and only the voice remains, it is a success.'
