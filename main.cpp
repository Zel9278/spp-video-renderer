#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <memory>
#include <cstring>
#include <filesystem>

#include "opengl_renderer.h"
#include "piano_keyboard.h"
#include "midi_video_output.h"

// Video output constants (must match video settings)
constexpr int VIDEO_WIDTH = 1920;
constexpr int VIDEO_HEIGHT = 1080;
constexpr const char* WINDOW_TITLE = "OpenGL Piano Keyboard";

// Global variables
std::unique_ptr<OpenGLRenderer> g_renderer;
std::unique_ptr<PianoKeyboard> g_piano_keyboard;
std::unique_ptr<MidiVideoOutput> g_midi_video_output;

// GLFW callback functions
void error_callback(int error, const char* description);

// Command line options struct
struct CommandLineOptions {
    std::string midi_file;
    std::string video_codec = "libx264";  // Default to H.264
    bool debug_mode = false;  // Debug information overlay
};

// Parse command line arguments
CommandLineOptions ParseCommandLineArguments(int argc, char* argv[]) {
    CommandLineOptions options;
    
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [options] <midi_file>" << std::endl;
        std::cerr << "   or: " << argv[0] << " <midi_file> [options]" << std::endl;
        std::cerr << "Options:" << std::endl;
        std::cerr << "  --video-codec, -vc <codec>  Video codec for FFmpeg (default: libx264)" << std::endl;
        std::cerr << "  --debug, -d                 Show debug information overlay in video" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Supported codecs:" << std::endl;
        std::cerr << "  Software encoders:" << std::endl;
        std::cerr << "    libx264     - H.264 software encoder (default, widely compatible)" << std::endl;
        std::cerr << "    libx265     - H.265/HEVC software encoder (better compression)" << std::endl;
        std::cerr << "    libvpx-vp9  - VP9 software encoder (open source)" << std::endl;
        std::cerr << std::endl;
        std::cerr << "  Hardware encoders (require compatible hardware):" << std::endl;
        std::cerr << "    h264_nvenc  - NVIDIA NVENC H.264 (GeForce GTX 600+ / Quadro)" << std::endl;
        std::cerr << "    hevc_nvenc  - NVIDIA NVENC H.265/HEVC (GeForce GTX 900+ / Quadro)" << std::endl;
        std::cerr << "    h264_qsv    - Intel Quick Sync Video H.264 (Sandy Bridge+)" << std::endl;
        std::cerr << "    hevc_qsv    - Intel Quick Sync Video H.265/HEVC (Skylake+)" << std::endl;
        std::cerr << "    h264_amf    - AMD AMF H.264 (GCN+ / Polaris+)" << std::endl;
        std::cerr << "    hevc_amf    - AMD AMF H.265/HEVC (GCN+ / Polaris+)" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Examples:" << std::endl;
        std::cerr << "  " << argv[0] << " song.mid" << std::endl;
        std::cerr << "  " << argv[0] << " --video-codec h264_nvenc song.mid" << std::endl;
        std::cerr << "  " << argv[0] << " song.mid -vc libx265" << std::endl;
        std::cerr << "  " << argv[0] << " -d song.mid --video-codec hevc_nvenc" << std::endl;
        exit(-1);
    }
    
    bool midi_file_found = false;
    
    // Parse all arguments and classify them
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        // Check if this argument is an option (starts with - or --)
        if (arg.length() > 0 && arg[0] == '-') {
            if (arg == "--video-codec" || arg == "-vc") {
                if (i + 1 < argc) {
                    options.video_codec = argv[i + 1];
                    i++; // Skip the value argument
                } else {
                    std::cerr << "Error: " << arg << " requires a value" << std::endl;
                    exit(-1);
                }
            } else if (arg == "--debug" || arg == "-d") {
                options.debug_mode = true;
            } else if (arg == "--help" || arg == "-h") {
                // Show help and exit
                std::cerr << "Usage: " << argv[0] << " [options] <midi_file>" << std::endl;
                std::cerr << "   or: " << argv[0] << " <midi_file> [options]" << std::endl;
                std::cerr << "Options:" << std::endl;
                std::cerr << "  --video-codec, -vc <codec>  Video codec for FFmpeg (default: libx264)" << std::endl;
                std::cerr << "  --debug, -d                 Show debug information overlay in video" << std::endl;
                std::cerr << "  --help, -h                  Show this help message" << std::endl;
                exit(0);
            } else {
                std::cerr << "Error: Unknown option: " << arg << std::endl;
                exit(-1);
            }
        } else {
            // This argument doesn't start with '-', so it should be the MIDI file path
            if (!midi_file_found) {
                options.midi_file = arg;
                midi_file_found = true;
            } else {
                std::cerr << "Error: Multiple MIDI files specified. Only one MIDI file is allowed." << std::endl;
                std::cerr << "First file: " << options.midi_file << std::endl;
                std::cerr << "Second file: " << arg << std::endl;
                exit(-1);
            }
        }
    }
    
    // Check if MIDI file was provided
    if (!midi_file_found || options.midi_file.empty()) {
        std::cerr << "Error: No MIDI file specified." << std::endl;
        std::cerr << "Usage: " << argv[0] << " [options] <midi_file>" << std::endl;
        exit(-1);
    }
    
    return options;
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    CommandLineOptions options = ParseCommandLineArguments(argc, argv);
    
    std::cout << "Loading MIDI file: " << options.midi_file << std::endl;
    std::cout << "Video codec: " << options.video_codec << std::endl;
    std::cout << "Debug mode: " << (options.debug_mode ? "enabled" : "disabled") << std::endl;

    // Get the directory of the executable for output path
    std::filesystem::path exe_path(argv[0]);
    std::filesystem::path exe_dir = exe_path.parent_path();
    
    // Extract MIDI filename without extension for output naming
    std::filesystem::path midi_path(options.midi_file);
    std::string midi_name = midi_path.stem().string();
    
    // Create output path in the same directory as the executable
    std::filesystem::path output_path = exe_dir / (midi_name + "_output");
    
    std::cout << "Output will be saved to: " << output_path.string() << ".mp4" << std::endl;

    // Set GLFW error callback
    glfwSetErrorCallback(error_callback);
    
    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    // Configure GLFW for headless rendering (final version)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE); // Use compatibility profile for immediate mode
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); // Hide window for headless rendering
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE); // Don't focus window
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE); // Remove window decorations
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_FALSE); // Disable double buffering for headless

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // Create a hidden window for headless rendering (size must match video output)
    GLFWwindow* window = glfwCreateWindow(VIDEO_WIDTH, VIDEO_HEIGHT, "Piano Keyboard Video Renderer (Headless)", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    // Make the window's context current
    glfwMakeContextCurrent(window);

    // Initialize GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }
    
    std::cout << "OpenGL initialized successfully!" << std::endl;
    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;

    // Disable V-Sync for headless rendering (not needed)
    glfwSwapInterval(0);

    // Initialize OpenGL renderer
    std::cout << "Initializing OpenGL renderer..." << std::endl;
    g_renderer = std::make_unique<OpenGLRenderer>();
    g_renderer->Initialize(VIDEO_WIDTH, VIDEO_HEIGHT);
    std::cout << "OpenGL renderer initialized successfully!" << std::endl;

    // Initialize piano keyboard
    std::cout << "Initializing piano keyboard..." << std::endl;
    g_piano_keyboard = std::make_unique<PianoKeyboard>();
    g_piano_keyboard->Initialize();
    g_piano_keyboard->UpdateLayout(VIDEO_WIDTH, VIDEO_HEIGHT);
    std::cout << "Piano keyboard initialized successfully!" << std::endl;

    // Initialize MIDI video output
    std::cout << "Initializing MIDI video output..." << std::endl;
    g_midi_video_output = std::make_unique<MidiVideoOutput>();
    if (!g_midi_video_output->Initialize(g_piano_keyboard.get(), g_renderer.get())) {
        std::cerr << "Failed to initialize MIDI video output" << std::endl;
        return -1;
    }
    std::cout << "MIDI video output initialized successfully!" << std::endl;

    // Load MIDI file from command line argument
    std::cout << "Attempting to load MIDI file: " << options.midi_file << std::endl;
    if (!g_midi_video_output->LoadMidiFile(options.midi_file)) {
        std::cerr << "Failed to load MIDI file: " << options.midi_file << std::endl;
        std::cerr << "Please check if the file exists and is a valid MIDI file." << std::endl;
        return -1;
    }

    std::cout << "MIDI file loaded successfully!" << std::endl;
    std::cout << "OpenGL Piano Keyboard with MIDI Video Output initialized successfully!" << std::endl;
    std::cout << "Starting automatic video rendering..." << std::endl;

    // Configure video settings for high quality output
    auto video_settings = g_midi_video_output->GetVideoSettings();
    video_settings.width = 1920;
    video_settings.height = 1080;
    video_settings.fps = 60;
    video_settings.bitrate = 240000000; // 8 Mbps
    video_settings.output_path = output_path.string(); // Use the calculated output path
    video_settings.video_codec = options.video_codec; // Use command line specified codec
    video_settings.show_debug_info = options.debug_mode; // Enable debug overlay if requested
    std::cout << "Configuring video settings:" << std::endl;
    std::cout << "  Resolution: " << video_settings.width << "x" << video_settings.height << std::endl;
    std::cout << "  FPS: " << video_settings.fps << std::endl;
    std::cout << "  Bitrate: " << video_settings.bitrate << " bps" << std::endl;
    std::cout << "  Video codec: " << video_settings.video_codec << std::endl;
    std::cout << "  Debug overlay: " << (video_settings.show_debug_info ? "enabled" : "disabled") << std::endl;
    std::cout << "  Output path: " << video_settings.output_path << std::endl;
    g_midi_video_output->SetVideoSettings(video_settings);

    // Start recording video
    std::cout << "Starting video output..." << std::endl;
    if (!g_midi_video_output->StartVideoOutput(video_settings)) {
        std::cerr << "Failed to start video recording" << std::endl;
        return -1;
    }
    std::cout << "Video output started successfully!" << std::endl;

    // Start MIDI playback
    std::cout << "Starting MIDI playback..." << std::endl;
    g_midi_video_output->Play();
    std::cout << "MIDI playback started!" << std::endl;

    // Main render loop for headless video generation
    double lastFrameTime = glfwGetTime();
    std::cout << "Starting headless rendering..." << std::endl;
    
    int frame_counter = 0;
    int max_frames = static_cast<int>(g_midi_video_output->GetTotalDuration() * 60.0) + 60; // 安全マージン1秒
    std::cout << "Maximum expected frames: " << max_frames << std::endl;
    
    while (!glfwWindowShouldClose(window) && frame_counter < max_frames) {
        frame_counter++;
        
        // 定期的な進捗表示
        if (frame_counter % 1800 == 0) { // 30秒ごと (60fps * 30s)
            double progress = (double)frame_counter / max_frames * 100.0;
            std::cout << "Progress: " << progress << "% (Frame " << frame_counter << "/" << max_frames << ")" << std::endl;
        }
        
        // Only poll events minimally for headless operation
        glfwPollEvents();

        // Calculate delta time for consistent frame rate
        double currentFrameTime = glfwGetTime();
        double deltaTime = 1.0 / 60.0; // Fixed 60 FPS for consistent video output
        lastFrameTime = currentFrameTime;

        // Update piano keyboard
        g_piano_keyboard->Update();

        // Update MIDI video output
        g_midi_video_output->Update(deltaTime);

        // Check if MIDI playback is finished - more detailed checking
        bool is_playing = g_midi_video_output->IsPlaying();
        double current_time = g_midi_video_output->GetCurrentTime();
        double total_duration = g_midi_video_output->GetTotalDuration();
        
        // デバッグ: 最初の3フレームのみ
        if (frame_counter <= 3) {
            std::cout << "Frame " << frame_counter << " - Time: " << current_time 
                      << "s, Playing: " << (is_playing ? "true" : "false") << std::endl;
        }
        
        if (!is_playing && current_time > 0) {
            std::cout << "MIDI playback finished." << std::endl;
            std::cout << "  Current time: " << current_time << " seconds" << std::endl;
            std::cout << "  Total duration: " << total_duration << " seconds" << std::endl;
            std::cout << "  Is playing: " << (is_playing ? "true" : "false") << std::endl;
            std::cout << "Stopping recording..." << std::endl;
            g_midi_video_output->StopVideoOutput();
            std::cout << "Video saved to: " << output_path.string() << ".mp4" << std::endl;
            break;
        }

        // Render to offscreen framebuffer for video output
        g_renderer->BindOffscreenFramebuffer(); // ビデオ解像度のオフスクリーンFBOにバインド
        g_renderer->Clear(Color(0.1f, 0.1f, 0.1f, 1.0f)); // Dark gray background
        g_piano_keyboard->Render(*g_renderer);
        
        // デバッグ情報を描画 (デバッグモードが有効な場合)
        g_midi_video_output->RenderDebugOverlay();
        
        // Ensure all OpenGL commands are executed before frame capture
        glFlush();
        glFinish();
        
        // フレームバッファのバインドを解除（デフォルトフレームバッファに戻す）
        g_renderer->UnbindOffscreenFramebuffer();
    }

    // Cleanup
    g_midi_video_output.reset();
    g_piano_keyboard.reset();
    g_renderer.reset();

    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "Application closed successfully." << std::endl;
    return 0;
}

void error_callback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}