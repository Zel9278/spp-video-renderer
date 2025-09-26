#ifndef MIDI_PARSER_H
#define MIDI_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// MIDIイベントの種類を定義する列挙型
typedef enum {
    MIDI_EVENT_NOTE_OFF = 0x80,
    MIDI_EVENT_NOTE_ON = 0x90,
    MIDI_EVENT_POLY_PRESSURE = 0xA0,
    MIDI_EVENT_CONTROL_CHANGE = 0xB0,
    MIDI_EVENT_PROGRAM_CHANGE = 0xC0,
    MIDI_EVENT_CHANNEL_PRESSURE = 0xD0,
    MIDI_EVENT_PITCH_BEND = 0xE0,
    MIDI_EVENT_META = 0xFF,
    MIDI_EVENT_SYSEX = 0xF0,
    MIDI_EVENT_SYSEX_END = 0xF7,
    MIDI_EVENT_UNKNOWN
} MidiEventType;

// メタイベントの種類
typedef enum {
    MIDI_META_SEQUENCE_NUMBER = 0x00,
    MIDI_META_TEXT = 0x01,
    MIDI_META_COPYRIGHT = 0x02,
    MIDI_META_TRACK_NAME = 0x03,
    MIDI_META_INSTRUMENT_NAME = 0x04,
    MIDI_META_LYRIC = 0x05,
    MIDI_META_MARKER = 0x06,
    MIDI_META_CUE_POINT = 0x07,
    MIDI_META_CHANNEL_PREFIX = 0x20,
    MIDI_META_END_OF_TRACK = 0x2F,
    MIDI_META_SET_TEMPO = 0x51,
    MIDI_META_SMPTE_OFFSET = 0x54,
    MIDI_META_TIME_SIGNATURE = 0x58,
    MIDI_META_KEY_SIGNATURE = 0x59,
    MIDI_META_SEQUENCER_SPECIFIC = 0x7F
} MidiMetaEventType;

// MIDIファイルヘッダー構造体
typedef struct {
    char chunkID[4];           // "MThd"
    uint32_t chunkSize;        // 通常は6
    uint16_t formatType;       // 0, 1, 2
    uint16_t numberOfTracks;   // トラック数
    uint16_t timeDivision;     // 分解能
} MidiHeader;

// MIDIトラックヘッダー構造体
typedef struct {
    char chunkID[4];           // "MTrk"
    uint32_t chunkSize;        // トラックデータのサイズ
} MidiTrackHeader;

// MIDIイベント構造体
typedef struct {
    uint32_t deltaTime;        // デルタタイム
    MidiEventType eventType;   // イベントタイプ
    uint8_t channel;           // チャンネル (0-15)
    uint8_t data1;             // 1番目のデータバイト
    uint8_t data2;             // 2番目のデータバイト
    uint8_t metaType;          // メタイベントタイプ（メタイベントの場合）
    uint32_t metaLength;       // メタイベントデータ長
    uint8_t* metaData;         // メタイベントデータ
    uint32_t sysexLength;      // SysExデータ長
    uint8_t* sysexData;        // SysExデータ
} MidiEvent;

// トラック構造体
typedef struct {
    uint8_t* data;             // トラックデータの開始位置
    uint8_t* current;          // 現在の読み取り位置
    size_t size;               // トラックデータのサイズ
    uint32_t currentTick;      // 現在の絶対ティック位置
    uint8_t runningStatus;     // ランニングステータス
    bool ended;                // トラック終了フラグ
} MidiTrack;

// MIDIファイル構造体
typedef struct {
    MidiHeader header;         // ヘッダー情報
    MidiTrack* tracks;         // トラック配列
    uint8_t* data;             // 全データバッファ
    size_t dataSize;           // データサイズ
    uint32_t totalTicks;       // 総ティック数
} MidiFile;

// パース結果
typedef enum {
    MIDI_PARSE_SUCCESS,
    MIDI_PARSE_ERROR_FILE_NOT_FOUND,
    MIDI_PARSE_ERROR_INVALID_HEADER,
    MIDI_PARSE_ERROR_MEMORY_ALLOCATION,
    MIDI_PARSE_ERROR_CORRUPTED_DATA,
    MIDI_PARSE_ERROR_UNKNOWN
} MidiParseResult;

// 関数プロトタイプ

// MIDIファイルのロードとパース
MidiParseResult midi_load_file(const char* filename, MidiFile** midiFile);
MidiParseResult midi_load_from_memory(const uint8_t* data, size_t size, MidiFile** midiFile);

// MIDIファイルの解放
void midi_free_file(MidiFile* midiFile);

// イベント読み取り
bool midi_read_next_event(MidiTrack* track, MidiEvent* event);
void midi_free_event(MidiEvent* event);

// ヘルパー関数
uint32_t midi_read_variable_length(uint8_t** data, size_t* remaining);
uint16_t midi_swap_uint16(uint16_t val);
uint32_t midi_swap_uint32(uint32_t val);

// デバッグ・情報取得
void midi_print_header_info(const MidiFile* midiFile);
void midi_print_track_info(const MidiFile* midiFile, int trackIndex);
void midi_print_event_info(const MidiEvent* event);

// 時間計算ヘルパー
double midi_ticks_to_time(uint32_t ticks, uint32_t division, uint32_t tempo);
uint32_t midi_time_to_ticks(double time, uint32_t division, uint32_t tempo);

#ifdef __cplusplus
}
#endif

#endif // MIDI_PARSER_H