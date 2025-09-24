@echo off
REM MIDI Parser Test Script for Windows

echo === MIDI Parser Build and Test ===

REM コンパイル
echo Building MIDI parser...
make clean-win
gcc -Wall -Wextra -std=c99 -O2 -o midi_example.exe midi_parser.c example.c

if %ERRORLEVEL% neq 0 (
    echo Build failed!
    exit /b 1
)

echo Build successful!

REM テスト用のMIDIファイルがあるかチェック
set TEST_MIDI=
for %%f in (*.mid *.midi ..\*.mid ..\*.midi) do (
    if exist "%%f" (
        set TEST_MIDI=%%f
        goto :found
    )
)

:found
if defined TEST_MIDI (
    echo Testing with MIDI file: %TEST_MIDI%
    midi_example.exe "%TEST_MIDI%"
) else (
    echo No MIDI test file found. Please provide a .mid file to test.
    echo Usage: midi_example.exe ^<midi_file^>
)

echo === Test completed ===
pause