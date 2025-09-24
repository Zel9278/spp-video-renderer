#include "midi_video_output.h"
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <imgui.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

// STB Image Write for saving images
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

MidiVideoOutput::MidiVideoOutput()
    : playback_state_(MidiPlaybackState::Stopped)
    , midi_file_(nullptr)
    , current_time_(0.0)
    , total_duration_(0.0)
    , current_event_index_(0)
    , pause_duration_(0.0)
    , current_frame_(0)
    , frame_time_(1.0 / 60.0)  // 60FPS = 0.016666...秒/フレーム
    , is_recording_(false)
    , frame_count_(0)
    , piano_keyboard_(nullptr)
    , renderer_(nullptr)
    , active_notes_(128, false)
    , note_press_times_(128)
    , show_midi_controls_(true)
    , show_video_output_ui_(false)
    , current_tempo_(500000) // デフォルト120 BPM
    , total_note_count_(0)
    , processed_event_count_(0)
    , ffmpeg_process_(nullptr)
{
    // パス文字列を初期化
    strcpy_s(midi_file_path_, sizeof(midi_file_path_), "");
    strcpy_s(video_output_path_, sizeof(video_output_path_), "output_video");
}

MidiVideoOutput::~MidiVideoOutput() {
    Cleanup();
}

bool MidiVideoOutput::Initialize(PianoKeyboard* piano_keyboard, OpenGLRenderer* renderer) {
    if (!piano_keyboard || !renderer) {
        std::cerr << "Error: Invalid piano_keyboard or renderer pointer" << std::endl;
        return false;
    }
    
    piano_keyboard_ = piano_keyboard;
    renderer_ = renderer;
    
    // 初期設定
    video_settings_.output_path = "output_video";
    video_settings_.fps = 60;
    video_settings_.width = 1920;
    video_settings_.height = 1080;
    
    std::cout << "MidiVideoOutput initialized successfully" << std::endl;
    return true;
}

void MidiVideoOutput::Cleanup() {
    if (is_recording_) {
        StopVideoOutput();
    }
    
    // FFmpegプロセスを終了
    FinalizeFFmpeg();
    
    UnloadMidiFile();
    
    piano_keyboard_ = nullptr;
    renderer_ = nullptr;
}

bool MidiVideoOutput::LoadMidiFile(const std::string& filepath) {
    std::cout << "Loading MIDI file: " << filepath << std::endl;
    
    // 既存のファイルをアンロード
    UnloadMidiFile();
    
    // MIDIファイルをロード
    MidiFile* midi_file_raw = nullptr;
    MidiParseResult result = midi_load_file(filepath.c_str(), &midi_file_raw);
    
    if (result != MIDI_PARSE_SUCCESS) {
        std::cerr << "Failed to load MIDI file: " << filepath << " (Error: " << static_cast<int>(result) << ")" << std::endl;
        return false;
    }
    
    midi_file_ = std::unique_ptr<MidiFile>(midi_file_raw);
    
    // ファイルパスを保存
    strncpy_s(midi_file_path_, sizeof(midi_file_path_), filepath.c_str(), _TRUNCATE);
    
    // タイミング情報付きイベントに変換
    ConvertMidiEventsToTimed();
    
    // 総再生時間を計算
    total_duration_ = CalculateTotalDuration();
    
    std::cout << "MIDI file loaded successfully:" << std::endl;
    std::cout << "  Format: " << midi_file_->header.formatType << std::endl;
    std::cout << "  Tracks: " << midi_file_->header.numberOfTracks << std::endl;
    std::cout << "  Division: " << midi_file_->header.timeDivision << std::endl;
    std::cout << "  Duration: " << total_duration_ << " seconds" << std::endl;
    std::cout << "  Total events: " << timed_events_.size() << std::endl;
    std::cout << "  Note events: " << total_note_count_ << std::endl;
    
    return true;
}

void MidiVideoOutput::UnloadMidiFile() {
    Stop();
    
    if (midi_file_) {
        midi_file_.reset();
        timed_events_.clear();
        tempo_changes_.clear();
        
        current_time_ = 0.0;
        total_duration_ = 0.0;
        current_event_index_ = 0;
        total_note_count_ = 0;
        processed_event_count_ = 0;
        
        // アクティブノートをクリア
        std::fill(active_notes_.begin(), active_notes_.end(), false);
        
        std::cout << "MIDI file unloaded" << std::endl;
    }
}

bool MidiVideoOutput::IsMidiLoaded() const {
    return midi_file_ != nullptr;
}

void MidiVideoOutput::Play() {
    if (!IsMidiLoaded()) {
        std::cerr << "No MIDI file loaded" << std::endl;
        return;
    }
    
    if (playback_state_ == MidiPlaybackState::Playing) {
        return; // 既に再生中
    }
    
    if (playback_state_ == MidiPlaybackState::Paused) {
        // 一時停止から再開
        auto now = std::chrono::steady_clock::now();
        auto pause_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - pause_time_).count();
        pause_duration_ += pause_elapsed / 1000.0;
    } else {
        // 新規再生
        playback_start_time_ = std::chrono::steady_clock::now();
        pause_duration_ = 0.0;
        current_event_index_ = 0;
        processed_event_count_ = 0;
        current_frame_ = 0;  // フレームカウンターをリセット
        current_time_ = 0.0;  // 時間もリセット
        
        // すべてのイベントを未処理にリセット
        for (auto& event : timed_events_) {
            event.processed = false;
        }
    }
    
    playback_state_ = MidiPlaybackState::Playing;
    std::cout << "MIDI playback started" << std::endl;
}

void MidiVideoOutput::Pause() {
    if (playback_state_ == MidiPlaybackState::Playing) {
        playback_state_ = MidiPlaybackState::Paused;
        pause_time_ = std::chrono::steady_clock::now();
        std::cout << "MIDI playback paused" << std::endl;
    }
}

void MidiVideoOutput::Stop() {
    playback_state_ = MidiPlaybackState::Stopped;
    current_time_ = 0.0;
    current_event_index_ = 0;
    processed_event_count_ = 0;
    
    // すべてのキーをリリース
    if (piano_keyboard_) {
        for (int note = 0; note < 128; note++) {
            piano_keyboard_->SetKeyPressed(note, false);
        }
    }
    
    // アクティブノートをクリア
    std::fill(active_notes_.begin(), active_notes_.end(), false);
    
    // すべてのイベントを未処理にリセット
    for (auto& event : timed_events_) {
        event.processed = false;
    }
    
    std::cout << "MIDI playback stopped" << std::endl;
}

void MidiVideoOutput::Seek(double time_seconds) {
    if (!IsMidiLoaded()) {
        return;
    }
    
    time_seconds = std::max(0.0, std::min(time_seconds, total_duration_));
    current_time_ = time_seconds;
    
    // 指定時間に対応するイベントインデックスを見つける
    current_event_index_ = 0;
    for (size_t i = 0; i < timed_events_.size(); i++) {
        if (timed_events_[i].time_seconds <= time_seconds) {
            current_event_index_ = i + 1;
        } else {
            break;
        }
    }
    
    // すべてのキーをリリース
    if (piano_keyboard_) {
        for (int note = 0; note < 128; note++) {
            piano_keyboard_->SetKeyPressed(note, false);
        }
    }
    
    // アクティブノートをクリア
    std::fill(active_notes_.begin(), active_notes_.end(), false);
    
    // 現在時刻までのノートオンイベントを適用
    for (size_t i = 0; i < current_event_index_ && i < timed_events_.size(); i++) {
        const auto& timed_event = timed_events_[i];
        if (timed_event.event.eventType == MIDI_EVENT_NOTE_ON && timed_event.event.data2 > 0) {
            // このノートがまだオフになっていないかチェック
            bool note_still_active = true;
            for (size_t j = i + 1; j < current_event_index_ && j < timed_events_.size(); j++) {
                const auto& later_event = timed_events_[j];
                if ((later_event.event.eventType == MIDI_EVENT_NOTE_OFF ||
                     (later_event.event.eventType == MIDI_EVENT_NOTE_ON && later_event.event.data2 == 0)) &&
                    later_event.event.channel == timed_event.event.channel &&
                    later_event.event.data1 == timed_event.event.data1) {
                    note_still_active = false;
                    break;
                }
            }
            
            if (note_still_active && piano_keyboard_) {
                piano_keyboard_->SetKeyPressed(timed_event.event.data1, true);
                active_notes_[timed_event.event.data1] = true;
            }
        }
    }
    
    // イベント処理状態を更新
    for (size_t i = 0; i < timed_events_.size(); i++) {
        timed_events_[i].processed = (i < current_event_index_);
    }
    
    processed_event_count_ = static_cast<int>(current_event_index_);
    
    std::cout << "Seeked to " << time_seconds << " seconds" << std::endl;
}

bool MidiVideoOutput::StartVideoOutput(const VideoOutputSettings& settings) {
    if (!IsMidiLoaded()) {
        std::cerr << "Cannot start video output: No MIDI file loaded" << std::endl;
        return false;
    }
    
    if (is_recording_) {
        std::cerr << "Video output already in progress" << std::endl;
        return false;
    }
    
    video_settings_ = settings;
    
    // 出力ビデオファイルのパスを設定
    output_video_path_ = settings.output_path + ".mp4";
    
    // FFmpegを初期化
    if (!InitializeFFmpeg()) {
        std::cerr << "Failed to initialize FFmpeg" << std::endl;
        return false;
    }
    
    // 録画開始
    is_recording_ = true;
    frame_count_ = 0;
    playback_state_ = MidiPlaybackState::Recording;
    
    // 再生を最初から開始
    Stop();
    current_time_ = 0.0;
    Play();
    
    std::cout << "Video output started:" << std::endl;
    std::cout << "  Output file: " << output_video_path_ << std::endl;
    std::cout << "  Resolution: " << settings.width << "x" << settings.height << std::endl;
    std::cout << "  FPS: " << settings.fps << std::endl;
    std::cout << "  Bitrate: " << settings.bitrate << " bps" << std::endl;
    
    // MIDI情報を表示
    std::cout << "MIDI Information:" << std::endl;
    if (midi_file_ && midi_file_->header.numberOfTracks > 0) {
        // 全トラックのイベント数を計算（概算）
        std::cout << "  Number of tracks: " << midi_file_->header.numberOfTracks << std::endl;
        std::cout << "  Time division: " << midi_file_->header.timeDivision << std::endl;
    } else {
        std::cout << "  No tracks available" << std::endl;
    }
    std::cout << "  Default tempo: " << current_tempo_ << " μs/quarter" << std::endl;
    std::cout << "  Tempo changes: " << tempo_changes_.size() << std::endl;
    
    // 最初の数個のテンポ変更を表示
    for (size_t i = 0; i < std::min((size_t)5, tempo_changes_.size()); ++i) {
        const auto& tc = tempo_changes_[i];
        std::cout << "    Tempo change " << i << ": tick=" << tc.tick 
                  << ", tempo=" << tc.tempo << " μs/quarter" << std::endl;
    }
    
    return true;
}

void MidiVideoOutput::StopVideoOutput() {
    if (is_recording_) {
        is_recording_ = false;
        playback_state_ = MidiPlaybackState::Stopped;
        
        // FFmpegプロセスを終了
        FinalizeFFmpeg();
        
        std::cout << "Video output stopped. Captured " << frame_count_ << " frames" << std::endl;
        std::cout << "Output file: " << output_video_path_ << std::endl;
        
        if (frame_captured_callback_) {
            frame_captured_callback_(-1); // -1 indicates completion
        }
    }
}

bool MidiVideoOutput::IsRecording() const {
    return is_recording_;
}

void MidiVideoOutput::Update(double delta_time) {
    static int update_counter = 0;
    update_counter++;
    
    if (playback_state_ != MidiPlaybackState::Playing && playback_state_ != MidiPlaybackState::Recording) {
        if (update_counter <= 3) {
            std::cout << "Update " << update_counter << ": playback_state_ = " 
                      << static_cast<int>(playback_state_) << " (not Playing/Recording)" << std::endl;
        }
        return;
    }
    
    if (!IsMidiLoaded()) {
        if (update_counter <= 3) {
            std::cout << "Update " << update_counter << ": MIDI not loaded" << std::endl;
        }
        return;
    }
    
    // フレームベースの時間更新
    current_frame_++;
    current_time_ = current_frame_ * frame_time_; // 60FPSベースで正確な時間
    
    if (update_counter <= 3) {
        std::cout << "Update " << update_counter << ": frame=" << current_frame_ 
                  << ", time=" << current_time_ << "s, duration=" << total_duration_ << "s" << std::endl;
    }
    
    // 終了チェック
    if (current_time_ >= total_duration_) {
        if (is_recording_) {
            StopVideoOutput();
        } else {
            Stop();
        }
        return;
    }
    
    // MIDIイベントを処理
    if (update_counter <= 3) {
        std::cout << "Update " << update_counter << ": Processing MIDI events at " << current_time_ << "s" << std::endl;
    }
    ProcessMidiEvents(current_time_);
    
    // アクティブノートを更新
    UpdateActiveNotes(current_time_);
    
    // 進行状況コールバック
    if (progress_callback_) {
        progress_callback_(GetProgress());
    }
    
    // 録画中はフレームをキャプチャ
    if (is_recording_) {
        if (update_counter <= 3) {
            std::cout << "Update " << update_counter << ": Capturing frame" << std::endl;
        }
        CaptureFrame();
    }
}

bool MidiVideoOutput::CaptureFrame() {
    if (!is_recording_ || !renderer_ || !ffmpeg_process_) {
        std::cerr << "CaptureFrame failed: is_recording_=" << is_recording_ 
                  << ", renderer_=" << (renderer_ ? "valid" : "null") 
                  << ", ffmpeg_process_=" << (ffmpeg_process_ ? "valid" : "null") << std::endl;
        return false;
    }
    
    // フレームバッファをキャプチャ
    std::vector<uint8_t> frame_data = CaptureFramebuffer();
    if (frame_data.empty()) {
        std::cerr << "CaptureFrame failed: frame_data is empty" << std::endl;
        return false;
    }
    
    // デバッグ: フレームデータの情報を出力
    if (frame_count_ < 5 || frame_count_ % 100 == 0) {
        std::cerr << "Frame " << frame_count_ << ": data size=" << frame_data.size() 
                  << ", expected=" << (video_settings_.width * video_settings_.height * 4) << std::endl;
        
        // 最初の数ピクセルの値をチェック
        if (frame_data.size() >= 16) {
            std::cerr << "First 4 pixels RGBA: ";
            for (int i = 0; i < 16; i += 4) {
                std::cerr << "(" << (int)frame_data[i] << "," << (int)frame_data[i+1] 
                         << "," << (int)frame_data[i+2] << "," << (int)frame_data[i+3] << ") ";
            }
            std::cerr << std::endl;
        }
    }
    
    // FFmpegプロセスにフレームデータを送信
    bool success = WriteFrameToFFmpeg(frame_data);
    
    if (success) {
        frame_count_++;
        
        if (frame_captured_callback_) {
            frame_captured_callback_(frame_count_);
        }
        
        // 100フレームごとに進行状況を表示
        if (frame_count_ % 100 == 0) {
            std::cout << "Captured frame " << frame_count_ << " (" << std::fixed << std::setprecision(1) 
                      << GetProgress() * 100.0f << "%)" << std::endl;
        }
    }
    
    return success;
}

MidiPlaybackState MidiVideoOutput::GetPlaybackState() const {
    return playback_state_;
}

double MidiVideoOutput::GetCurrentTime() const {
    return current_time_;
}

double MidiVideoOutput::GetTotalDuration() const {
    return total_duration_;
}

float MidiVideoOutput::GetProgress() const {
    if (total_duration_ <= 0.0) {
        return 0.0f;
    }
    return static_cast<float>(current_time_ / total_duration_);
}

VideoOutputSettings& MidiVideoOutput::GetVideoSettings() {
    return video_settings_;
}

const VideoOutputSettings& MidiVideoOutput::GetVideoSettings() const {
    return video_settings_;
}

void MidiVideoOutput::SetVideoSettings(const VideoOutputSettings& settings) {
    video_settings_ = settings;
}

const MidiFile* MidiVideoOutput::GetMidiFile() const {
    return midi_file_.get();
}

std::vector<TimedMidiEvent> MidiVideoOutput::GetEventsInRange(double start_time, double end_time) const {
    std::vector<TimedMidiEvent> events;
    
    for (const auto& event : timed_events_) {
        if (event.time_seconds >= start_time && event.time_seconds <= end_time) {
            events.push_back(event);
        }
    }
    
    return events;
}

int MidiVideoOutput::GetTotalNoteCount() const {
    return total_note_count_;
}

int MidiVideoOutput::GetActiveNoteCount() const {
    return static_cast<int>(std::count(active_notes_.begin(), active_notes_.end(), true));
}

void MidiVideoOutput::SetProgressCallback(std::function<void(float)> callback) {
    progress_callback_ = callback;
}

void MidiVideoOutput::SetFrameCapturedCallback(std::function<void(int)> callback) {
    frame_captured_callback_ = callback;
}

// 内部メソッドの実装

void MidiVideoOutput::ProcessMidiEvents(double current_time) {
    if (!midi_file_) {
        return;
    }
    
    // デバッグ情報
    static int debug_count = 0;
    if (debug_count < 10) {
        std::cout << "ProcessMidiEvents: current_time=" << current_time 
                  << "s, current_event_index=" << current_event_index_ 
                  << "/" << timed_events_.size() << std::endl;
    }
    
    // フレームベースでMIDIイベントを処理
    while (current_event_index_ < timed_events_.size()) {
        const auto& timed_event = timed_events_[current_event_index_];
        
        // 事前計算された時間を使用（リアルタイム計算は不要）
        double event_time = timed_event.time_seconds;
        
        // デバッグ情報
        if (debug_count < 10) {
            std::cout << "  Event " << current_event_index_ << ": tick=" << timed_event.tick 
                      << ", precalculated_time=" << event_time << "s" << std::endl;
            debug_count++;
        }
        
        if (event_time > current_time) {
            break; // まだ時間に達していない
        }
        
        if (!timed_event.processed) {
            ProcessNoteEvent(timed_event.event, event_time);
            timed_events_[current_event_index_].processed = true;
            processed_event_count_++;
        }
        
        current_event_index_++;
    }
}

void MidiVideoOutput::ProcessNoteEvent(const MidiEvent& event, double event_time) {
    if (!piano_keyboard_) {
        return;
    }
    
    if (event.eventType == MIDI_EVENT_NOTE_ON && event.data2 > 0) {
        // ノートオン
        int note = event.data1;
        if (note >= 0 && note < 128) {
            piano_keyboard_->SetKeyPressed(note, true);
            active_notes_[note] = true;
            note_press_times_[note] = std::chrono::steady_clock::now();
            
            // MIDIチャンネル固有の色でブリップエフェクトを常に追加
            Color channel_color = MidiChannelColors::GetChannelColor(event.channel);
            piano_keyboard_->AddKeyBlip(note, channel_color);
        }
    } else if (event.eventType == MIDI_EVENT_NOTE_OFF || 
               (event.eventType == MIDI_EVENT_NOTE_ON && event.data2 == 0)) {
        // ノートオフ
        int note = event.data1;
        if (note >= 0 && note < 128) {
            piano_keyboard_->SetKeyPressed(note, false);
            active_notes_[note] = false;
        }
    }
}

void MidiVideoOutput::UpdateActiveNotes(double current_time) {
    if (!piano_keyboard_) {
        return;
    }
    
    // キー押下継続時間による自動リリース
    auto now = std::chrono::steady_clock::now();
    for (int note = 0; note < 128; note++) {
        if (active_notes_[note]) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - note_press_times_[note]).count();
            if (elapsed > video_settings_.key_press_duration * 1000) {
                piano_keyboard_->SetKeyPressed(note, false);
                active_notes_[note] = false;
            }
        }
    }
}

double MidiVideoOutput::CalculateTotalDuration() {
    if (timed_events_.empty()) {
        return 0.0;
    }
    
    // 最後のイベントの時間を取得
    double last_event_time = 0.0;
    for (const auto& event : timed_events_) {
        if (event.time_seconds > last_event_time) {
            last_event_time = event.time_seconds;
        }
    }
    
    // 少し余裕を持たせる
    return last_event_time + 2.0;
}

void MidiVideoOutput::ConvertMidiEventsToTimed() {
    if (!midi_file_) {
        return;
    }
    
    timed_events_.clear();
    tempo_changes_.clear();
    total_note_count_ = 0;
    
    // デフォルトテンポ
    current_tempo_ = 500000; // 120 BPM
    tempo_changes_.push_back({0, current_tempo_});
    
    // 各トラックのイベントを時系列順に変換
    for (int track_index = 0; track_index < midi_file_->header.numberOfTracks; track_index++) {
        MidiTrack track_copy = midi_file_->tracks[track_index];
        MidiEvent event;
        uint32_t absolute_tick = 0;
        
        while (midi_read_next_event(&track_copy, &event)) {
            absolute_tick += event.deltaTime;
            
            // テンポ変更イベントを記録
            if (event.eventType == MIDI_EVENT_META && event.metaType == MIDI_META_SET_TEMPO && event.metaLength == 3) {
                uint32_t tempo = (event.metaData[0] << 16) | (event.metaData[1] << 8) | event.metaData[2];
                tempo_changes_.push_back({absolute_tick, tempo});
            }
            
            // ノートイベントのみを記録
            if (event.eventType == MIDI_EVENT_NOTE_ON || event.eventType == MIDI_EVENT_NOTE_OFF) {
                TimedMidiEvent timed_event;
                timed_event.event = event;
                timed_event.tick = absolute_tick;
                timed_event.time_seconds = 0.0; // 後で正確に計算
                timed_event.processed = false;
                
                timed_events_.push_back(timed_event);
                
                if (event.eventType == MIDI_EVENT_NOTE_ON && event.data2 > 0) {
                    total_note_count_++;
                }
            }
            
            // デバッグ: 全イベントの最初の20個を表示
            static int event_debug_count = 0;
            if (event_debug_count < 5) { // 5個に減らす
                std::cout << "All event " << event_debug_count << ": tick=" << absolute_tick 
                          << ", type=0x" << std::hex << (int)event.eventType << std::dec;
                if (event.eventType == MIDI_EVENT_NOTE_ON || event.eventType == MIDI_EVENT_NOTE_OFF) {
                    std::cout << " (NOTE " << (event.eventType == MIDI_EVENT_NOTE_ON ? "ON" : "OFF") 
                              << ", note=" << (int)event.data1 << ", vel=" << (int)event.data2 << ")";
                } else if (event.eventType == MIDI_EVENT_META) {
                    std::cout << " (META, type=0x" << std::hex << (int)event.metaType << std::dec << ")";
                }
                std::cout << std::endl;
                event_debug_count++;
            }
            
            midi_free_event(&event);
        }
    }
    
    // テンポ変更を時系列順にソート
    std::sort(tempo_changes_.begin(), tempo_changes_.end(),
              [](const TempoChange& a, const TempoChange& b) {
                  return a.tick < b.tick;
              });
    
    // 正確な時間計算を実行
    CalculateAccurateTiming();
    
    // ソート前の最初の10個のイベントの時間を表示
    std::cout << "Before sorting - First 10 events timing:" << std::endl;
    for (size_t i = 0; i < std::min((size_t)10, timed_events_.size()); ++i) {
        const auto& event = timed_events_[i];
        std::cout << "  Event " << i << ": tick=" << event.tick 
                  << ", time=" << event.time_seconds << "s" << std::endl;
    }
    
    // 時系列順にソート
    std::sort(timed_events_.begin(), timed_events_.end(), 
              [](const TimedMidiEvent& a, const TimedMidiEvent& b) {
                  return a.time_seconds < b.time_seconds;
              });
    
    std::cout << "Converted " << timed_events_.size() << " MIDI events to timed events" << std::endl;
    
    // 最小時間のイベントを検索
    if (!timed_events_.empty()) {
        auto min_time_event = std::min_element(timed_events_.begin(), timed_events_.end(),
            [](const TimedMidiEvent& a, const TimedMidiEvent& b) {
                return a.time_seconds < b.time_seconds;
            });
        std::cout << "Minimum time event: tick=" << min_time_event->tick 
                  << ", time=" << min_time_event->time_seconds << "s" << std::endl;
    }
    
    // ソート後の最初の10個のイベントの時間を表示
    std::cout << "After sorting - First 10 events timing:" << std::endl;
    for (size_t i = 0; i < std::min((size_t)10, timed_events_.size()); ++i) {
        const auto& event = timed_events_[i];
        std::cout << "  Event " << i << ": tick=" << event.tick 
                  << ", time=" << event.time_seconds << "s" << std::endl;
    }
}

void MidiVideoOutput::CalculateAccurateTiming() {
    if (timed_events_.empty() || !midi_file_) {
        return;
    }
    
    // midiplayer-base式の改良アルゴリズムを使用
    for (auto& event : timed_events_) {
        event.time_seconds = CalculateElapsedTimeFromTick(event.tick);
    }
}

// midiplayer-baseを参考にした改良時間計算
double MidiVideoOutput::CalculateElapsedTimeFromTick(uint32_t targetTick) const {
    if (!midi_file_ || midi_file_->header.timeDivision <= 0) {
        return 0.0;
    }
    
    double totalSeconds = 0.0;
    uint32_t currentTick = 0;
    uint32_t currentTempo = current_tempo_; // デフォルトテンポ
    
    // テンポマップが空の場合はデフォルトテンポで計算
    if (tempo_changes_.empty()) {
        return TicksToSeconds(targetTick, midi_file_->header.timeDivision, currentTempo);
    }
    
    // 最初のテンポ変更が0ティックでない場合の初期区間
    if (!tempo_changes_.empty() && tempo_changes_[0].tick > 0) {
        uint32_t initialTicks = std::min(targetTick, tempo_changes_[0].tick);
        totalSeconds += TicksToSeconds(initialTicks, midi_file_->header.timeDivision, currentTempo);
        currentTick = initialTicks;
        
        if (targetTick <= tempo_changes_[0].tick) {
            return totalSeconds;
        }
    }
    
    // テンポマップを使って区間ごとに計算
    for (size_t i = 0; i < tempo_changes_.size(); i++) {
        const auto& tempoChange = tempo_changes_[i];
        
        // 現在のテンポを更新
        currentTempo = tempoChange.tempo;
        
        // 次の区間の終点を決定
        uint32_t nextTick = (i < tempo_changes_.size() - 1) ? 
                           tempo_changes_[i + 1].tick : 
                           targetTick;
        
        // 区間の開始位置を調整
        uint32_t segmentStart = std::max(currentTick, tempoChange.tick);
        uint32_t segmentEnd = std::min(targetTick, nextTick);
        
        if (segmentStart < segmentEnd) {
            uint32_t segmentTicks = segmentEnd - segmentStart;
            totalSeconds += TicksToSeconds(segmentTicks, midi_file_->header.timeDivision, currentTempo);
            currentTick = segmentEnd;
        }
        
        // 目標ティックに達した場合は終了
        if (targetTick <= nextTick) {
            break;
        }
    }
    
    return totalSeconds;
}

double MidiVideoOutput::TicksToSeconds(uint32_t ticks, uint32_t division, uint32_t tempo) const {
    if (division & 0x8000) {
        // SMPTE時間（現在は簡易実装）
        return static_cast<double>(ticks) / 1000.0;
    } else {
        // 音楽時間
        // tempo = マイクロ秒 per quarter note
        // division = ticks per quarter note
        double quarter_notes = static_cast<double>(ticks) / division;
        double seconds_per_quarter = tempo / 1000000.0;  // マイクロ秒を秒に変換
        return quarter_notes * seconds_per_quarter;
    }
}

double MidiVideoOutput::CalculateEventTimeRealtime(uint32_t tick) const {
    if (!midi_file_) {
        return 0.0;
    }
    
    double time = 0.0;
    uint32_t current_tick = 0;
    uint32_t current_tempo = current_tempo_;  // デフォルトテンポから開始
    
    // デバッグ情報（最初の5回のみ）
    static int debug_count = 0;
    bool should_debug = debug_count < 5;
    
    if (should_debug) {
        std::cout << "CalculateEventTimeRealtime: target_tick=" << tick 
                  << ", division=" << midi_file_->header.timeDivision 
                  << ", tempo_changes_count=" << tempo_changes_.size() << std::endl;
    }
    
    // テンポ変更を適用しながら時間を計算
    for (const auto& tempo_change : tempo_changes_) {
        if (tempo_change.tick > tick) {
            // 目標tickに到達する前にテンポ変更があった
            break;
        }
        
        if (tempo_change.tick > current_tick) {
            // 前のテンポで現在ティックまでの時間を加算
            uint32_t tick_diff = tempo_change.tick - current_tick;
            double time_diff = TicksToSeconds(tick_diff, midi_file_->header.timeDivision, current_tempo);
            time += time_diff;
            current_tick = tempo_change.tick;
            
            if (should_debug) {
                std::cout << "  Applied tempo " << current_tempo << " for " << tick_diff 
                          << " ticks, time_diff=" << time_diff << "s" << std::endl;
            }
        }
        
        current_tempo = tempo_change.tempo;
        if (should_debug) {
            std::cout << "  Tempo change at tick " << tempo_change.tick 
                      << " to " << tempo_change.tempo << " μs/quarter" << std::endl;
        }
    }
    
    // 残りのティック差分を現在のテンポで計算
    if (tick > current_tick) {
        uint32_t tick_diff = tick - current_tick;
        double time_diff = TicksToSeconds(tick_diff, midi_file_->header.timeDivision, current_tempo);
        time += time_diff;
        
        if (should_debug) {
            std::cout << "  Final calculation: " << tick_diff << " ticks with tempo " 
                      << current_tempo << ", time_diff=" << time_diff << "s" << std::endl;
        }
    }
    
    if (should_debug) {
        std::cout << "  Final result: " << time << "s" << std::endl;
        debug_count++;
    }
    
    return time;
}

bool MidiVideoOutput::SaveFrameToFile(const std::string& filepath) {
    if (!renderer_) {
        return false;
    }
    
    // フレームバッファからピクセルデータを取得
    std::vector<uint8_t> pixels = CaptureFramebuffer();
    if (pixels.empty()) {
        return false;
    }
    
    // 形式に応じて保存
    if (video_settings_.frame_format == "png") {
        return FrameCapture::SavePNG(filepath, pixels, video_settings_.width, video_settings_.height);
    } else if (video_settings_.frame_format == "jpg" || video_settings_.frame_format == "jpeg") {
        return FrameCapture::SaveJPEG(filepath, pixels, video_settings_.width, video_settings_.height);
    } else if (video_settings_.frame_format == "bmp") {
        return FrameCapture::SaveBMP(filepath, pixels, video_settings_.width, video_settings_.height);
    }
    
    return false;
}

std::vector<uint8_t> MidiVideoOutput::CaptureFramebuffer() {
    if (!renderer_) {
        return {};
    }
    
    int width = video_settings_.width;
    int height = video_settings_.height;
    
    // Use OpenGLRenderer's ReadFramebuffer method for proper offscreen capture
    return renderer_->ReadFramebuffer(width, height);
}

void MidiVideoOutput::CreateOutputDirectory() {
    CreateDirectoryRecursive(output_directory_);
}

std::string MidiVideoOutput::GetTimestampString() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
    return ss.str();
}

bool MidiVideoOutput::CreateDirectoryRecursive(const std::string& path) {
    try {
        std::filesystem::create_directories(path);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to create directory: " << e.what() << std::endl;
        return false;
    }
}

// ImGui UI実装
void MidiVideoOutput::RenderMidiControls() {
    if (!show_midi_controls_) {
        return;
    }
    
    if (ImGui::Begin("MIDI Controls", &show_midi_controls_)) {
        // ファイル読み込み
        ImGui::Text("MIDI File:");
        ImGui::InputText("##midi_path", midi_file_path_, sizeof(midi_file_path_));
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            if (strlen(midi_file_path_) > 0) {
                LoadMidiFile(midi_file_path_);
            }
        }
        
        ImGui::Separator();
        
        if (IsMidiLoaded()) {
            // 再生情報
            ImGui::Text("Duration: %.1f seconds", total_duration_);
            ImGui::Text("Events: %zu", timed_events_.size());
            ImGui::Text("Notes: %d", total_note_count_);
            
            // 再生制御
            if (ImGui::Button("Play")) Play();
            ImGui::SameLine();
            if (ImGui::Button("Pause")) Pause();
            ImGui::SameLine();
            if (ImGui::Button("Stop")) Stop();
            
            // シークバー
            float progress = GetProgress();
            if (ImGui::SliderFloat("Progress", &progress, 0.0f, 1.0f)) {
                Seek(progress * total_duration_);
            }
            
            // 時間表示
            ImGui::Text("Time: %.1f / %.1f", current_time_, total_duration_);
            
            // 再生状態
            const char* state_names[] = {"Stopped", "Playing", "Paused", "Recording"};
            ImGui::Text("State: %s", state_names[static_cast<int>(playback_state_)]);
            
            ImGui::Separator();
            
            // 動画出力UI表示ボタン
            if (ImGui::Button("Video Output Settings")) {
                show_video_output_ui_ = true;
            }
        } else {
            ImGui::Text("No MIDI file loaded");
        }
    }
    ImGui::End();
}

void MidiVideoOutput::RenderVideoOutputUI() {
    if (!show_video_output_ui_) {
        return;
    }
    
    if (ImGui::Begin("Video Output", &show_video_output_ui_)) {
        // 出力設定
        ImGui::InputText("Output Path", video_output_path_, sizeof(video_output_path_));
        video_settings_.output_path = video_output_path_;
        
        ImGui::SliderInt("FPS", &video_settings_.fps, 24, 120);
        ImGui::SliderInt("Width", &video_settings_.width, 640, 3840);
        ImGui::SliderInt("Height", &video_settings_.height, 480, 2160);
        
        const char* formats[] = {"png", "jpg", "bmp"};
        static int format_index = 0;
        if (ImGui::Combo("Format", &format_index, formats, 3)) {
            video_settings_.frame_format = formats[format_index];
        }
        
        ImGui::SliderFloat("Playback Speed", &video_settings_.playback_speed, 0.1f, 4.0f);
        ImGui::SliderFloat("Key Press Duration", &video_settings_.key_press_duration, 0.05f, 1.0f);
        
        ImGui::Checkbox("Rainbow Effects", &video_settings_.show_rainbow_effects);
        ImGui::Checkbox("Key Blips", &video_settings_.show_key_blips);
        
        ImGui::Separator();
        
        // 録画制御
        if (is_recording_) {
            ImGui::Text("Recording... Frame: %d", frame_count_);
            if (ImGui::Button("Stop Recording")) {
                StopVideoOutput();
            }
        } else {
            if (IsMidiLoaded()) {
                if (ImGui::Button("Start Recording")) {
                    StartVideoOutput(video_settings_);
                }
            } else {
                ImGui::Text("Load a MIDI file first");
            }
        }
        
        if (!output_directory_.empty()) {
            ImGui::Text("Last output: %s", output_directory_.c_str());
        }
    }
    ImGui::End();
}

// FrameCapture namespace implementation
namespace FrameCapture {
    bool SavePNG(const std::string& filepath, const std::vector<uint8_t>& rgba_data, int width, int height) {
        return stbi_write_png(filepath.c_str(), width, height, 4, rgba_data.data(), width * 4) != 0;
    }
    
    bool SaveJPEG(const std::string& filepath, const std::vector<uint8_t>& rgba_data, int width, int height, int quality) {
        // RGBAからRGBに変換
        std::vector<uint8_t> rgb_data(width * height * 3);
        for (int i = 0; i < width * height; i++) {
            rgb_data[i * 3 + 0] = rgba_data[i * 4 + 0]; // R
            rgb_data[i * 3 + 1] = rgba_data[i * 4 + 1]; // G
            rgb_data[i * 3 + 2] = rgba_data[i * 4 + 2]; // B
        }
        return stbi_write_jpg(filepath.c_str(), width, height, 3, rgb_data.data(), quality) != 0;
    }
    
    bool SaveBMP(const std::string& filepath, const std::vector<uint8_t>& rgba_data, int width, int height) {
        // RGBAからRGBに変換
        std::vector<uint8_t> rgb_data(width * height * 3);
        for (int i = 0; i < width * height; i++) {
            rgb_data[i * 3 + 0] = rgba_data[i * 4 + 0]; // R
            rgb_data[i * 3 + 1] = rgba_data[i * 4 + 1]; // G
            rgb_data[i * 3 + 2] = rgba_data[i * 4 + 2]; // B
        }
        return stbi_write_bmp(filepath.c_str(), width, height, 3, rgb_data.data()) != 0;
    }
}

// FFmpeg関連のメソッド実装
bool MidiVideoOutput::InitializeFFmpeg() {
    if (ffmpeg_process_) {
        FinalizeFFmpeg();
    }
    
    // FFmpegコマンドを構築
    std::stringstream cmd;
    cmd << "ffmpeg -y"; // -y: ファイルを上書き
    cmd << " -f rawvideo"; // 入力フォーマット: raw video
    cmd << " -pixel_format rgba"; // ピクセルフォーマット: RGBA
    cmd << " -video_size " << video_settings_.width << "x" << video_settings_.height; // 解像度
    cmd << " -framerate " << video_settings_.fps; // フレームレート
    cmd << " -i pipe:0"; // 標準入力から読み取り
    cmd << " -c:v libx264"; // ビデオコーデック: H.264
    cmd << " -preset ultrafast"; // エンコードプリセット: 最高速
    cmd << " -tune zerolatency"; // 低遅延チューニング
    cmd << " -crf 23"; // CRF値 (高速化のため品質を少し下げる)
    cmd << " -b:v " << video_settings_.bitrate; // ビットレート: CBR 8000 kbps
    cmd << " -maxrate " << video_settings_.bitrate; // 最大ビットレート
    cmd << " -bufsize " << (video_settings_.bitrate * 2); // バッファサイズ
    cmd << " -pix_fmt yuv420p"; // 出力ピクセルフォーマット
    cmd << " -threads 0"; // 全CPUコアを使用
    cmd << " \"" << output_video_path_ << "\""; // 出力ファイル
    
    std::string command = cmd.str();
    std::cout << "Starting FFmpeg with command: " << command << std::endl;
    
#ifdef _WIN32
    ffmpeg_process_ = _popen(command.c_str(), "wb");
#else
    ffmpeg_process_ = popen(command.c_str(), "w");
#endif
    
    if (!ffmpeg_process_) {
        std::cerr << "Failed to start FFmpeg process" << std::endl;
        return false;
    }
    
    return true;
}

void MidiVideoOutput::FinalizeFFmpeg() {
    if (ffmpeg_process_) {
        std::cout << "Finalizing FFmpeg process..." << std::endl;
        
        // パイプを閉じる前にバッファをフラッシュ
        fflush(ffmpeg_process_);
        
        // FFmpegプロセスを終了
#ifdef _WIN32
        int result = _pclose(ffmpeg_process_);
#else
        int result = pclose(ffmpeg_process_);
#endif
        ffmpeg_process_ = nullptr;
        
        std::cout << "FFmpeg process closed with result: " << result << std::endl;
        
        if (result == 0) {
            std::cout << "Video encoding completed successfully" << std::endl;
        } else {
            std::cout << "Video encoding finished with errors (exit code: " << result << ")" << std::endl;
        }
    }
}

bool MidiVideoOutput::WriteFrameToFFmpeg(const std::vector<uint8_t>& frame_data) {
    if (!ffmpeg_process_ || frame_data.empty()) {
        std::cerr << "WriteFrameToFFmpeg failed: ffmpeg_process_=" << (ffmpeg_process_ ? "valid" : "null") 
                  << ", frame_data.empty()=" << frame_data.empty() << std::endl;
        return false;
    }
    
    size_t expected_size = video_settings_.width * video_settings_.height * 4; // RGBA
    if (frame_data.size() != expected_size) {
        std::cerr << "Frame data size mismatch. Expected: " << expected_size 
                  << ", Got: " << frame_data.size() << std::endl;
        return false;
    }
    
    // フレームデータをFFmpegプロセスに書き込み
    size_t written = fwrite(frame_data.data(), 1, frame_data.size(), ffmpeg_process_);
    if (written != frame_data.size()) {
        std::cerr << "Failed to write frame data to FFmpeg. Written: " << written 
                  << ", Expected: " << frame_data.size() << ", ferror: " << ferror(ffmpeg_process_) << std::endl;
        return false;
    }
    
    // バッファをフラッシュ
    int flush_result = fflush(ffmpeg_process_);
    if (flush_result != 0) {
        std::cerr << "Failed to flush FFmpeg pipe. fflush returned: " << flush_result 
                  << ", ferror: " << ferror(ffmpeg_process_) << std::endl;
        return false;
    }
    
    return true;
}