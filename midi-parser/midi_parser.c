#include "midi_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

// バイトスワップ関数（ビッグエンディアン → リトルエンディアン）
uint16_t midi_swap_uint16(uint16_t val) {
    return (val << 8) | (val >> 8);
}

uint32_t midi_swap_uint32(uint32_t val) {
    val = ((val << 8) & 0xFF00FF00) | ((val >> 8) & 0x00FF00FF);
    return (val << 16) | (val >> 16);
}

// 可変長数値の読み取り
uint32_t midi_read_variable_length(uint8_t** data, size_t* remaining) {
    uint32_t value = 0;
    uint8_t byte;
    
    do {
        if (*remaining == 0) {
            return 0; // エラー：データ不足
        }
        
        byte = **data;
        (*data)++;
        (*remaining)--;
        
        value = (value << 7) | (byte & 0x7F);
    } while (byte & 0x80);
    
    return value;
}

// ファイルからMIDIファイルをロード
MidiParseResult midi_load_file(const char* filename, MidiFile** midiFile) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "Error: Could not open file %s: %s\n", filename, strerror(errno));
        return MIDI_PARSE_ERROR_FILE_NOT_FOUND;
    }
    
    // ファイルサイズを取得
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (fileSize <= 0) {
        fclose(file);
        return MIDI_PARSE_ERROR_CORRUPTED_DATA;
    }
    
    // データを読み込み
    uint8_t* data = (uint8_t*)malloc(fileSize);
    if (!data) {
        fclose(file);
        return MIDI_PARSE_ERROR_MEMORY_ALLOCATION;
    }
    
    size_t bytesRead = fread(data, 1, fileSize, file);
    fclose(file);
    
    if (bytesRead != (size_t)fileSize) {
        free(data);
        return MIDI_PARSE_ERROR_CORRUPTED_DATA;
    }
    
    // メモリからパース
    MidiParseResult result = midi_load_from_memory(data, fileSize, midiFile);
    
    // パースに失敗した場合はデータを解放
    if (result != MIDI_PARSE_SUCCESS) {
        free(data);
    }
    
    return result;
}

// メモリからMIDIファイルをパース
MidiParseResult midi_load_from_memory(const uint8_t* data, size_t size, MidiFile** midiFile) {
    if (!data || size < 14 || !midiFile) {
        return MIDI_PARSE_ERROR_CORRUPTED_DATA;
    }
    
    // MIDIFile構造体を作成
    MidiFile* midi = (MidiFile*)calloc(1, sizeof(MidiFile));
    if (!midi) {
        return MIDI_PARSE_ERROR_MEMORY_ALLOCATION;
    }
    
    // データバッファをコピー
    midi->data = (uint8_t*)malloc(size);
    if (!midi->data) {
        free(midi);
        return MIDI_PARSE_ERROR_MEMORY_ALLOCATION;
    }
    memcpy(midi->data, data, size);
    midi->dataSize = size;
    
    uint8_t* current = midi->data;
    size_t remaining = size;
    
    // ヘッダーをパース
    if (remaining < 14) {
        midi_free_file(midi);
        return MIDI_PARSE_ERROR_CORRUPTED_DATA;
    }
    
    // ヘッダーを手動で読み取り（パディングを避ける）
    memcpy(midi->header.chunkID, current, 4);
    current += 4;
    
    midi->header.chunkSize = midi_swap_uint32(*(uint32_t*)current);
    current += 4;
    
    midi->header.formatType = midi_swap_uint16(*(uint16_t*)current);
    current += 2;
    
    midi->header.numberOfTracks = midi_swap_uint16(*(uint16_t*)current);
    current += 2;
    
    midi->header.timeDivision = midi_swap_uint16(*(uint16_t*)current);
    current += 2;
    
    remaining -= 14;
    
    // ヘッダーの検証
    if (strncmp(midi->header.chunkID, "MThd", 4) != 0) {
        fprintf(stderr, "Error: Invalid MIDI header signature\n");
        midi_free_file(midi);
        return MIDI_PARSE_ERROR_INVALID_HEADER;
    }
    
    if (midi->header.chunkSize != 6) {
        fprintf(stderr, "Warning: Non-standard header size: %u\n", midi->header.chunkSize);
    }
    
    if (midi->header.formatType > 2) {
        fprintf(stderr, "Warning: Unsupported format type: %u\n", midi->header.formatType);
    }
    
    if (midi->header.numberOfTracks == 0) {
        fprintf(stderr, "Error: No tracks in MIDI file\n");
        midi_free_file(midi);
        return MIDI_PARSE_ERROR_CORRUPTED_DATA;
    }
    
    // トラック配列を作成
    midi->tracks = (MidiTrack*)calloc(midi->header.numberOfTracks, sizeof(MidiTrack));
    if (!midi->tracks) {
        midi_free_file(midi);
        return MIDI_PARSE_ERROR_MEMORY_ALLOCATION;
    }
    
    // 各トラックをパース
    uint32_t maxTicks = 0;
    for (int i = 0; i < midi->header.numberOfTracks; i++) {
        if (remaining < 8) {
            fprintf(stderr, "Error: Insufficient data for track %d header\n", i);
            midi_free_file(midi);
            return MIDI_PARSE_ERROR_CORRUPTED_DATA;
        }
        
        // トラックヘッダーを読み取り
        MidiTrackHeader trackHeader;
        memcpy(trackHeader.chunkID, current, 4);
        current += 4;
        
        trackHeader.chunkSize = midi_swap_uint32(*(uint32_t*)current);
        current += 4;
        remaining -= 8;
        
        // トラックヘッダーの検証
        if (strncmp(trackHeader.chunkID, "MTrk", 4) != 0) {
            fprintf(stderr, "Warning: Invalid track %d header signature '%.4s'\n", i, trackHeader.chunkID);
            // 次のMTrkを探す
            bool found = false;
            while (remaining >= 4) {
                if (strncmp((char*)current, "MTrk", 4) == 0) {
                    found = true;
                    break;
                }
                current++;
                remaining--;
            }
            
            if (!found) {
                fprintf(stderr, "Error: Could not find valid track header\n");
                midi_free_file(midi);
                return MIDI_PARSE_ERROR_CORRUPTED_DATA;
            }
            
            // 再度ヘッダーを読み取り
            memcpy(trackHeader.chunkID, current, 4);
            current += 4;
            
            if (remaining < 4) {
                midi_free_file(midi);
                return MIDI_PARSE_ERROR_CORRUPTED_DATA;
            }
            
            trackHeader.chunkSize = midi_swap_uint32(*(uint32_t*)current);
            current += 4;
            remaining -= 8;
        }
        
        if (remaining < trackHeader.chunkSize) {
            fprintf(stderr, "Error: Insufficient data for track %d content\n", i);
            midi_free_file(midi);
            return MIDI_PARSE_ERROR_CORRUPTED_DATA;
        }
        
        // トラックデータを設定
        midi->tracks[i].data = current;
        midi->tracks[i].current = current;
        midi->tracks[i].size = trackHeader.chunkSize;
        midi->tracks[i].currentTick = 0;
        midi->tracks[i].runningStatus = 0;
        midi->tracks[i].ended = false;
        
        // 次のトラックに移動
        current += trackHeader.chunkSize;
        remaining -= trackHeader.chunkSize;
        
        // このトラックの総ティック数を計算（簡易版）
        uint8_t* trackData = midi->tracks[i].data;
        size_t trackRemaining = midi->tracks[i].size;
        uint32_t trackTicks = 0;
        uint8_t runningStatus = 0;
        
        while (trackRemaining > 0) {
            // デルタタイムを読み取り
            uint32_t deltaTime = midi_read_variable_length(&trackData, &trackRemaining);
            trackTicks += deltaTime;
            
            if (trackRemaining == 0) break;
            
            // イベントタイプを確認
            uint8_t eventByte = *trackData;
            
            if (eventByte & 0x80) {
                // 新しいステータスバイト
                runningStatus = eventByte;
                trackData++;
                trackRemaining--;
            } else if (runningStatus == 0) {
                // ランニングステータスなし、データバイトをスキップ
                trackData++;
                trackRemaining--;
                continue;
            }
            
            // イベントデータをスキップ
            if (runningStatus == 0xFF) {
                // メタイベント
                if (trackRemaining > 0) {
                    uint8_t metaType = *trackData++;
                    trackRemaining--;
                    
                    uint32_t length = midi_read_variable_length(&trackData, &trackRemaining);
                    
                    if (metaType == 0x2F) {
                        // End of Track
                        break;
                    }
                    
                    if (trackRemaining >= length) {
                        trackData += length;
                        trackRemaining -= length;
                    } else {
                        break;
                    }
                }
            } else if (runningStatus == 0xF0 || runningStatus == 0xF7) {
                // SysEx
                uint32_t length = midi_read_variable_length(&trackData, &trackRemaining);
                if (trackRemaining >= length) {
                    trackData += length;
                    trackRemaining -= length;
                } else {
                    break;
                }
            } else {
                // チャンネルメッセージ
                uint8_t msgType = runningStatus & 0xF0;
                size_t dataSize = (msgType == 0xC0 || msgType == 0xD0) ? 1 : 2;
                
                if (trackRemaining >= dataSize) {
                    trackData += dataSize;
                    trackRemaining -= dataSize;
                } else {
                    break;
                }
            }
        }
        
        if (trackTicks > maxTicks) {
            maxTicks = trackTicks;
        }
    }
    
    midi->totalTicks = maxTicks;
    *midiFile = midi;
    
    return MIDI_PARSE_SUCCESS;
}

// MIDIファイルを解放
void midi_free_file(MidiFile* midiFile) {
    if (!midiFile) return;
    
    if (midiFile->tracks) {
        free(midiFile->tracks);
    }
    
    if (midiFile->data) {
        free(midiFile->data);
    }
    
    free(midiFile);
}

// 次のMIDIイベントを読み取り
bool midi_read_next_event(MidiTrack* track, MidiEvent* event) {
    if (!track || !event || track->ended) {
        return false;
    }
    
    size_t remaining = track->size - (track->current - track->data);
    if (remaining == 0) {
        track->ended = true;
        return false;
    }
    
    // イベント構造体を初期化
    memset(event, 0, sizeof(MidiEvent));
    
    // デルタタイムを読み取り
    event->deltaTime = midi_read_variable_length(&track->current, &remaining);
    track->currentTick += event->deltaTime;
    
    if (remaining == 0) {
        track->ended = true;
        return false;
    }
    
    // イベントタイプを読み取り
    uint8_t eventByte = *track->current;
    
    if (eventByte & 0x80) {
        // 新しいステータスバイト
        track->runningStatus = eventByte;
        track->current++;
        remaining--;
    } else if (track->runningStatus == 0) {
        // ランニングステータスが設定されていない
        track->ended = true;
        return false;
    } else {
        // ランニングステータスを使用
        eventByte = track->runningStatus;
    }
    
    event->eventType = (MidiEventType)(eventByte & 0xF0);
    event->channel = eventByte & 0x0F;
    
    // イベントデータを処理
    if (eventByte == 0xFF) {
        // メタイベント
        event->eventType = MIDI_EVENT_META;
        
        if (remaining == 0) {
            track->ended = true;
            return false;
        }
        
        event->metaType = *track->current++;
        remaining--;
        
        event->metaLength = midi_read_variable_length(&track->current, &remaining);
        
        if (remaining < event->metaLength) {
            track->ended = true;
            return false;
        }
        
        if (event->metaLength > 0) {
            event->metaData = (uint8_t*)malloc(event->metaLength);
            if (event->metaData) {
                memcpy(event->metaData, track->current, event->metaLength);
            }
            track->current += event->metaLength;
            remaining -= event->metaLength;
        }
        
        // End of Track チェック
        if (event->metaType == MIDI_META_END_OF_TRACK) {
            track->ended = true;
        }
        
    } else if (eventByte == 0xF0 || eventByte == 0xF7) {
        // SysEx イベント
        event->eventType = (eventByte == 0xF0) ? MIDI_EVENT_SYSEX : MIDI_EVENT_SYSEX_END;
        
        event->sysexLength = midi_read_variable_length(&track->current, &remaining);
        
        if (remaining < event->sysexLength) {
            track->ended = true;
            return false;
        }
        
        if (event->sysexLength > 0) {
            event->sysexData = (uint8_t*)malloc(event->sysexLength);
            if (event->sysexData) {
                memcpy(event->sysexData, track->current, event->sysexLength);
            }
            track->current += event->sysexLength;
            remaining -= event->sysexLength;
        }
        
    } else {
        // チャンネルメッセージ
        uint8_t msgType = eventByte & 0xF0;
        
        if (msgType == 0xC0 || msgType == 0xD0) {
            // プログラムチェンジ、チャンネルプレッシャー（1バイト）
            if (remaining < 1) {
                track->ended = true;
                return false;
            }
            event->data1 = *track->current++;
            remaining--;
        } else {
            // その他のチャンネルメッセージ（2バイト）
            if (remaining < 2) {
                track->ended = true;
                return false;
            }
            event->data1 = *track->current++;
            event->data2 = *track->current++;
            remaining -= 2;
        }
    }
    
    return true;
}

// イベントメモリを解放
void midi_free_event(MidiEvent* event) {
    if (!event) return;
    
    if (event->metaData) {
        free(event->metaData);
        event->metaData = NULL;
    }
    
    if (event->sysexData) {
        free(event->sysexData);
        event->sysexData = NULL;
    }
}

// ヘッダー情報を出力
void midi_print_header_info(const MidiFile* midiFile) {
    if (!midiFile) return;
    
    printf("MIDI File Information:\n");
    printf("  Format Type: %u\n", midiFile->header.formatType);
    printf("  Number of Tracks: %u\n", midiFile->header.numberOfTracks);
    printf("  Time Division: %u\n", midiFile->header.timeDivision);
    printf("  Total Ticks: %u\n", midiFile->totalTicks);
    
    if (midiFile->header.timeDivision & 0x8000) {
        // SMPTE時間
        int framerate = -(int8_t)(midiFile->header.timeDivision >> 8);
        int ticksPerFrame = midiFile->header.timeDivision & 0xFF;
        printf("  SMPTE Format: %d fps, %d ticks per frame\n", framerate, ticksPerFrame);
    } else {
        printf("  Ticks per quarter note: %u\n", midiFile->header.timeDivision);
    }
}

// トラック情報を出力
void midi_print_track_info(const MidiFile* midiFile, int trackIndex) {
    if (!midiFile || trackIndex < 0 || trackIndex >= midiFile->header.numberOfTracks) {
        return;
    }
    
    const MidiTrack* track = &midiFile->tracks[trackIndex];
    printf("Track %d Information:\n", trackIndex);
    printf("  Data Size: %zu bytes\n", track->size);
    printf("  Current Tick: %u\n", track->currentTick);
    printf("  Ended: %s\n", track->ended ? "Yes" : "No");
}

// イベント情報を出力
void midi_print_event_info(const MidiEvent* event) {
    if (!event) return;
    
    printf("MIDI Event:\n");
    printf("  Delta Time: %u\n", event->deltaTime);
    printf("  Event Type: 0x%02X\n", event->eventType);
    
    if (event->eventType != MIDI_EVENT_META && event->eventType != MIDI_EVENT_SYSEX) {
        printf("  Channel: %u\n", event->channel);
    }
    
    switch (event->eventType) {
        case MIDI_EVENT_NOTE_OFF:
            printf("  Note OFF: Note=%u, Velocity=%u\n", event->data1, event->data2);
            break;
        case MIDI_EVENT_NOTE_ON:
            printf("  Note ON: Note=%u, Velocity=%u\n", event->data1, event->data2);
            break;
        case MIDI_EVENT_POLY_PRESSURE:
            printf("  Poly Pressure: Note=%u, Pressure=%u\n", event->data1, event->data2);
            break;
        case MIDI_EVENT_CONTROL_CHANGE:
            printf("  Control Change: Controller=%u, Value=%u\n", event->data1, event->data2);
            break;
        case MIDI_EVENT_PROGRAM_CHANGE:
            printf("  Program Change: Program=%u\n", event->data1);
            break;
        case MIDI_EVENT_CHANNEL_PRESSURE:
            printf("  Channel Pressure: Pressure=%u\n", event->data1);
            break;
        case MIDI_EVENT_PITCH_BEND:
            printf("  Pitch Bend: LSB=%u, MSB=%u (Value=%d)\n", 
                   event->data1, event->data2, 
                   (event->data2 << 7) | event->data1);
            break;
        case MIDI_EVENT_META:
            printf("  Meta Event: Type=0x%02X, Length=%u\n", event->metaType, event->metaLength);
            if (event->metaType == MIDI_META_SET_TEMPO && event->metaLength == 3) {
                uint32_t tempo = (event->metaData[0] << 16) | (event->metaData[1] << 8) | event->metaData[2];
                double bpm = 60000000.0 / tempo;
                printf("    Tempo: %u microseconds per quarter note (%.2f BPM)\n", tempo, bpm);
            }
            break;
        case MIDI_EVENT_SYSEX:
            printf("  SysEx: Length=%u\n", event->sysexLength);
            break;
        default:
            printf("  Unknown Event\n");
            break;
    }
}

// 時間計算ヘルパー関数
double midi_ticks_to_time(uint32_t ticks, uint32_t division, uint32_t tempo) {
    if (division & 0x8000) {
        // SMPTE時間
        int framerate = -(int8_t)(division >> 8);
        int ticksPerFrame = division & 0xFF;
        return (double)ticks / (framerate * ticksPerFrame);
    } else {
        // 音楽時間
        double quarterNotes = (double)ticks / division;
        return quarterNotes * tempo / 1000000.0; // 秒単位
    }
}

uint32_t midi_time_to_ticks(double time, uint32_t division, uint32_t tempo) {
    if (division & 0x8000) {
        // SMPTE時間
        int framerate = -(int8_t)(division >> 8);
        int ticksPerFrame = division & 0xFF;
        return (uint32_t)(time * framerate * ticksPerFrame);
    } else {
        // 音楽時間
        double quarterNotes = time * 1000000.0 / tempo;
        return (uint32_t)(quarterNotes * division);
    }
}