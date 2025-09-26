#include "midi_video_output.h"
#include <iostream>
#include <algorithm>
#include <array>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <cmath>
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
    , total_event_count_(0)
    , last_event_tick_(0)
    , ffmpeg_process_(nullptr)
{
    // パス文字列を初期化
#ifdef _WIN32
    strcpy_s(midi_file_path_, sizeof(midi_file_path_), "");
    strcpy_s(video_output_path_, sizeof(video_output_path_), "output_video");
#else
    strcpy(midi_file_path_, "");
    strcpy(video_output_path_, "output_video");
#endif
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
#ifdef _WIN32
    strncpy_s(midi_file_path_, sizeof(midi_file_path_), filepath.c_str(), _TRUNCATE);
#else
    strncpy(midi_file_path_, filepath.c_str(), sizeof(midi_file_path_) - 1);
    midi_file_path_[sizeof(midi_file_path_) - 1] = '\0';  // null終端を保証
#endif
    
    // テンポマップと統計情報を作成
    BuildTempoMapAndStats();
    
    // 総再生時間を計算
    total_duration_ = CalculateTotalDuration();
    
    // ストリーミング状態を初期化
    ResetStreamingState();
    
    std::cout << "MIDI file loaded successfully:" << std::endl;
    std::cout << "  Format: " << midi_file_->header.formatType << std::endl;
    std::cout << "  Tracks: " << midi_file_->header.numberOfTracks << std::endl;
    std::cout << "  Division: " << midi_file_->header.timeDivision << std::endl;
    std::cout << "  Duration: " << total_duration_ << " seconds" << std::endl;
    std::cout << "  Total events: " << total_event_count_ << std::endl;
    std::cout << "  Note events: " << total_note_count_ << std::endl;
    
    return true;
}

void MidiVideoOutput::UnloadMidiFile() {
    Stop();
    
    if (midi_file_) {
        ClearStreamingResources();
        midi_file_.reset();
        tempo_changes_.clear();
        
        current_time_ = 0.0;
        total_duration_ = 0.0;
        total_note_count_ = 0;
        processed_event_count_ = 0;
        total_event_count_ = 0;
        last_event_tick_ = 0;
        
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
        current_frame_ = 0;  // フレームカウンターをリセット
        current_time_ = 0.0;  // 時間もリセット
        
        ResetStreamingState();
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
    processed_event_count_ = 0;
    
    // すべてのキーをリリース
    if (piano_keyboard_) {
        for (int note = 0; note < 128; note++) {
            piano_keyboard_->SetKeyPressed(note, false);
        }
    }
    
    // アクティブノートをクリア
    std::fill(active_notes_.begin(), active_notes_.end(), false);
    
    ResetStreamingState();
    
    std::cout << "MIDI playback stopped" << std::endl;
}

void MidiVideoOutput::Seek(double time_seconds) {
    if (!IsMidiLoaded()) {
        return;
    }
    
    time_seconds = std::max(0.0, std::min(time_seconds, total_duration_));
    ResetStreamingState();

    // すべてのキーをリリース
    if (piano_keyboard_) {
        for (int note = 0; note < 128; note++) {
            piano_keyboard_->SetKeyPressed(note, false);
        }
    }

    std::fill(active_notes_.begin(), active_notes_.end(), false);

    std::array<bool, 128> note_state{};
    processed_event_count_ = 0;

    while (!pending_events_.empty()) {
        PendingEvent next = pending_events_.top();
        if (next.time_seconds > time_seconds + kTimeEpsilon) {
            break;
        }

        size_t track_index = next.track_index;
        pending_events_.pop();

        if (track_index >= streaming_tracks_.size()) {
            continue;
        }

        auto& track_state = streaming_tracks_[track_index];
        if (!track_state.has_event || std::fabs(track_state.event_time - next.time_seconds) > kTimeEpsilon) {
            continue;
        }

        const MidiEvent& event = track_state.current_event;
        if (event.eventType == MIDI_EVENT_NOTE_ON && event.data2 > 0) {
            int note = event.data1;
            if (note >= 0 && note < 128) {
                note_state[note] = true;
            }
        } else if (event.eventType == MIDI_EVENT_NOTE_OFF ||
                   (event.eventType == MIDI_EVENT_NOTE_ON && event.data2 == 0)) {
            int note = event.data1;
            if (note >= 0 && note < 128) {
                note_state[note] = false;
            }
        }

        midi_free_event(&track_state.current_event);
        track_state.current_event = MidiEvent{};
        track_state.has_event = false;
        processed_event_count_++;

        LoadNextTrackEvent(track_index);
    }

    auto now = std::chrono::steady_clock::now();
    if (piano_keyboard_) {
        for (int note = 0; note < 128; note++) {
            piano_keyboard_->SetKeyPressed(note, note_state[note]);
            active_notes_[note] = note_state[note];
            if (note_state[note]) {
                note_press_times_[note] = now;
            }
        }
    } else {
        for (int note = 0; note < 128; note++) {
            active_notes_[note] = note_state[note];
            if (note_state[note]) {
                note_press_times_[note] = now;
            }
        }
    }

    current_time_ = time_seconds;
    current_frame_ = static_cast<int>(current_time_ / frame_time_);

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
    if (video_settings_.include_audio && video_settings_.audio_file_path.empty()) {
        std::cerr << "Audio output requested but no audio file path provided." << std::endl;
        return false;
    }

    if (video_settings_.include_audio) {
        std::filesystem::path audio_path(video_settings_.audio_file_path);
        if (!std::filesystem::exists(audio_path)) {
            std::cerr << "Audio file does not exist: " << audio_path << std::endl;
            return false;
        }
    }
    
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
    
    // デバッグ情報を初期化
    debug_info_.start_time = std::chrono::system_clock::now();
    debug_info_.recording_start = std::chrono::steady_clock::now();
    debug_info_.elapsed_seconds = 0.0;
    debug_info_.estimated_total_duration = total_duration_;
    debug_info_.current_frame_count = 0;
    debug_info_.current_fps = 0.0;
    
    // 再生を最初から開始
    Stop();
    current_time_ = 0.0;
    Play();
    
    std::cout << "Video output started:" << std::endl;
    std::cout << "  Output file: " << output_video_path_ << std::endl;
    std::cout << "  Resolution: " << settings.width << "x" << settings.height << std::endl;
    std::cout << "  FPS: " << settings.fps << std::endl;
    std::cout << "  Bitrate: " << settings.bitrate << " bps" << std::endl;
    if (video_settings_.include_audio) {
        std::cout << "  Audio file: " << video_settings_.audio_file_path << std::endl;
        std::cout << "  Audio codec: aac" << std::endl;
        std::cout << "  Audio bitrate: " << video_settings_.audio_bitrate << " bps" << std::endl;
    } else {
        std::cout << "  Audio file: (none)" << std::endl;
    }
    
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
    
    // デバッグ情報の更新
    if (is_recording_ && video_settings_.show_debug_info) {
        UpdateDebugInfo();
    }
    
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
    
    // Measure frame capture time
    auto capture_start = std::chrono::high_resolution_clock::now();
    
    // フレームバッファをキャプチャ
    std::vector<uint8_t> frame_data = CaptureFramebuffer();
    
    auto capture_end = std::chrono::high_resolution_clock::now();
    auto capture_duration = std::chrono::duration_cast<std::chrono::microseconds>(capture_end - capture_start);
    
    if (frame_data.empty()) {
        std::cerr << "CaptureFrame failed: frame_data is empty" << std::endl;
        return false;
    }
    
    // デバッグ: フレームデータとパフォーマンス情報を出力
    if (frame_count_ < 5 || frame_count_ % 100 == 0) {
        std::cerr << "Frame " << frame_count_ << ": data size=" << frame_data.size() 
                  << ", expected=" << (video_settings_.width * video_settings_.height * 4) 
                  << ", capture time=" << capture_duration.count() << "μs"
                  << ", GPU optimized=" << (video_settings_.use_gpu_optimized_capture ? "yes" : "no") << std::endl;
        
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
    if (!midi_file_ || end_time < start_time) {
        return events;
    }

    for (int track_index = 0; track_index < midi_file_->header.numberOfTracks; ++track_index) {
        MidiTrack track_copy = midi_file_->tracks[track_index];
        MidiEvent event{};

        while (midi_read_next_event(&track_copy, &event)) {
            uint32_t absolute_tick = track_copy.currentTick;

            if ((event.eventType == MIDI_EVENT_NOTE_ON && event.data2 > 0) ||
                event.eventType == MIDI_EVENT_NOTE_OFF ||
                (event.eventType == MIDI_EVENT_NOTE_ON && event.data2 == 0)) {
                double time = CalculateElapsedTimeFromTick(absolute_tick);
                if (time >= start_time && time <= end_time) {
                    TimedMidiEvent timed_event{};
                    timed_event.event = event;
                    timed_event.tick = absolute_tick;
                    timed_event.time_seconds = time;
                    timed_event.processed = false;
                    events.push_back(timed_event);
                }
            }

            midi_free_event(&event);
            event = MidiEvent{};
        }
    }

    std::sort(events.begin(), events.end(), [](const TimedMidiEvent& a, const TimedMidiEvent& b) {
        if (a.time_seconds == b.time_seconds) {
            return a.tick < b.tick;
        }
        return a.time_seconds < b.time_seconds;
    });

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
    
    static int debug_count = 0;
    if (debug_count < 10) {
        std::cout << "ProcessMidiEvents: current_time=" << current_time
                  << "s, processed=" << processed_event_count_ << "/" << total_event_count_ << std::endl;
    }

    while (!pending_events_.empty()) {
        PendingEvent next = pending_events_.top();
        if (next.time_seconds > current_time + kTimeEpsilon) {
            break;
        }

        pending_events_.pop();

        if (next.track_index >= streaming_tracks_.size()) {
            continue;
        }

        auto& track_state = streaming_tracks_[next.track_index];
        if (!track_state.has_event || std::fabs(track_state.event_time - next.time_seconds) > kTimeEpsilon) {
            continue;
        }

        if (debug_count < 10) {
            std::cout << "  Event track=" << next.track_index
                      << ", tick=" << track_state.event_tick
                      << ", time=" << track_state.event_time << "s" << std::endl;
            debug_count++;
        }

        ProcessNoteEvent(track_state.current_event, track_state.event_time);
        midi_free_event(&track_state.current_event);
        track_state.current_event = MidiEvent{};
        track_state.has_event = false;
        processed_event_count_++;

        LoadNextTrackEvent(next.track_index);
    }
}

void MidiVideoOutput::ResetStreamingState() {
    ClearStreamingResources();

    if (!midi_file_) {
        return;
    }

    streaming_tracks_.resize(midi_file_->header.numberOfTracks);
    for (size_t i = 0; i < streaming_tracks_.size(); ++i) {
        auto& state = streaming_tracks_[i];
        state.track_state = midi_file_->tracks[i];
        state.current_event = MidiEvent{};
        state.has_event = false;
        state.event_tick = 0;
        state.event_time = 0.0;
    }

    processed_event_count_ = 0;
    pending_events_ = PendingEventQueue{};

    for (size_t i = 0; i < streaming_tracks_.size(); ++i) {
        LoadNextTrackEvent(i);
    }
}

void MidiVideoOutput::ClearStreamingResources() {
    for (auto& state : streaming_tracks_) {
        if (state.has_event) {
            midi_free_event(&state.current_event);
            state.current_event = MidiEvent{};
            state.has_event = false;
        }
    }
    streaming_tracks_.clear();
    pending_events_ = PendingEventQueue{};
}

bool MidiVideoOutput::LoadNextTrackEvent(size_t track_index) {
    if (!midi_file_ || track_index >= streaming_tracks_.size()) {
        return false;
    }

    auto& state = streaming_tracks_[track_index];
    if (state.has_event) {
        midi_free_event(&state.current_event);
        state.current_event = MidiEvent{};
        state.has_event = false;
    }

    MidiEvent event{};
    while (midi_read_next_event(&state.track_state, &event)) {
        uint32_t absolute_tick = state.track_state.currentTick;

        if (event.eventType == MIDI_EVENT_META && event.metaType == MIDI_META_SET_TEMPO && event.metaLength == 3) {
            uint32_t tempo = (event.metaData[0] << 16) | (event.metaData[1] << 8) | event.metaData[2];
            current_tempo_ = tempo;
        }

        if ((event.eventType == MIDI_EVENT_NOTE_ON && event.data2 > 0) ||
            event.eventType == MIDI_EVENT_NOTE_OFF ||
            (event.eventType == MIDI_EVENT_NOTE_ON && event.data2 == 0)) {
            state.current_event = event;
            state.has_event = true;
            state.event_tick = absolute_tick;
            state.event_time = CalculateElapsedTimeFromTick(absolute_tick);

            pending_events_.push({track_index, state.event_time, state.event_tick});
            return true;
        }

        midi_free_event(&event);
        event = MidiEvent{};
    }

    state.has_event = false;
    return false;
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
    if (!midi_file_ || total_event_count_ == 0) {
        return 0.0;
    }

    double duration = CalculateElapsedTimeFromTick(last_event_tick_);
    return duration + 2.0;
}

void MidiVideoOutput::BuildTempoMapAndStats() {
    if (!midi_file_) {
        return;
    }

    tempo_changes_.clear();
    total_note_count_ = 0;
    total_event_count_ = 0;
    processed_event_count_ = 0;
    last_event_tick_ = 0;

    current_tempo_ = 500000; // 120 BPM
    tempo_changes_.push_back({0, current_tempo_});

    for (int track_index = 0; track_index < midi_file_->header.numberOfTracks; ++track_index) {
        MidiTrack track_copy = midi_file_->tracks[track_index];
        MidiEvent event{};

        while (midi_read_next_event(&track_copy, &event)) {
            uint32_t absolute_tick = track_copy.currentTick;

            if (event.eventType == MIDI_EVENT_META && event.metaType == MIDI_META_SET_TEMPO && event.metaLength == 3) {
                uint32_t tempo = (event.metaData[0] << 16) | (event.metaData[1] << 8) | event.metaData[2];
                tempo_changes_.push_back({absolute_tick, tempo});
            }

            if ((event.eventType == MIDI_EVENT_NOTE_ON && event.data2 > 0) ||
                event.eventType == MIDI_EVENT_NOTE_OFF ||
                (event.eventType == MIDI_EVENT_NOTE_ON && event.data2 == 0)) {
                total_event_count_++;
                if (event.eventType == MIDI_EVENT_NOTE_ON && event.data2 > 0) {
                    total_note_count_++;
                }
                if (absolute_tick > last_event_tick_) {
                    last_event_tick_ = absolute_tick;
                }
            }

            midi_free_event(&event);
            event = MidiEvent{};
        }
    }

    std::sort(tempo_changes_.begin(), tempo_changes_.end(),
              [](const TempoChange& a, const TempoChange& b) {
                  return a.tick < b.tick;
              });

    if (!tempo_changes_.empty()) {
        current_tempo_ = tempo_changes_.front().tempo;
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
    
    // Use GPU-optimized PBO capture if enabled, otherwise fall back to standard method
    if (video_settings_.use_gpu_optimized_capture) {
        return renderer_->ReadFramebufferPBO(width, height);
    } else {
        return renderer_->ReadFramebuffer(width, height);
    }
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
            ImGui::Text("Events: %zu", total_event_count_);
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
        ImGui::Checkbox("GPU Optimized Capture", &video_settings_.use_gpu_optimized_capture);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Use PBO (Pixel Buffer Objects) for faster GPU-to-CPU data transfer");
        }
        
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

// コーデック固有の設定を取得するヘルパー関数
std::vector<std::string> MidiVideoOutput::GetCodecSpecificSettings(const std::string& codec) const {
    std::vector<std::string> settings;
    
    if (codec == "libx264") {
        // H.264 ソフトウェアエンコーダー
        settings.push_back("-preset");
        settings.push_back("ultrafast");
        settings.push_back("-tune");
        settings.push_back("zerolatency");
        settings.push_back("-crf");
        settings.push_back("23");
        settings.push_back("-threads");
        settings.push_back("0");
    } else if (codec == "libx265") {
        // H.265/HEVC ソフトウェアエンコーダー
        settings.push_back("-preset");
        settings.push_back("ultrafast");
        settings.push_back("-tune");
        settings.push_back("zerolatency");
        settings.push_back("-crf");
        settings.push_back("28"); // H.265は少し高い値でも同等品質
        settings.push_back("-threads");
        settings.push_back("0");
    } else if (codec == "h264_nvenc") {
        // NVIDIA NVENC H.264 ハードウェアエンコーダー
        settings.push_back("-preset");
        settings.push_back("p1"); // 最高速プリセット (NVENC用)
        settings.push_back("-tune");
        settings.push_back("ll"); // 低遅延 (NVENC用)
        settings.push_back("-rc");
        settings.push_back("cbr"); // CBR レート制御
        settings.push_back("-gpu");
        settings.push_back("0"); // GPU 0を使用
    } else if (codec == "hevc_nvenc") {
        // NVIDIA NVENC H.265/HEVC ハードウェアエンコーダー
        settings.push_back("-preset");
        settings.push_back("p1"); // 最高速プリセット
        settings.push_back("-tune");
        settings.push_back("ll"); // 低遅延
        settings.push_back("-rc");
        settings.push_back("cbr"); // CBR レート制御
        settings.push_back("-gpu");
        settings.push_back("0"); // GPU 0を使用
    } else if (codec == "h264_qsv") {
        // Intel Quick Sync Video H.264 ハードウェアエンコーダー
        settings.push_back("-preset");
        settings.push_back("veryfast");
        settings.push_back("-look_ahead");
        settings.push_back("0"); // 先読み無効
        settings.push_back("-global_quality");
        settings.push_back("23");
    } else if (codec == "hevc_qsv") {
        // Intel Quick Sync Video H.265/HEVC ハードウェアエンコーダー
        settings.push_back("-preset");
        settings.push_back("veryfast");
        settings.push_back("-look_ahead");
        settings.push_back("0");
        settings.push_back("-global_quality");
        settings.push_back("28");
    } else if (codec == "libvpx-vp9") {
        // VP9 ソフトウェアエンコーダー
        settings.push_back("-deadline");
        settings.push_back("realtime");
        settings.push_back("-cpu-used");
        settings.push_back("8"); // 最高速
        settings.push_back("-threads");
        settings.push_back("0");
    } else if (codec == "h264_amf") {
        // AMD AMF H.264 ハードウェアエンコーダー
        settings.push_back("-quality");
        settings.push_back("speed"); // 速度優先
        settings.push_back("-rc");
        settings.push_back("cbr");
    } else if (codec == "hevc_amf") {
        // AMD AMF H.265/HEVC ハードウェアエンコーダー
        settings.push_back("-quality");
        settings.push_back("speed");
        settings.push_back("-rc");
        settings.push_back("cbr");
    } else {
        // 未知のコーデック: 基本設定のみ
        std::cout << "Warning: Unknown codec '" << codec << "', using basic settings" << std::endl;
        settings.push_back("-threads");
        settings.push_back("0");
    }
    
    return settings;
}

// FFmpeg関連のメソッド実装
bool MidiVideoOutput::InitializeFFmpeg() {
    if (ffmpeg_process_) {
        FinalizeFFmpeg();
    }
    
    // コーデックに応じた設定を取得
    auto codec_settings = GetCodecSpecificSettings(video_settings_.video_codec);
    
    // FFmpegコマンドを構築
    std::stringstream cmd;
    cmd << "ffmpeg -y"; // -y: ファイルを上書き
    cmd << " -f rawvideo"; // 入力フォーマット: raw video
    cmd << " -pixel_format rgba"; // ピクセルフォーマット: RGBA
    cmd << " -video_size " << video_settings_.width << "x" << video_settings_.height; // 解像度
    cmd << " -framerate " << video_settings_.fps; // フレームレート
    cmd << " -i pipe:0"; // 標準入力から読み取り
    if (video_settings_.include_audio) {
        cmd << " -i \"" << video_settings_.audio_file_path << "\""; // 外部オーディオ入力
    }
    cmd << " -c:v " << video_settings_.video_codec; // ビデオコーデック: コマンドライン引数から設定
    
    // コーデック固有の設定を追加
    for (const auto& setting : codec_settings) {
        cmd << " " << setting;
    }
    
    cmd << " -b:v " << video_settings_.bitrate; // ビットレート
    cmd << " -maxrate " << video_settings_.bitrate; // 最大ビットレート
    cmd << " -bufsize " << (video_settings_.bitrate * 2); // バッファサイズ
    if (video_settings_.include_audio) {
        cmd << " -c:a aac";
        int kbps = std::max(1, video_settings_.audio_bitrate / 1000);
        cmd << " -b:a " << kbps << "k";
        cmd << " -shortest";
    }
    cmd << " -pix_fmt yuv420p"; // 出力ピクセルフォーマット
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

// デバッグ情報の更新
void MidiVideoOutput::UpdateDebugInfo() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - debug_info_.recording_start);
    
    debug_info_.elapsed_seconds = elapsed_duration.count() / 1000.0;
    debug_info_.current_frame_count = frame_count_;
    
    // FPS計算（過去1秒間の平均）
    if (debug_info_.elapsed_seconds > 0.0) {
        debug_info_.current_fps = frame_count_ / debug_info_.elapsed_seconds;
    }
    
    // 推定残り時間の更新
    if (current_time_ > 0.0 && debug_info_.elapsed_seconds > 0.0) {
        double progress_ratio = current_time_ / total_duration_;
        if (progress_ratio > 0.0) {
            debug_info_.estimated_total_duration = debug_info_.elapsed_seconds / progress_ratio;
        }
    }
}

// デバッグオーバーレイの描画
void MidiVideoOutput::RenderDebugOverlay() {
    if (!video_settings_.show_debug_info || !renderer_) {
        return;
    }
    
    // 現在時刻の文字列を生成
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time_t);
    
    char real_time_str[64];
    std::strftime(real_time_str, sizeof(real_time_str), "%Y/%m/%d %H:%M:%S", &tm);
    
    // 経過時間の計算
    int elapsed_days = static_cast<int>(debug_info_.elapsed_seconds) / 86400;
    int elapsed_hours = (static_cast<int>(debug_info_.elapsed_seconds) % 86400) / 3600;
    int elapsed_minutes = (static_cast<int>(debug_info_.elapsed_seconds) % 3600) / 60;
    int elapsed_seconds = static_cast<int>(debug_info_.elapsed_seconds) % 60;
    
    // ETA（推定残り時間）の計算
    double remaining_seconds = debug_info_.estimated_total_duration - debug_info_.elapsed_seconds;
    if (remaining_seconds < 0) remaining_seconds = 0;
    
    int eta_days = static_cast<int>(remaining_seconds) / 86400;
    int eta_hours = (static_cast<int>(remaining_seconds) % 86400) / 3600;
    int eta_minutes = (static_cast<int>(remaining_seconds) % 3600) / 60;
    int eta_secs = static_cast<int>(remaining_seconds) % 60;
    
    // デバッグ情報の文字列を構築
    std::ostringstream debug_text;
    debug_text << "RealTime: " << real_time_str << "\n";
    
    if (elapsed_days > 0) {
        debug_text << "Elapsed: " << elapsed_days << "d/" << elapsed_hours << ":" 
                   << std::setfill('0') << std::setw(2) << elapsed_minutes << ":" 
                   << std::setfill('0') << std::setw(2) << elapsed_seconds << "\n";
    } else {
        debug_text << "Elapsed: " << elapsed_hours << ":" 
                   << std::setfill('0') << std::setw(2) << elapsed_minutes << ":" 
                   << std::setfill('0') << std::setw(2) << elapsed_seconds << "\n";
    }
    
    if (eta_days > 0) {
        debug_text << "ETA: " << eta_days << "d/" << eta_hours << ":" 
                   << std::setfill('0') << std::setw(2) << eta_minutes << ":" 
                   << std::setfill('0') << std::setw(2) << eta_secs << "\n";
    } else {
        debug_text << "ETA: " << eta_hours << ":" 
                   << std::setfill('0') << std::setw(2) << eta_minutes << ":" 
                   << std::setfill('0') << std::setw(2) << eta_secs << "\n";
    }
    
    debug_text << "FrameCount: " << debug_info_.current_frame_count << "\n";
    
    // FPS と Speed の計算（60FPSを基準として速度倍率を算出）
    double target_fps = 60.0; // 標準フレームレート
    double speed_multiplier = debug_info_.current_fps / target_fps;
    
    debug_text << "FPS/Speed: " << std::fixed << std::setprecision(1) << debug_info_.current_fps 
               << "/" << std::setprecision(1) << speed_multiplier << "x";
    
    // デバッグ情報のレイアウト計算
    std::istringstream stream(debug_text.str());
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    
    // パネルの寸法計算
    float padding = 10.0f;
    float line_height = 24.0f;
    float panel_width = 380.0f;  // デバッグ情報用のパネル幅
    float panel_height = lines.size() * line_height + padding * 2;
    
    // 左下の位置（少し余白を取る）
    Vec2 panel_position(15.0f, video_settings_.height - panel_height - 15.0f);
    
    // 背景パネルを描画（半透明の黒）
    Color panel_bg_color(0.0f, 0.0f, 0.0f, 0.7f); // 半透明の黒
    renderer_->DrawRect(panel_position, Vec2(panel_width, panel_height), panel_bg_color);
    
    // フレームを描画（明るいグレー）
    Color frame_color(0.8f, 0.8f, 0.8f, 1.0f); // 明るいグレー
    float frame_thickness = 2.0f;
    
    // 上辺
    renderer_->DrawRect(Vec2(panel_position.x, panel_position.y), 
                       Vec2(panel_width, frame_thickness), frame_color);
    // 下辺
    renderer_->DrawRect(Vec2(panel_position.x, panel_position.y + panel_height - frame_thickness), 
                       Vec2(panel_width, frame_thickness), frame_color);
    // 左辺
    renderer_->DrawRect(Vec2(panel_position.x, panel_position.y), 
                       Vec2(frame_thickness, panel_height), frame_color);
    // 右辺
    renderer_->DrawRect(Vec2(panel_position.x + panel_width - frame_thickness, panel_position.y), 
                       Vec2(frame_thickness, panel_height), frame_color);
    
    // テキストを描画（背景パネルの中に）
    Color debug_color(1.0f, 1.0f, 1.0f, 1.0f); // 白色
    Vec2 text_position(panel_position.x + padding, panel_position.y + padding);
    
    for (size_t i = 0; i < lines.size(); i++) {
        Vec2 line_position(text_position.x, text_position.y + line_height * i);
        renderer_->DrawText(lines[i], line_position, debug_color, 2.0f); // フォントサイズを2倍に
    }
}