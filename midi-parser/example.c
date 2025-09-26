#include "midi_parser.h"
#include <stdio.h>
#include <stdlib.h>

// 使用例とテストプログラム
int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Usage: %s <midi_file>\n", argv[0]);
        return 1;
    }
    
    const char* filename = argv[1];
    MidiFile* midiFile = NULL;
    
    printf("Loading MIDI file: %s\n", filename);
    
    // MIDIファイルをロード
    MidiParseResult result = midi_load_file(filename, &midiFile);
    if (result != MIDI_PARSE_SUCCESS) {
        printf("Failed to load MIDI file. Error code: %d\n", result);
        return 1;
    }
    
    printf("MIDI file loaded successfully!\n\n");
    
    // ヘッダー情報を表示
    midi_print_header_info(midiFile);
    printf("\n");
    
    // 各トラックの基本情報を表示
    for (int i = 0; i < midiFile->header.numberOfTracks; i++) {
        midi_print_track_info(midiFile, i);
        
        // 最初の10個のイベントを表示
        printf("  First 10 events:\n");
        
        MidiTrack trackCopy = midiFile->tracks[i]; // コピーを使用（元データを保護）
        MidiEvent event;
        int eventCount = 0;
        
        while (eventCount < 10 && midi_read_next_event(&trackCopy, &event)) {
            printf("    Event %d: ", eventCount + 1);
            
            switch (event.eventType) {
                case MIDI_EVENT_NOTE_ON:
                    if (event.data2 > 0) { // ベロシティ > 0 のみをNote ONとして扱う
                        printf("Note ON - Ch:%d, Note:%d, Vel:%d\n", 
                               event.channel + 1, event.data1, event.data2);
                    } else {
                        printf("Note OFF - Ch:%d, Note:%d, Vel:%d\n", 
                               event.channel + 1, event.data1, event.data2);
                    }
                    break;
                case MIDI_EVENT_NOTE_OFF:
                    printf("Note OFF - Ch:%d, Note:%d, Vel:%d\n", 
                           event.channel + 1, event.data1, event.data2);
                    break;
                case MIDI_EVENT_PROGRAM_CHANGE:
                    printf("Program Change - Ch:%d, Program:%d\n", 
                           event.channel + 1, event.data1);
                    break;
                case MIDI_EVENT_CONTROL_CHANGE:
                    printf("Control Change - Ch:%d, CC:%d, Value:%d\n", 
                           event.channel + 1, event.data1, event.data2);
                    break;
                case MIDI_EVENT_META:
                    printf("Meta Event - Type:0x%02X", event.metaType);
                    if (event.metaType == MIDI_META_SET_TEMPO && event.metaLength == 3) {
                        uint32_t tempo = (event.metaData[0] << 16) | 
                                       (event.metaData[1] << 8) | 
                                       event.metaData[2];
                        double bpm = 60000000.0 / tempo;
                        printf(" (Tempo: %.2f BPM)", bpm);
                    } else if (event.metaType == MIDI_META_TRACK_NAME && event.metaData) {
                        printf(" (Track Name: ");
                        for (uint32_t j = 0; j < event.metaLength && j < 50; j++) {
                            if (event.metaData[j] >= 32 && event.metaData[j] < 127) {
                                printf("%c", event.metaData[j]);
                            }
                        }
                        printf(")");
                    }
                    printf("\n");
                    break;
                default:
                    printf("Other Event - Type:0x%02X\n", event.eventType);
                    break;
            }
            
            midi_free_event(&event);
            eventCount++;
        }
        
        printf("\n");
    }
    
    // 簡単な統計情報を表示
    printf("=== MIDI File Statistics ===\n");
    int noteCount = 0;
    int tempoChangeCount = 0;
    
    for (int i = 0; i < midiFile->header.numberOfTracks; i++) {
        MidiTrack trackCopy = midiFile->tracks[i];
        MidiEvent event;
        
        while (midi_read_next_event(&trackCopy, &event)) {
            if (event.eventType == MIDI_EVENT_NOTE_ON && event.data2 > 0) {
                noteCount++;
            } else if (event.eventType == MIDI_EVENT_META && 
                      event.metaType == MIDI_META_SET_TEMPO) {
                tempoChangeCount++;
            }
            
            midi_free_event(&event);
        }
    }
    
    printf("Total Note On events: %d\n", noteCount);
    printf("Total Tempo changes: %d\n", tempoChangeCount);
    
    // 概算の演奏時間を計算（デフォルトテンポ120 BPMを仮定）
    if (midiFile->header.timeDivision > 0 && !(midiFile->header.timeDivision & 0x8000)) {
        double estimatedTime = midi_ticks_to_time(midiFile->totalTicks, 
                                                 midiFile->header.timeDivision, 
                                                 500000); // 120 BPM = 500000 microseconds per quarter note
        printf("Estimated duration (120 BPM): %.2f seconds\n", estimatedTime);
    }
    
    // メモリを解放
    midi_free_file(midiFile);
    
    printf("\nMIDI parsing completed successfully!\n");
    return 0;
}