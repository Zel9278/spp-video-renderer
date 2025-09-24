#!/bin/bash

# MIDI Parser Test Script

echo "=== MIDI Parser Build and Test ==="

# コンパイル
echo "Building MIDI parser..."
make clean
make

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo "Build successful!"

# テスト用のMIDIファイルがあるかチェック
TEST_MIDI=""
for file in *.mid *.midi ../*.mid ../*.midi; do
    if [ -f "$file" ]; then
        TEST_MIDI="$file"
        break
    fi
done

if [ -n "$TEST_MIDI" ]; then
    echo "Testing with MIDI file: $TEST_MIDI"
    ./midi_example "$TEST_MIDI"
else
    echo "No MIDI test file found. Please provide a .mid file to test."
    echo "Usage: ./midi_example <midi_file>"
fi

echo "=== Test completed ==="