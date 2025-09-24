# MIDI Parser

MIDIファイルをパースするための軽量なCライブラリです。midiplayer-baseから抜き出したMIDIパース処理を独立したモジュールとして提供します。

## 機能

- MIDIファイルの読み込みとパース
- MIDIヘッダー情報の取得
- トラック別のイベント読み取り
- メタイベント、SysExイベントの対応
- 時間計算ヘルパー関数
- メモリ効率的な設計

## ファイル構成

```
midi-parser/
├── midi_parser.h    # ヘッダーファイル
├── midi_parser.c    # 実装ファイル
├── example.c        # 使用例
└── README.md        # このファイル
```

## 使用方法

### 基本的な使用例

```c
#include "midi_parser.h"

int main() {
    MidiFile* midiFile = NULL;
    
    // MIDIファイルをロード
    MidiParseResult result = midi_load_file("example.mid", &midiFile);
    if (result != MIDI_PARSE_SUCCESS) {
        printf("Failed to load MIDI file\n");
        return 1;
    }
    
    // ヘッダー情報を表示
    midi_print_header_info(midiFile);
    
    // 各トラックのイベントを読み取り
    for (int i = 0; i < midiFile->header.numberOfTracks; i++) {
        MidiTrack* track = &midiFile->tracks[i];
        MidiEvent event;
        
        while (midi_read_next_event(track, &event)) {
            // イベントを処理
            if (event.eventType == MIDI_EVENT_NOTE_ON && event.data2 > 0) {
                printf("Note ON: Ch%d, Note%d, Vel%d\n", 
                       event.channel, event.data1, event.data2);
            }
            
            // イベントメモリを解放
            midi_free_event(&event);
        }
    }
    
    // MIDIファイルを解放
    midi_free_file(midiFile);
    return 0;
}
```

### コンパイル

```bash
gcc -o midi_example midi_parser.c example.c
```

### 実行

```bash
./midi_example example.mid
```

## API リファレンス

### データ構造

#### MidiFile
MIDIファイル全体を表す構造体
- `header`: MIDIヘッダー情報
- `tracks`: トラック配列
- `data`: 生データバッファ
- `dataSize`: データサイズ
- `totalTicks`: 総ティック数

#### MidiEvent
個々のMIDIイベントを表す構造体
- `deltaTime`: デルタタイム
- `eventType`: イベントタイプ
- `channel`: チャンネル（0-15）
- `data1`, `data2`: データバイト
- `metaType`, `metaLength`, `metaData`: メタイベント情報
- `sysexLength`, `sysexData`: SysExイベント情報

### 主要関数

#### ファイル操作

```c
// ファイルからMIDIデータを読み込み
MidiParseResult midi_load_file(const char* filename, MidiFile** midiFile);

// メモリからMIDIデータを読み込み
MidiParseResult midi_load_from_memory(const uint8_t* data, size_t size, MidiFile** midiFile);

// MIDIファイルのメモリを解放
void midi_free_file(MidiFile* midiFile);
```

#### イベント読み取り

```c
// 次のMIDIイベントを読み取り
bool midi_read_next_event(MidiTrack* track, MidiEvent* event);

// イベントのメモリを解放
void midi_free_event(MidiEvent* event);
```

#### ユーティリティ

```c
// 可変長数値の読み取り
uint32_t midi_read_variable_length(uint8_t** data, size_t* remaining);

// バイトスワップ
uint16_t midi_swap_uint16(uint16_t val);
uint32_t midi_swap_uint32(uint32_t val);

// 時間計算
double midi_ticks_to_time(uint32_t ticks, uint32_t division, uint32_t tempo);
uint32_t midi_time_to_ticks(double time, uint32_t division, uint32_t tempo);
```

#### デバッグ・情報表示

```c
// ヘッダー情報を表示
void midi_print_header_info(const MidiFile* midiFile);

// トラック情報を表示
void midi_print_track_info(const MidiFile* midiFile, int trackIndex);

// イベント情報を表示
void midi_print_event_info(const MidiEvent* event);
```

## エラーコード

```c
typedef enum {
    MIDI_PARSE_SUCCESS,                    // 成功
    MIDI_PARSE_ERROR_FILE_NOT_FOUND,       // ファイルが見つからない
    MIDI_PARSE_ERROR_INVALID_HEADER,       // 無効なヘッダー
    MIDI_PARSE_ERROR_MEMORY_ALLOCATION,    // メモリ割り当て失敗
    MIDI_PARSE_ERROR_CORRUPTED_DATA,       // データ破損
    MIDI_PARSE_ERROR_UNKNOWN               // 不明なエラー
} MidiParseResult;
```

## 対応するMIDIイベント

### チャンネルメッセージ
- Note OFF (0x80)
- Note ON (0x90)
- Polyphonic Key Pressure (0xA0)
- Control Change (0xB0)
- Program Change (0xC0)
- Channel Pressure (0xD0)
- Pitch Bend (0xE0)

### システムメッセージ
- SysEx (0xF0)
- End of SysEx (0xF7)

### メタイベント
- Sequence Number (0x00)
- Text Events (0x01-0x07)
- Channel Prefix (0x20)
- End of Track (0x2F)
- Set Tempo (0x51)
- SMPTE Offset (0x54)
- Time Signature (0x58)
- Key Signature (0x59)
- Sequencer Specific (0x7F)

## 特徴

### 軽量性
- 外部依存なし（標準Cライブラリのみ）
- 最小限のメモリ使用量
- 高速なパース処理

### 安全性
- 境界チェック
- メモリリーク防止
- エラーハンドリング

### 互換性
- SMF（Standard MIDI File）フォーマット対応
- Format 0, 1, 2 対応
- 破損したファイルへの堅牢性

## 元ソースからの変更点

1. **依存関係の除去**
   - OmniMIDI、プレイヤー機能への依存を除去
   - パース処理のみに特化

2. **API の簡素化**
   - 使いやすいインターフェース
   - 明確な関数命名

3. **メモリ管理の改善**
   - 明示的なメモリ管理
   - リーク防止機構

4. **エラーハンドリングの強化**
   - 詳細なエラーコード
   - 破損データへの対応

## ライセンス

このライブラリは元のmidiplayer-baseプロジェクトから抜き出したものです。
元プロジェクトのライセンスに従います。