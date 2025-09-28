#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef DrawText
#undef DrawText
#endif
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#endif

#include <glad/glad.h>
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <iostream>
#include <memory>
#include <cstring>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <vector>
#include <stdexcept>
#include <limits>
#include <cstdlib>
#include <cstddef>

#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#include "opengl_renderer.h"
#ifdef _WIN32
#include "directx12_renderer.h"
#endif
#include "piano_keyboard.h"
#include "midi_video_output.h"

#include "resources/window_icon_loader.h"

static const unsigned char kWindowIconPng[] = {
#include "icon.png.h"
};
static constexpr std::size_t kWindowIconPngSize = sizeof(kWindowIconPng);

// Default video output resolution
constexpr int DEFAULT_VIDEO_WIDTH = 1920;
constexpr int DEFAULT_VIDEO_HEIGHT = 1080;
constexpr int PREVIEW_WIDTH = 1280;
constexpr int PREVIEW_HEIGHT = 720;
constexpr const char* WINDOW_TITLE = "OpenGL Piano Keyboard";

enum class RendererType {
    OpenGL,
    DirectX12
};

static void SetFallbackWindowIcon(GLFWwindow* window) {
    // Create a simple 32x32 piano-themed icon
    const int size = 32;
    unsigned char* pixels = new unsigned char[size * size * 4];
    
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int idx = (y * size + x) * 4;
            
            // Create a piano keyboard pattern
            bool is_black_key_area = (y < size * 0.6); // Top 60% for black keys
            bool is_black_key = is_black_key_area && ((x / 3) % 2 == 1);
            
            if (is_black_key) {
                // Black keys
                pixels[idx + 0] = 30;   // R
                pixels[idx + 1] = 30;   // G
                pixels[idx + 2] = 30;   // B
            } else {
                // White keys and background
                pixels[idx + 0] = 250;  // R
                pixels[idx + 1] = 250;  // G
                pixels[idx + 2] = 250;  // B
            }
            pixels[idx + 3] = 255;      // A (fully opaque)
        }
    }
    
    GLFWimage icon;
    icon.width = size;
    icon.height = size;
    icon.pixels = pixels;
    
    glfwSetWindowIcon(window, 1, &icon);
    delete[] pixels;
}

static void SetWindowIcon(GLFWwindow* window) {
    if (!window_icon::SetWindowIconFromPng(window, kWindowIconPng, kWindowIconPngSize)) {
        SetFallbackWindowIcon(window);
    }
}

static std::string FormatTime(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }

    int total_seconds = static_cast<int>(seconds + 0.5);
    int hours = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int secs = total_seconds % 60;

    std::ostringstream oss;
    oss << std::setfill('0');
    if (hours > 0) {
        oss << hours << ":" << std::setw(2) << minutes << ":" << std::setw(2) << secs;
    } else {
        oss << minutes << ":" << std::setw(2) << secs;
    }
    return oss.str();
}

static const char* ColorModeToString(VideoOutputSettings::ColorMode mode) {
    switch (mode) {
        case VideoOutputSettings::ColorMode::Channel:
            return "channel";
        case VideoOutputSettings::ColorMode::Track:
            return "track";
        case VideoOutputSettings::ColorMode::Both:
            return "both";
    }
    return "channel";
}

// Global variables
std::unique_ptr<RendererBackend> g_renderer;
OpenGLRenderer* g_opengl_renderer = nullptr;
#ifdef _WIN32
DirectX12Renderer* g_directx_renderer = nullptr;
#endif
std::unique_ptr<PianoKeyboard> g_piano_keyboard;
std::unique_ptr<MidiVideoOutput> g_midi_video_output;

// GLFW callback functions
void error_callback(int error, const char* description);

// Command line options struct
static int ParseBitrateOption(const std::string& input) {
    std::string compact;
    compact.reserve(input.size());
    for (char ch : input) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            compact.push_back(ch);
        }
    }

    if (compact.empty()) {
        throw std::invalid_argument("Bitrate value is empty");
    }

    std::string lowercase;
    lowercase.reserve(compact.size());
    for (char ch : compact) {
        lowercase.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    auto strip_suffix = [&](const std::string& suffix) {
        if (lowercase.size() >= suffix.size() &&
            lowercase.compare(lowercase.size() - suffix.size(), suffix.size(), suffix) == 0) {
            lowercase.erase(lowercase.size() - suffix.size());
            compact.erase(compact.size() - suffix.size());
            return true;
        }
        return false;
    };

    double multiplier = 1.0;
    if (strip_suffix("kbps")) {
        multiplier = 1000.0;
    } else if (strip_suffix("mbps")) {
        multiplier = 1000000.0;
    } else if (strip_suffix("gbps")) {
        multiplier = 1000000000.0;
    } else if (!compact.empty()) {
        char last = static_cast<char>(std::tolower(static_cast<unsigned char>(compact.back())));
        if (last == 'k') {
            compact.pop_back();
            lowercase.pop_back();
            multiplier = 1000.0;
        } else if (last == 'm') {
            compact.pop_back();
            lowercase.pop_back();
            multiplier = 1000000.0;
        } else if (last == 'g') {
            compact.pop_back();
            lowercase.pop_back();
            multiplier = 1000000000.0;
        }
    }

    if (compact.empty()) {
        throw std::invalid_argument("Bitrate value has no numeric component");
    }

    double numeric = 0.0;
    try {
        numeric = std::stod(compact);
    } catch (const std::exception&) {
        throw std::invalid_argument("Failed to parse bitrate numeric value");
    }

    double result = numeric * multiplier;
    if (result <= 0.0) {
        throw std::invalid_argument("Bitrate must be positive");
    }

    double max_int = static_cast<double>(std::numeric_limits<int>::max());
    if (result > max_int) {
        throw std::out_of_range("Bitrate value exceeds supported range");
    }

    return static_cast<int>(std::llround(result));
}

// Command line options struct
struct CommandLineOptions {
    std::string midi_file;
    std::string video_codec = "libx264";  // Default to H.264
    bool debug_mode = false;  // Debug information overlay
    std::string audio_file;
    bool show_preview = false;
    int video_width = DEFAULT_VIDEO_WIDTH;
    int video_height = DEFAULT_VIDEO_HEIGHT;
    bool use_cbr = true;
    int video_bitrate = 240000000;
    VideoOutputSettings::ColorMode color_mode = VideoOutputSettings::ColorMode::Channel;
    std::string ffmpeg_path;  // Custom FFmpeg executable path
    std::string output_directory;  // Custom output directory
    std::string renderer = "opengl"; // Rendering backend: opengl or dx12 (Windows only)
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
        std::cerr << "  --audio-file, -af <path>    External audio file to mux with the render" << std::endl;
        std::cerr << "  --resolution, -r <WxH>      Set video resolution (default: 1920x1080)" << std::endl;
        std::cerr << "  --bitrate, -br <value>     Set video bitrate (accepts suffixes like 20M, 5000k, 25mbps)" << std::endl;
        std::cerr << "  --cbr                       Force constant bitrate encoding" << std::endl;
        std::cerr << "  --vbr, --no-cbr             Use variable bitrate encoding" << std::endl;
        std::cerr << "  --show-preview, -sp         Display a 1280x720 preview window" << std::endl;
        std::cerr << "  --color-mode, -cm <mode>    Blip color mode: channel, track, both" << std::endl;
        std::cerr << "  --ffmpeg-path, -fp <path>   Path to FFmpeg executable (default: system PATH)" << std::endl;
        std::cerr << "  --output-directory, -o <path> Output directory for video files (default: executable dir)" << std::endl;
        std::cerr << "  --renderer, -rdr <backend>  Rendering backend: opengl (default) or dx12 (Windows)" << std::endl;
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
        std::cerr << "  " << argv[0] << " song.mid -r 2560x1440" << std::endl;
        std::cerr << "  " << argv[0] << " song.mid --bitrate 40M --vbr" << std::endl;
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
            } else if (arg == "--resolution" || arg == "-r") {
                if (i + 1 < argc) {
                    std::string value = argv[i + 1];
                    auto x_pos = value.find('x');
                    if (x_pos == std::string::npos) {
                        x_pos = value.find('X');
                    }
                    if (x_pos == std::string::npos) {
                        std::cerr << "Error: Resolution must be in <width>x<height> format (e.g., 1920x1080)" << std::endl;
                        exit(-1);
                    }

                    std::string width_str = value.substr(0, x_pos);
                    std::string height_str = value.substr(x_pos + 1);

                    try {
                        int width = std::stoi(width_str);
                        int height = std::stoi(height_str);
                        if (width <= 0 || height <= 0) {
                            throw std::invalid_argument("Resolution dimensions must be positive");
                        }
                        options.video_width = width;
                        options.video_height = height;
                    } catch (const std::exception& e) {
                        std::cerr << "Error: Invalid resolution '" << value << "': " << e.what() << std::endl;
                        exit(-1);
                    }
                    i++;
                } else {
                    std::cerr << "Error: " << arg << " requires a value" << std::endl;
                    exit(-1);
                }
            } else if (arg == "--audio-file" || arg == "-af") {
                if (i + 1 < argc) {
                    options.audio_file = argv[i + 1];
                    i++;
                } else {
                    std::cerr << "Error: " << arg << " requires a file path" << std::endl;
                    exit(-1);
                }
            } else if (arg == "--bitrate" || arg == "-br") {
                if (i + 1 < argc) {
                    std::string value = argv[i + 1];
                    try {
                        options.video_bitrate = ParseBitrateOption(value);
                    } catch (const std::exception& e) {
                        std::cerr << "Error: Invalid bitrate '" << value << "': " << e.what() << std::endl;
                        exit(-1);
                    }
                    i++;
                } else {
                    std::cerr << "Error: " << arg << " requires a value" << std::endl;
                    exit(-1);
                }
            } else if (arg == "--cbr") {
                options.use_cbr = true;
            } else if (arg == "--vbr" || arg == "--no-cbr") {
                options.use_cbr = false;
            } else if (arg == "--debug" || arg == "-d") {
                options.debug_mode = true;
            } else if (arg == "--show-preview" || arg == "-sp") {
                options.show_preview = true;
            } else if (arg == "--color-mode" || arg == "-cm") {
                if (i + 1 < argc) {
                    std::string value = argv[i + 1];
                    std::string lowercase;
                    lowercase.reserve(value.size());
                    for (char ch : value) {
                        lowercase.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                    }

                    if (lowercase == "channel") {
                        options.color_mode = VideoOutputSettings::ColorMode::Channel;
                    } else if (lowercase == "track") {
                        options.color_mode = VideoOutputSettings::ColorMode::Track;
                    } else if (lowercase == "both") {
                        options.color_mode = VideoOutputSettings::ColorMode::Both;
                    } else {
                        std::cerr << "Error: Invalid color mode '" << value << "'. Supported values are channel, track, both." << std::endl;
                        exit(-1);
                    }
                    i++;
                } else {
                    std::cerr << "Error: " << arg << " requires a value" << std::endl;
                    exit(-1);
                }
            } else if (arg == "--ffmpeg-path" || arg == "-fp") {
                if (i + 1 < argc) {
                    options.ffmpeg_path = argv[i + 1];
                    i++;
                } else {
                    std::cerr << "Error: " << arg << " requires a path" << std::endl;
                    exit(-1);
                }
            } else if (arg == "--output-directory" || arg == "-o") {
                if (i + 1 < argc) {
                    options.output_directory = argv[i + 1];
                    i++;
                } else {
                    std::cerr << "Error: " << arg << " requires a path" << std::endl;
                    exit(-1);
                }
            } else if (arg == "--renderer" || arg == "-rdr") {
                if (i + 1 < argc) {
                    options.renderer = argv[i + 1];
                    i++;
                } else {
                    std::cerr << "Error: " << arg << " requires a value (opengl or dx12)" << std::endl;
                    exit(-1);
                }
            } else if (arg == "--help" || arg == "-h") {
                // Show help and exit
                std::cerr << "Usage: " << argv[0] << " [options] <midi_file>" << std::endl;
                std::cerr << "   or: " << argv[0] << " <midi_file> [options]" << std::endl;
                std::cerr << "Options:" << std::endl;
                std::cerr << "  --video-codec, -vc <codec>  Video codec for FFmpeg (default: libx264)" << std::endl;
                std::cerr << "  --debug, -d                 Show debug information overlay in video" << std::endl;
                std::cerr << "  --audio-file, -af <path>    External audio file to mux with the render" << std::endl;
                std::cerr << "  --resolution, -r <WxH>      Set video resolution (default: 1920x1080)" << std::endl;
                std::cerr << "  --bitrate, -br <value>     Set video bitrate (accepts suffixes like 20M, 5000k, 25mbps)" << std::endl;
                std::cerr << "  --cbr                       Force constant bitrate encoding" << std::endl;
                std::cerr << "  --vbr, --no-cbr             Use variable bitrate encoding" << std::endl;
                std::cerr << "  --show-preview, -sp         Display a 1280x720 preview window" << std::endl;
                std::cerr << "  --color-mode, -cm <mode>    Blip color mode: channel, track, both" << std::endl;
                std::cerr << "  --ffmpeg-path, -fp <path>   Path to FFmpeg executable (default: system PATH)" << std::endl;
                std::cerr << "  --output-directory, -o <path> Output directory for video files (default: executable dir)" << std::endl;
                std::cerr << "  --renderer, -rdr <backend>  Rendering backend: opengl (default) or dx12 (Windows)" << std::endl;
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

    std::string renderer_lower;
    renderer_lower.reserve(options.renderer.size());
    for (char ch : options.renderer) {
        renderer_lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    RendererType renderer_type = RendererType::OpenGL;
    if (renderer_lower == "dx12" || renderer_lower == "directx" || renderer_lower == "directx12") {
#ifdef _WIN32
        renderer_type = RendererType::DirectX12;
#else
        std::cerr << "Error: DirectX 12 renderer is only available on Windows." << std::endl;
        return -1;
#endif
    } else if (!renderer_lower.empty() && renderer_lower != "opengl") {
        std::cerr << "Warning: Unknown renderer '" << options.renderer << "'. Falling back to OpenGL." << std::endl;
    }
    
    std::cout << "Loading MIDI file: " << options.midi_file << std::endl;
    std::cout << "Video codec: " << options.video_codec << std::endl;
    std::cout << "Debug mode: " << (options.debug_mode ? "enabled" : "disabled") << std::endl;
    std::cout << "Preview window: " << (options.show_preview ? "enabled (1280x720)" : "disabled") << std::endl;
    std::cout << "Video resolution: " << options.video_width << "x" << options.video_height << std::endl;
    std::cout << "Rate control: " << (options.use_cbr ? "CBR" : "VBR") << std::endl;
    std::cout << "Target bitrate: " << options.video_bitrate << " bps" << std::endl;
    std::cout << "Blip color mode: " << ColorModeToString(options.color_mode) << std::endl;
    std::cout << "FFmpeg path: " << (options.ffmpeg_path.empty() ? "(system default)" : options.ffmpeg_path) << std::endl;
    std::cout << "Output directory: " << (options.output_directory.empty() ? "(executable directory)" : options.output_directory) << std::endl;

    // Determine output directory
    std::filesystem::path output_dir;
    if (!options.output_directory.empty()) {
        output_dir = options.output_directory;
    } else {
        // Default to executable directory
        std::filesystem::path exe_path(argv[0]);
        output_dir = exe_path.parent_path();
    }
    
    // Extract MIDI filename without extension for output naming
    std::filesystem::path midi_path(options.midi_file);
    std::string midi_name = midi_path.stem().string();
    
    // Create output path
    std::filesystem::path output_path = output_dir / (midi_name + "_output");
    
    std::cout << "Output will be saved to: " << output_path.string() << ".mp4" << std::endl;

    // Set GLFW error callback
    glfwSetErrorCallback(error_callback);
    
    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }

    const int video_width = options.video_width;
    const int video_height = options.video_height;

    GLFWwindow* window = nullptr;
    GLFWwindow* preview_window = nullptr;
    g_opengl_renderer = nullptr;
#ifdef _WIN32
    g_directx_renderer = nullptr;
#endif
    if (renderer_type == RendererType::OpenGL) {
        glfwDefaultWindowHints();
        // Configure GLFW for hidden OpenGL context
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_FALSE);

#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

        window = glfwCreateWindow(video_width, video_height, "Piano Keyboard Video Renderer (OpenGL)", nullptr, nullptr);
        if (!window) {
            std::cerr << "Failed to create GLFW window" << std::endl;
            glfwTerminate();
            return -1;
        }

        SetWindowIcon(window);
        glfwMakeContextCurrent(window);

        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            std::cerr << "Failed to initialize GLAD" << std::endl;
            glfwDestroyWindow(window);
            glfwTerminate();
            return -1;
        }

        std::cout << "OpenGL initialized successfully!" << std::endl;
        std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;

        if (options.show_preview) {
            glfwDefaultWindowHints();
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
            glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
            glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);

            preview_window = glfwCreateWindow(PREVIEW_WIDTH, PREVIEW_HEIGHT, "Rendering Preview", nullptr, window);
            if (preview_window) {
                SetWindowIcon(preview_window);
                glfwMakeContextCurrent(preview_window);
                gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
                glfwSwapInterval(1);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glfwSwapBuffers(preview_window);
                std::cout << "Preview window created successfully." << std::endl;
            } else {
                std::cerr << "Warning: Failed to create preview window." << std::endl;
            }
        }

        glfwMakeContextCurrent(window);
        glfwSwapInterval(0);

        std::cout << "Initializing OpenGL renderer..." << std::endl;
        auto opengl_renderer = std::make_unique<OpenGLRenderer>();
        g_opengl_renderer = opengl_renderer.get();
        opengl_renderer->Initialize(video_width, video_height);
        g_renderer = std::move(opengl_renderer);
        std::cout << "OpenGL renderer initialized successfully!" << std::endl;
    } else {
#ifdef _WIN32
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

        window = glfwCreateWindow(video_width, video_height, "Piano Keyboard Video Renderer (DirectX 12)", nullptr, nullptr);
        if (!window) {
            std::cerr << "Failed to create headless window for DirectX renderer" << std::endl;
            glfwTerminate();
            return -1;
        }

        SetWindowIcon(window);

        if (options.show_preview) {
            std::cout << "Preview window is currently unavailable for the DirectX 12 backend. Rendering will continue headless." << std::endl;
        }

        std::cout << "Initializing DirectX 12 renderer..." << std::endl;
        auto dx_renderer = std::make_unique<DirectX12Renderer>();
        g_directx_renderer = dx_renderer.get();
        dx_renderer->Initialize(video_width, video_height);
        g_renderer = std::move(dx_renderer);
        std::cout << "DirectX 12 renderer initialized successfully!" << std::endl;
#else
        (void)window;
#endif
    }

    // Initialize piano keyboard
    std::cout << "Initializing piano keyboard..." << std::endl;
    g_piano_keyboard = std::make_unique<PianoKeyboard>();
    g_piano_keyboard->Initialize();
    g_piano_keyboard->UpdateLayout(video_width, video_height);
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

    const char* renderer_label = (renderer_type == RendererType::OpenGL) ? "OpenGL" : "DirectX 12";
    std::cout << "MIDI file loaded successfully!" << std::endl;
    std::cout << renderer_label << " Piano Keyboard with MIDI Video Output initialized successfully!" << std::endl;
    std::cout << "Starting automatic video rendering..." << std::endl;

    // Configure video settings for high quality output
    auto video_settings = g_midi_video_output->GetVideoSettings();
    video_settings.width = video_width;
    video_settings.height = video_height;
    video_settings.fps = 60;
    video_settings.bitrate = options.video_bitrate;
    video_settings.use_cbr = options.use_cbr;
    video_settings.output_path = output_path.string(); // Use the calculated output path
    video_settings.video_codec = options.video_codec; // Use command line specified codec
    video_settings.show_debug_info = options.debug_mode; // Enable debug overlay if requested
    video_settings.color_mode = options.color_mode;
    video_settings.ffmpeg_executable_path = options.ffmpeg_path; // Set custom FFmpeg path if specified
    if (!options.audio_file.empty()) {
        video_settings.include_audio = true;
        video_settings.audio_file_path = options.audio_file;
    }
    std::cout << "Configuring video settings:" << std::endl;
    std::cout << "  Resolution: " << video_settings.width << "x" << video_settings.height << std::endl;
    std::cout << "  FPS: " << video_settings.fps << std::endl;
    std::cout << "  Bitrate: " << video_settings.bitrate << " bps" << std::endl;
    std::cout << "  Video codec: " << video_settings.video_codec << std::endl;
    std::cout << "  Debug overlay: " << (video_settings.show_debug_info ? "enabled" : "disabled") << std::endl;
    std::cout << "  Audio file: " << (video_settings.include_audio ? video_settings.audio_file_path : "(none)") << std::endl;
    std::cout << "  Output path: " << video_settings.output_path << std::endl;
    std::cout << "  Blip color mode: " << ColorModeToString(video_settings.color_mode) << std::endl;
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

        if (preview_window && glfwWindowShouldClose(preview_window)) {
            std::cout << "Preview window closed by user. Continuing headless rendering only." << std::endl;
            glfwDestroyWindow(preview_window);
            preview_window = nullptr;
            glfwMakeContextCurrent(window);
        }

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
        g_renderer->ResetDrawCallCount();
        g_renderer->BindOffscreenFramebuffer(); // ビデオ解像度のオフスクリーンFBOにバインド
        g_renderer->Clear(Color(0.1f, 0.1f, 0.1f, 1.0f)); // Dark gray background
        g_piano_keyboard->Render(*g_renderer);

        // デバッグ情報を描画 (デバッグモードが有効な場合)
        g_midi_video_output->RenderDebugOverlay();

        if (g_opengl_renderer) {
            // Ensure all OpenGL commands are executed before frame capture
            glFlush();
            glFinish();
        }

        // フレームバッファのバインドを解除（デフォルトフレームバッファに戻す）
        g_renderer->UnbindOffscreenFramebuffer();

        if (preview_window && g_opengl_renderer) {
            glfwMakeContextCurrent(preview_window);
            int preview_fb_width = PREVIEW_WIDTH;
            int preview_fb_height = PREVIEW_HEIGHT;
            glfwGetFramebufferSize(preview_window, &preview_fb_width, &preview_fb_height);
            glClearColor(0.05f, 0.05f, 0.05f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            g_renderer->RenderOffscreenTextureToScreen(preview_fb_width, preview_fb_height);

            std::vector<std::string> overlay_lines;
            const auto& preview_settings = g_midi_video_output->GetVideoSettings();

            std::ostringstream ffmpeg_stream;
            ffmpeg_stream << "FFmpeg: " << preview_settings.video_codec
                          << " | " << preview_settings.width << "x" << preview_settings.height
                          << "@" << preview_settings.fps << "fps"
                          << " | " << std::fixed << std::setprecision(1)
                          << (preview_settings.bitrate / 1000000.0f) << " Mbps"
                          << " (" << (preview_settings.use_cbr ? "CBR" : "VBR") << ")";
            overlay_lines.push_back(ffmpeg_stream.str());

            std::ostringstream audio_stream;
            if (preview_settings.include_audio && !preview_settings.audio_file_path.empty()) {
                std::filesystem::path audio_path(preview_settings.audio_file_path);
                audio_stream << "Audio: AAC " << (preview_settings.audio_bitrate / 1000)
                             << " kbps (" << audio_path.filename().string() << ")";
            } else {
                audio_stream << "Audio: (none)";
            }
            overlay_lines.push_back(audio_stream.str());

            double current_time = g_midi_video_output->GetCurrentTime();
            double total_duration = g_midi_video_output->GetTotalDuration();
            std::string total_time_str = total_duration > 0.0 ? FormatTime(total_duration) : "--:--";

            std::ostringstream time_stream;
            time_stream << "Time: " << FormatTime(current_time) << " / " << total_time_str;
            overlay_lines.push_back(time_stream.str());

            float progress_ratio = g_midi_video_output->GetProgress();
            g_renderer->RenderPreviewOverlay(preview_fb_width, preview_fb_height, overlay_lines, progress_ratio);

            glfwSwapBuffers(preview_window);
            glfwMakeContextCurrent(window);
        }
    }

    // Cleanup
    g_midi_video_output.reset();
    g_piano_keyboard.reset();
    g_renderer.reset();

    if (preview_window) {
        glfwDestroyWindow(preview_window);
        preview_window = nullptr;
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    std::cout << "Application closed successfully." << std::endl;
    return 0;
}

void error_callback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}