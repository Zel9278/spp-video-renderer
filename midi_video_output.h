#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include <functional>
#include <fstream>
#include <queue>
#include "midi_parser.h"
#include "piano_keyboard.h"
#include "opengl_renderer.h"

// Forward declarations
class PianoKeyboard;
class OpenGLRenderer;

// MIDIチャンネル用カラーパレット (16チャンネル分)
namespace MidiChannelColors {
    static const Color CHANNEL_COLORS[16] = {
        Color::FromHex(0x3366FF),  // チャンネル 0 - RGB(51,102,255)
        Color::FromHex(0xFF7E33),  // チャンネル 1 - RGB(255,126,51)
        Color::FromHex(0x33FF66),  // チャンネル 2 - RGB(51,255,102)
        Color::FromHex(0xFF3381),  // チャンネル 3 - RGB(255,51,129)
        Color::FromHex(0x33FFFF),  // チャンネル 4 - RGB(51,255,255)
        Color::FromHex(0xE433FF),  // チャンネル 5 - RGB(228,51,255)
        Color::FromHex(0x99FF33),  // チャンネル 6 - RGB(153,255,51)
        Color::FromHex(0x4B33FF),  // チャンネル 7 - RGB(75,51,255)
        Color::FromHex(0xFFCC33),  // チャンネル 8 - RGB(255,204,51)
        Color::FromHex(0x33B4FF),  // チャンネル 9 - RGB(51,180,255)
        Color::FromHex(0xFF3333),  // チャンネル 10 - RGB(255,51,51)
        Color::FromHex(0x33FFB1),  // チャンネル 11 - RGB(51,255,177)
        Color::FromHex(0xFF33CC),  // チャンネル 12 - RGB(255,51,204)
        Color::FromHex(0x4EFF33),  // チャンネル 13 - RGB(78,255,51)
        Color::FromHex(0x9933FF),  // チャンネル 14 - RGB(153,51,255)
        Color::FromHex(0xE7FF33),  // チャンネル 15 - RGB(231,255,51)
    };
    
    // チャンネル番号から色を取得
    inline Color GetChannelColor(uint8_t channel) {
        return CHANNEL_COLORS[channel & 0x0F]; // 0-15の範囲に制限
    }
}

// MIDI再生状態
enum class MidiPlaybackState {
    Stopped,
    Playing,
    Paused,
    Recording  // 動画出力中
};

// 動画出力設定
struct VideoOutputSettings {
    std::string output_path = "output_video";
    int fps = 60;                    // フレームレート
    int width = 1920;               // 動画幅
    int height = 1080;              // 動画高さ
    int bitrate = 8000000;          // ビットレート (8000 kbps = 8 Mbps)
    bool use_cbr = true;            // CBR (true) or VBR (false)
    bool save_frames = false;       // 個別フレームを保存するか（FFmpegを使用する場合は不要）
    std::string frame_format = "png"; // フレーム形式 (png, jpg, bmp)
    std::string video_codec = "h264"; // 動画コーデック

    enum class ColorMode {
        Channel,
        Track,
        Both
    };

    ColorMode color_mode = ColorMode::Channel;
    
    // 再生設定
    float playback_speed = 1.0f;    // 再生速度倍率
    float key_press_duration = 0.1f; // キー押下継続時間（秒）
    
    // 視覚効果設定
    bool show_rainbow_effects = true;  // カラーブリップエフェクト（MIDIチャンネル色）
    bool show_key_blips = true;
    float blip_intensity = 1.0f;
    
    // GPU最適化設定
    bool use_gpu_optimized_capture = true;  // PBO使用でGPU最適化フレームキャプチャ
    
    // デバッグ情報設定
    bool show_debug_info = false;  // 動画内にデバッグ情報を表示
    
    // オーディオ設定（将来の拡張用）
    bool include_audio = false;
    std::string audio_file_path;
    int audio_bitrate = 192000; // 192 kbps
    
    // FFmpeg executable path (empty = use default "ffmpeg" from PATH)
    std::string ffmpeg_executable_path;
};

// MIDIイベントとタイミング情報
struct TimedMidiEvent {
    MidiEvent event;
    double time_seconds;     // 絶対時間（秒）
    uint32_t tick;          // MIDI ティック
    bool processed;         // このイベントが処理されたか
};

struct TempoChange {
    uint32_t tick;          // 変更が発生するティック
    uint32_t tempo;         // マイクロ秒/四分音符
};

struct StreamingTrackState {
    MidiTrack track_state{};
    MidiEvent current_event{};
    bool has_event{false};
    uint32_t event_tick{0};
    double event_time{0.0};
};

struct PendingEvent {
    size_t track_index{0};
    double time_seconds{0.0};
    uint32_t tick{0};
};

struct PendingEventCompare {
    bool operator()(const PendingEvent& lhs, const PendingEvent& rhs) const {
        if (lhs.time_seconds == rhs.time_seconds) {
            return lhs.tick > rhs.tick;
        }
        return lhs.time_seconds > rhs.time_seconds;
    }
};

// デバッグ情報構造体
struct DebugInfo {
    std::chrono::system_clock::time_point start_time;  // 録画開始時刻
    std::chrono::steady_clock::time_point recording_start;  // 録画開始タイマー
    double elapsed_seconds;  // 経過時間（秒）
    double estimated_total_duration;  // 推定総時間（秒）
    int current_frame_count;  // 現在のフレーム数
    double current_fps;  // 現在のFPS
    
    DebugInfo() : elapsed_seconds(0.0), estimated_total_duration(0.0), 
                  current_frame_count(0), current_fps(0.0) {}
};

// MIDI動画出力クラス
class MidiVideoOutput {
public:
    MidiVideoOutput();
    ~MidiVideoOutput();

    // 初期化
    bool Initialize(PianoKeyboard* piano_keyboard, OpenGLRenderer* renderer);
    void Cleanup();

    // MIDIファイル操作
    bool LoadMidiFile(const std::string& filepath);
    void UnloadMidiFile();
    bool IsMidiLoaded() const;
    
    // 便利なエイリアス
    bool LoadMidi(const std::string& filepath) { return LoadMidiFile(filepath); }
    
    // 再生制御
    void Play();
    void Pause();
    void Stop();
    void Seek(double time_seconds);
    bool IsPlaying() const { return playback_state_ == MidiPlaybackState::Playing; }
    
    // 動画出力
    bool StartVideoOutput(const VideoOutputSettings& settings);
    void StopVideoOutput();
    bool IsRecording() const;
    
    // 便利なエイリアス
    bool StartRecording(const std::string& output_path) {
        VideoOutputSettings settings = video_settings_;
        settings.output_path = output_path;
        return StartVideoOutput(settings);
    }
    void StopRecording() { StopVideoOutput(); }
    
    // 更新とレンダリング
    void Update(double delta_time);
    bool CaptureFrame(); // 現在のフレームをキャプチャ
    
    // 状態取得
    MidiPlaybackState GetPlaybackState() const;
    double GetCurrentTime() const;
    double GetTotalDuration() const;
    float GetProgress() const; // 0.0 - 1.0
    
    // 設定
    VideoOutputSettings& GetVideoSettings();
    const VideoOutputSettings& GetVideoSettings() const;
    void SetVideoSettings(const VideoOutputSettings& settings);
    
    // MIDI情報取得
    const MidiFile* GetMidiFile() const;
    std::vector<TimedMidiEvent> GetEventsInRange(double start_time, double end_time) const;
    int GetTotalNoteCount() const;
    int GetActiveNoteCount() const;
    
    // ImGui UI
    void RenderMidiControls();
    void RenderVideoOutputUI();
    void RenderDebugOverlay();  // デバッグ情報の描画（公開メソッド）
    
    // コールバック設定
    void SetProgressCallback(std::function<void(float)> callback);
    void SetFrameCapturedCallback(std::function<void(int)> callback);

private:
    using PendingEventQueue = std::priority_queue<PendingEvent, std::vector<PendingEvent>, PendingEventCompare>;
    static constexpr double kTimeEpsilon = 1e-6;

    // 内部状態
    MidiPlaybackState playback_state_;
    std::unique_ptr<MidiFile> midi_file_;
    // ストリーミング再生用のトラック状態と次イベント待機キュー
    std::vector<StreamingTrackState> streaming_tracks_;
    PendingEventQueue pending_events_;
    
    // タイミング管理
    double current_time_;
    double total_duration_;
    std::chrono::steady_clock::time_point playback_start_time_;
    std::chrono::steady_clock::time_point pause_time_;
    double pause_duration_;
    
    // フレームベース管理
    int current_frame_;
    double frame_time_; // 1/60.0 = 0.016666...
    
    // 動画出力
    VideoOutputSettings video_settings_;
    bool is_recording_;
    int frame_count_;
    std::string output_directory_;
    
    // FFmpeg関連
    FILE* ffmpeg_process_;
    std::string output_video_path_;
    
    // 外部参照
    PianoKeyboard* piano_keyboard_;
    OpenGLRenderer* renderer_;
    
    // アクティブノート管理
    std::vector<bool> active_notes_; // 128要素、各MIDIノートの状態
    std::vector<std::chrono::steady_clock::time_point> note_press_times_; // ノート押下時刻
    
    // コールバック
    std::function<void(float)> progress_callback_;
    std::function<void(int)> frame_captured_callback_;
    
    // UI状態
    bool show_midi_controls_;
    bool show_video_output_ui_;
    char midi_file_path_[512];
    char video_output_path_[512];
    
    // 内部メソッド
    void ProcessMidiEvents(double current_time);
    void ProcessNoteEvent(const MidiEvent& event, double event_time, size_t track_index);
    void UpdateActiveNotes(double current_time);
    void ResetStreamingState();
    void ClearStreamingResources();
    bool LoadNextTrackEvent(size_t track_index);
    double CalculateTotalDuration();
    void BuildTempoMapAndStats();
    double TicksToSeconds(uint32_t ticks, uint32_t division, uint32_t tempo) const;
    double CalculateElapsedTimeFromTick(uint32_t targetTick) const;  // midiplayer-base式改良計算
    bool SaveFrameToFile(const std::string& filepath);
    std::vector<uint8_t> CaptureFramebuffer();
    void CreateOutputDirectory();
    
    // FFmpeg関連メソッド
    bool InitializeFFmpeg();
    void FinalizeFFmpeg();
    bool WriteFrameToFFmpeg(const std::vector<uint8_t>& frame_data);
    std::vector<std::string> GetCodecSpecificSettings(const std::string& codec, bool use_cbr) const;
    Color DetermineBlipColor(uint8_t channel, size_t track_index) const;
    
    // テンポ管理
    uint32_t current_tempo_; // マイクロ秒/四分音符
    std::vector<TempoChange> tempo_changes_; // テンポ変更のリスト
    
    // デバッグ・統計
    int total_note_count_;
    int processed_event_count_;
    size_t total_event_count_;
    uint32_t last_event_tick_;
    DebugInfo debug_info_;  // デバッグ情報
    
    // ヘルパー関数
    static std::string GetTimestampString();
    static bool CreateDirectoryRecursive(const std::string& path);
    void UpdateDebugInfo();  // デバッグ情報の更新
};

// MIDIトラック用カラーパレット
namespace MidiTrackColors {
    static const Color TRACK_COLORS[16] = {
        Color::FromHex(0xFF5733),  // Track 0
        Color::FromHex(0x33FF57),
        Color::FromHex(0x3357FF),
        Color::FromHex(0xFF33A8),
        Color::FromHex(0x33FFF3),
        Color::FromHex(0xFFC133),
        Color::FromHex(0x9D33FF),
        Color::FromHex(0xFF8333),
        Color::FromHex(0x33FF9D),
        Color::FromHex(0x3383FF),
        Color::FromHex(0xFF33D4),
        Color::FromHex(0x33FFD4),
        Color::FromHex(0xFFD633),
        Color::FromHex(0x7B33FF),
        Color::FromHex(0xFF3333),
        Color::FromHex(0x33FF33),
    };

    inline Color GetTrackColor(size_t track_index) {
        constexpr size_t count = sizeof(TRACK_COLORS) / sizeof(TRACK_COLORS[0]);
        if (count == 0) {
            return Color::FromHex(0xFFFFFF);
        }
        return TRACK_COLORS[track_index % count];
    }
}