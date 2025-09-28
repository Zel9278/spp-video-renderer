#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <glad/glad.h>
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <vector>
#include <cstdlib>
#include <cstddef>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#include "../resources/launcher_resource.h"
#include <shlobj_core.h>
#include <combaseapi.h>
#include <commdlg.h>
#pragma comment(lib, "Comdlg32.lib")
#endif

#include "../resources/window_icon_loader.h"

namespace {

static const unsigned char kLauncherIconPng[] = {
#include "icon.png.h"
};
static constexpr std::size_t kLauncherIconPngSize = sizeof(kLauncherIconPng);

static void SetFallbackWindowIcon(GLFWwindow* window) {
    // Create a simple 32x32 launcher-themed icon
    const int size = 32;
    unsigned char* pixels = new unsigned char[size * size * 4];
    
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int idx = (y * size + x) * 4;
            
            // Create a play button / launcher icon pattern
            int center_x = size / 2;
            int center_y = size / 2;
            int dx = x - center_x;
            int dy = y - center_y;
            
            // Triangle play button shape
            bool is_in_triangle = (dx > -8 && dx < 8 && dy > -6 && dy < 6) && (dx > dy * -0.5 - 4);
            
            if (is_in_triangle) {
                // Play button (green)
                pixels[idx + 0] = 50;   // R
                pixels[idx + 1] = 200;  // G
                pixels[idx + 2] = 50;   // B
            } else {
                // Background (light blue)
                pixels[idx + 0] = 100;  // R
                pixels[idx + 1] = 150;  // G
                pixels[idx + 2] = 255;  // B
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
    if (!window_icon::SetWindowIconFromPng(window, kLauncherIconPng, kLauncherIconPngSize)) {
        SetFallbackWindowIcon(window);
    }
}

enum class JobStatus {
    Idle,
    Running,
    Completed,
    Failed
};

struct RenderOptions {
    enum class ColorMode {
        Channel = 0,
        Track = 1,
        Both = 2
    };

    int video_width = 1920;
    int video_height = 1080;
    bool show_preview = false;
    bool debug_overlay = false;
    bool include_audio = false;
    std::string video_codec = "libx264";
    bool use_cbr = true;
    int video_bitrate = 240000000;
    ColorMode color_mode = ColorMode::Channel;
    std::string ffmpeg_path;        // Custom FFmpeg executable path
    std::string output_directory;   // Custom output directory
};

constexpr std::size_t kPathBufferSize = 1024;
constexpr std::size_t kMaxLogLines = 2000;

class ProcessRunner {
public:
    ProcessRunner() = default;
    ~ProcessRunner() {
        join();
    }

    void start(const std::string& command) {
        join();
        clear_logs();
        status_.store(JobStatus::Running);
        start_time_ = std::chrono::steady_clock::now();
        worker_ = std::thread([this, command]() {
#ifdef _WIN32
            std::wstring wide_command(command.begin(), command.end());
            run_windows(wide_command);
#else
            run_posix(command);
#endif
        });
    }

#ifdef _WIN32
    void start(const std::wstring& command) {
        join();
        clear_logs();
        status_.store(JobStatus::Running);
        start_time_ = std::chrono::steady_clock::now();
        worker_ = std::thread([this, command]() { run_windows(command); });
    }
#endif

    JobStatus status() const {
        return status_.load();
    }

    bool is_running() const {
        return status() == JobStatus::Running;
    }

    std::chrono::steady_clock::duration elapsed() const {
        if (status() == JobStatus::Idle) {
            return std::chrono::steady_clock::duration{0};
        }
        return std::chrono::steady_clock::now() - start_time_;
    }

    std::vector<std::string> log_snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return logs_;
    }

    bool consume_scroll_request() {
        return scroll_to_bottom_.exchange(false);
    }

    void append_line(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!line.empty()) {
            logs_.push_back(line);
            if (logs_.size() > kMaxLogLines) {
                logs_.erase(logs_.begin(), logs_.begin() + (logs_.size() - kMaxLogLines));
            }
            scroll_to_bottom_ = true;
        }
    }

    void join() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void terminate();

private:
    void clear_logs() {
        std::lock_guard<std::mutex> lock(mutex_);
        logs_.clear();
        partial_line_.clear();
        scroll_to_bottom_ = true;
    }

    void append_chunk(const std::string& chunk) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!chunk.empty()) {
            partial_line_ += chunk;
            std::size_t start = 0;
            while (start < partial_line_.size()) {
                auto pos = partial_line_.find_first_of("\r\n", start);
                if (pos == std::string::npos) {
                    break;
                }

                std::string line = partial_line_.substr(start, pos - start);
                if (!line.empty()) {
                    logs_.push_back(std::move(line));
                } else {
                    logs_.emplace_back();
                }
                if (logs_.size() > kMaxLogLines) {
                    logs_.erase(logs_.begin(), logs_.begin() + (logs_.size() - kMaxLogLines));
                }
                scroll_to_bottom_ = true;

                if (partial_line_[pos] == '\r' && pos + 1 < partial_line_.size() && partial_line_[pos + 1] == '\n') {
                    start = pos + 2;
                } else {
                    start = pos + 1;
                }
            }
            if (start > 0) {
                partial_line_.erase(0, start);
            }
        }
    }

    void flush_partial_line() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!partial_line_.empty()) {
            logs_.push_back(partial_line_);
            if (logs_.size() > kMaxLogLines) {
                logs_.erase(logs_.begin(), logs_.begin() + (logs_.size() - kMaxLogLines));
            }
            partial_line_.clear();
            scroll_to_bottom_ = true;
        }
    }

#ifndef _WIN32
    void run_posix(const std::string& command) {
        int pipefd[2] = {-1, -1};
        if (pipe(pipefd) == -1) {
            append_line("Failed to create pipe for process output.");
            status_.store(JobStatus::Failed);
            return;
        }

        pid_t pid = fork();
        if (pid == -1) {
            append_line("Failed to fork process.");
            close(pipefd[0]);
            close(pipefd[1]);
            status_.store(JobStatus::Failed);
            return;
        }

        if (pid == 0) {
            // Child process: redirect stdout/stderr and exec shell command.
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        // Parent process
        close(pipefd[1]);

        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            child_pid_ = pid;
        }

        std::array<char, 4096> buffer{};
        while (true) {
            ssize_t bytes_read = read(pipefd[0], buffer.data(), buffer.size());
            if (bytes_read > 0) {
                append_chunk(std::string(buffer.data(), static_cast<std::size_t>(bytes_read)));
            } else if (bytes_read == 0) {
                break; // EOF
            } else {
                if (errno == EINTR) {
                    continue;
                }
                append_line("Error reading process output.");
                break;
            }
        }

        close(pipefd[0]);

        int status_code = 0;
        pid_t waited = -1;
        do {
            waited = waitpid(pid, &status_code, 0);
        } while (waited == -1 && errno == EINTR);

        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            child_pid_ = -1;
        }

        flush_partial_line();

        int exit_code = 0;
        if (waited == -1) {
            append_line("Failed to retrieve process exit status.");
            status_.store(JobStatus::Failed);
            return;
        }

        if (WIFEXITED(status_code)) {
            exit_code = WEXITSTATUS(status_code);
        } else if (WIFSIGNALED(status_code)) {
            exit_code = 128 + WTERMSIG(status_code);
        }

        std::ostringstream oss;
        oss << "Exit code: " << exit_code;
        append_line(oss.str());
        status_.store(exit_code == 0 ? JobStatus::Completed : JobStatus::Failed);
    }
#else
    void run_windows(const std::wstring& command_line) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE read_pipe = nullptr;
        HANDLE write_pipe = nullptr;

        if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
            append_line("Failed to create output pipe.");
            status_.store(JobStatus::Failed);
            return;
        }

        if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
            append_line("Failed to configure pipe handle.");
            CloseHandle(read_pipe);
            CloseHandle(write_pipe);
            status_.store(JobStatus::Failed);
            return;
        }

        STARTUPINFOW si{};
        si.cb = sizeof(STARTUPINFOW);
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe;
        si.hStdError = write_pipe;
        si.hStdInput = nullptr;

        PROCESS_INFORMATION pi{};

        std::vector<wchar_t> cmd_buffer(command_line.begin(), command_line.end());
        cmd_buffer.push_back(L'\0');

        BOOL created = CreateProcessW(
            nullptr,
            cmd_buffer.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi);

        CloseHandle(write_pipe);

        if (!created) {
            append_line("Failed to start process.");
            if (read_pipe) {
                CloseHandle(read_pipe);
            }
            status_.store(JobStatus::Failed);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            process_handle_ = pi.hProcess;
        }

        std::array<char, 4096> buffer{};
        DWORD bytes_read = 0;
        BOOL success = FALSE;
        while (true) {
            success = ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &bytes_read, nullptr);
            if (!success || bytes_read == 0) {
                break;
            }
            buffer[bytes_read] = '\0';
            append_chunk(std::string(buffer.data(), bytes_read));
        }

        flush_partial_line();

        CloseHandle(read_pipe);
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        {
            std::lock_guard<std::mutex> lock(process_mutex_);
            process_handle_ = nullptr;
        }

        std::ostringstream oss;
    oss << "Exit code: " << exit_code;
        append_line(oss.str());
        status_.store(exit_code == 0 ? JobStatus::Completed : JobStatus::Failed);
    }
#endif

    std::thread worker_;
    std::mutex mutex_;
    std::mutex process_mutex_;
    std::vector<std::string> logs_;
    std::string partial_line_;
    std::atomic<JobStatus> status_{JobStatus::Idle};
    std::atomic<bool> scroll_to_bottom_{true};
    std::chrono::steady_clock::time_point start_time_{};
#ifdef _WIN32
    HANDLE process_handle_ = nullptr;
#else
    pid_t child_pid_ = -1;
#endif
};

void ProcessRunner::terminate() {
    if (!is_running()) {
        return;
    }

#ifdef _WIN32
    bool terminated = false;
    {
        std::lock_guard<std::mutex> lock(process_mutex_);
        if (process_handle_) {
            terminated = TerminateProcess(process_handle_, 1) != 0;
        }
    }

    if (terminated) {
        append_line("Termination requested by user.");
    } else {
        append_line("Failed to terminate renderer process.");
    }
#else
    pid_t pid = -1;
    {
        std::lock_guard<std::mutex> lock(process_mutex_);
        pid = child_pid_;
    }

    if (pid <= 0) {
        append_line("No active renderer process to terminate.");
        return;
    }

    bool terminated = false;
    if (kill(pid, SIGTERM) == 0) {
        terminated = true;
    } else if (errno == ESRCH) {
        terminated = true; // Process already gone
    } else {
        // Try a forceful kill as fallback
        if (kill(pid, SIGKILL) == 0) {
            terminated = true;
        }
    }

    if (terminated) {
        append_line("Termination requested by user.");
    } else {
        append_line("Failed to terminate renderer process.");
    }
#endif
}

std::string format_duration(std::chrono::steady_clock::duration duration) {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    std::ostringstream oss;
    oss << seconds << "s";
    return oss.str();
}

std::string quote_argument(const std::string& value) {
    if (value.find_first_of(" \"\t") == std::string::npos) {
        return value;
    }
    std::string quoted = "\"";
    for (char c : value) {
        if (c == '\"') {
            quoted += "\\\"";
        } else {
            quoted += c;
        }
    }
    quoted += '"';
    return quoted;
}

#ifdef _WIN32
std::wstring quote_argument(const std::wstring& value) {
    if (value.find_first_of(L" \"\t") == std::wstring::npos) {
        return value;
    }
    std::wstring quoted = L"\"";
    for (wchar_t c : value) {
        if (c == L'\"') {
            quoted += L"\\\"";
        } else {
            quoted += c;
        }
    }
    quoted += L'"';
    return quoted;
}
#endif

const char* color_mode_to_string(RenderOptions::ColorMode mode) {
    switch (mode) {
        case RenderOptions::ColorMode::Channel:
            return "channel";
        case RenderOptions::ColorMode::Track:
            return "track";
        case RenderOptions::ColorMode::Both:
            return "both";
    }
    return "channel";
}

#ifdef _WIN32
std::wstring color_mode_to_wstring(RenderOptions::ColorMode mode) {
    const char* value = color_mode_to_string(mode);
    return std::wstring(value, value + std::strlen(value));
}
#endif

std::string build_command_line(const std::filesystem::path& renderer,
                               const std::filesystem::path& midi_file,
                               const std::optional<std::filesystem::path>& audio_file,
                               const RenderOptions& opts) {
    std::ostringstream cmd;
    cmd << quote_argument(renderer.string());
    cmd << ' ' << quote_argument(midi_file.string());
    cmd << " --video-codec " << quote_argument(opts.video_codec);

    cmd << " --resolution " << quote_argument(std::to_string(opts.video_width) + "x" + std::to_string(opts.video_height));
    cmd << " --bitrate " << quote_argument(std::to_string(opts.video_bitrate));
    cmd << (opts.use_cbr ? " --cbr" : " --vbr");
    cmd << " --color-mode " << quote_argument(color_mode_to_string(opts.color_mode));

    if (opts.debug_overlay) {
        cmd << " --debug";
    }
    if (opts.show_preview) {
        cmd << " --show-preview";
    }

    if (!opts.ffmpeg_path.empty()) {
        cmd << " --ffmpeg-path " << quote_argument(opts.ffmpeg_path);
    }
    
    if (!opts.output_directory.empty()) {
        cmd << " --output-directory " << quote_argument(opts.output_directory);
    }

    if (audio_file) {
        cmd << " --audio-file " << quote_argument(audio_file->string());
    }

    return cmd.str();
}

#ifdef _WIN32
std::wstring build_command_line_w(const std::filesystem::path& renderer,
                                  const std::filesystem::path& midi_file,
                                  const std::optional<std::filesystem::path>& audio_file,
                                  const RenderOptions& opts) {
    std::wostringstream cmd;
    cmd << quote_argument(renderer.wstring());
    cmd << L' ' << quote_argument(midi_file.wstring());
    std::wstring codec(opts.video_codec.begin(), opts.video_codec.end());
    cmd << L" --video-codec " << quote_argument(codec);

    std::wostringstream resolution;
    resolution << opts.video_width << L"x" << opts.video_height;
    cmd << L" --resolution " << quote_argument(resolution.str());

    std::wstring bitrate = std::to_wstring(opts.video_bitrate);
    cmd << L" --bitrate " << quote_argument(bitrate);
    cmd << (opts.use_cbr ? L" --cbr" : L" --vbr");
    cmd << L" --color-mode " << quote_argument(color_mode_to_wstring(opts.color_mode));

    if (opts.debug_overlay) {
        cmd << L" --debug";
    }
    if (opts.show_preview) {
        cmd << L" --show-preview";
    }

    if (!opts.ffmpeg_path.empty()) {
        std::wstring ffmpeg_path_w(opts.ffmpeg_path.begin(), opts.ffmpeg_path.end());
        cmd << L" --ffmpeg-path " << quote_argument(ffmpeg_path_w);
    }
    
    if (!opts.output_directory.empty()) {
        std::wstring output_dir_w(opts.output_directory.begin(), opts.output_directory.end());
        cmd << L" --output-directory " << quote_argument(output_dir_w);
    }

    if (audio_file) {
        cmd << L" --audio-file " << quote_argument(audio_file->wstring());
    }

    return cmd.str();
}
#endif

void set_path_buffer(std::array<char, kPathBufferSize>& buffer, const std::filesystem::path& value) {
    auto str = value.string();
    if (str.size() >= buffer.size()) {
        str.resize(buffer.size() - 1);
    }
    std::memset(buffer.data(), 0, buffer.size());
    std::memcpy(buffer.data(), str.c_str(), str.size());
}

std::filesystem::path buffer_to_path(const std::array<char, kPathBufferSize>& buffer) {
    return std::filesystem::path(std::string(buffer.data()));
}

#ifdef _WIN32
std::optional<std::filesystem::path> show_open_file_dialog(GLFWwindow* window,
                                                           const wchar_t* filter) {
    HWND hwnd = glfwGetWin32Window(window);

    wchar_t file_buffer[MAX_PATH] = {0};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = file_buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        return std::filesystem::path(ofn.lpstrFile);
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> show_select_folder_dialog(GLFWwindow* window) {
    HWND hwnd = glfwGetWin32Window(window);

    wchar_t display_name[MAX_PATH];
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd;
    bi.pszDisplayName = display_name;
    bi.lpszTitle = L"Select output directory";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl != nullptr) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            CoTaskMemFree(pidl);
            return std::filesystem::path(path);
        }
        CoTaskMemFree(pidl);
    }

    return std::nullopt;
}
#endif

#ifndef _WIN32
std::string shell_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '"' || c == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

bool command_exists(const char* command) {
    std::string check = "command -v ";
    check += command;
    check += " >/dev/null 2>&1";
    int result = std::system(check.c_str());
    return result == 0;
}

std::string join_patterns(const std::vector<std::string>& patterns) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        if (i > 0) {
            oss << ' ';
        }
        oss << patterns[i];
    }
    return oss.str();
}

std::optional<std::string> run_command_capture(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return std::nullopt;
    }

    std::string output;
    std::array<char, 1024> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output.append(buffer.data());
    }

    int status = pclose(pipe);
    if (status != 0) {
        return std::nullopt;
    }

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }

    if (output.empty()) {
        return std::nullopt;
    }

    return output;
}

std::optional<std::string> run_zenity_like(const char* executable,
                                           const std::string& title,
                                           const std::vector<std::pair<std::string, std::vector<std::string>>>& filters) {
    std::ostringstream command;
    command << executable << " --file-selection --title=" << shell_escape(title);

    for (const auto& filter : filters) {
        if (filter.second.empty()) {
            continue;
        }
        std::string pattern = join_patterns(filter.second);
        std::string label = filter.first;
        if (label.empty()) {
            label = pattern;
        }
        command << " --file-filter=" << shell_escape(label + " | " + pattern);
    }

    return run_command_capture(command.str());
}

std::optional<std::string> run_kdialog(const std::string& title,
                                       const std::vector<std::pair<std::string, std::vector<std::string>>>& filters) {
    std::ostringstream command;
    command << "kdialog --getopenfilename " << shell_escape(".");
    if (!filters.empty() && !filters.front().second.empty()) {
        command << ' ' << shell_escape(join_patterns(filters.front().second));
    }
    command << " --title " << shell_escape(title);

    return run_command_capture(command.str());
}

std::optional<std::filesystem::path> show_open_file_dialog_generic(
    const std::string& title,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& filters) {
    const char* zenity_like_commands[] = {"zenity", "qarma"};
    for (const char* command : zenity_like_commands) {
        if (command_exists(command)) {
            if (auto result = run_zenity_like(command, title, filters)) {
                return std::filesystem::path(*result);
            }
        }
    }

    if (command_exists("kdialog")) {
        if (auto result = run_kdialog(title, filters)) {
            return std::filesystem::path(*result);
        }
    }

    std::cerr << "No supported file dialog command found (zenity/qarma/kdialog)." << std::endl;
    return std::nullopt;
}
#endif

std::optional<std::filesystem::path> select_renderer_executable(GLFWwindow* window) {
#ifdef _WIN32
    static constexpr wchar_t renderer_filter[] = L"Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0\0";
    return show_open_file_dialog(window, renderer_filter);
#else
    const std::vector<std::pair<std::string, std::vector<std::string>>> filters = {
        {"Executables", {"*"}},
        {"All files", {"*"}}
    };
    return show_open_file_dialog_generic("Select renderer executable", filters);
#endif
}

std::optional<std::filesystem::path> select_midi_file(GLFWwindow* window) {
#ifdef _WIN32
    static constexpr wchar_t midi_filter[] = L"MIDI Files (*.mid;*.midi)\0*.mid;*.midi\0All Files (*.*)\0*.*\0\0";
    return show_open_file_dialog(window, midi_filter);
#else
    const std::vector<std::pair<std::string, std::vector<std::string>>> filters = {
        {"MIDI files", {"*.mid", "*.midi"}},
        {"All files", {"*"}}
    };
    return show_open_file_dialog_generic("Select MIDI file", filters);
#endif
}

std::optional<std::filesystem::path> select_audio_file(GLFWwindow* window) {
#ifdef _WIN32
    static constexpr wchar_t audio_filter[] = L"Audio Files (*.wav;*.mp3;*.flac;*.ogg)\0*.wav;*.mp3;*.flac;*.ogg\0All Files (*.*)\0*.*\0\0";
    return show_open_file_dialog(window, audio_filter);
#else
    const std::vector<std::pair<std::string, std::vector<std::string>>> filters = {
        {"Audio files", {"*.wav", "*.mp3", "*.flac", "*.ogg"}},
        {"All files", {"*"}}
    };
    return show_open_file_dialog_generic("Select audio file", filters);
#endif
}

std::optional<std::filesystem::path> select_executable_file(GLFWwindow* window, const std::string& title, const std::vector<std::string>& extensions) {
#ifdef _WIN32
    static constexpr wchar_t exe_filter[] = L"Executable Files (*.exe)\0*.exe\0All Files (*.*)\0*.*\0\0";
    return show_open_file_dialog(window, exe_filter);
#else
    const std::vector<std::pair<std::string, std::vector<std::string>>> filters = {
        {"Executable files", extensions},
        {"All files", {"*"}}
    };
    return show_open_file_dialog_generic(title, filters);
#endif
}

std::optional<std::filesystem::path> select_directory(GLFWwindow* window, const std::string& title) {
#ifdef _WIN32
    return show_select_folder_dialog(window);
#else
    // For Unix-like systems, use zenity or kdialog directory selection
    std::string command = "zenity --file-selection --directory --title=" + shell_escape(title);
    auto result = run_command_capture(command);
    if (result && !result->empty()) {
        return std::filesystem::path(*result);
    }
    return std::nullopt;
#endif
}

std::filesystem::path default_renderer_path(const std::filesystem::path& exe_dir) {
#ifdef _WIN32
    std::filesystem::path candidate = exe_dir / "MPP Video Renderer.exe";
    if (std::filesystem::exists(candidate)) {
        return candidate;
    }
    return candidate;
#else
    std::filesystem::path candidate = exe_dir / "MPP Video Renderer";
    return candidate;
#endif
}

ImVec4 status_color(JobStatus status) {
    switch (status) {
        case JobStatus::Idle:
            return {0.8F, 0.8F, 0.8F, 1.0F};
        case JobStatus::Running:
            return {0.1F, 0.7F, 0.3F, 1.0F};
        case JobStatus::Completed:
            return {0.3F, 0.6F, 1.0F, 1.0F};
        case JobStatus::Failed:
            return {0.9F, 0.2F, 0.2F, 1.0F};
    }
    return {1.0F, 1.0F, 1.0F, 1.0F};
}

const char* status_text(JobStatus status) {
    switch (status) {
        case JobStatus::Idle:
            return "Idle";
        case JobStatus::Running:
            return "Rendering";
        case JobStatus::Completed:
            return "Completed";
        case JobStatus::Failed:
            return "Failed";
    }
    return "Unknown";
}

void on_window_close(GLFWwindow* window) {
    auto* runner = static_cast<ProcessRunner*>(glfwGetWindowUserPointer(window));
    if (runner && runner->is_running()) {
        runner->terminate();
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (!glfwInit()) {
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "MPP Video Renderer Launcher", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return -1;
    }
    
    // Set window icon
    SetWindowIcon(window);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();


    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    ProcessRunner runner;
    glfwSetWindowUserPointer(window, &runner);
    glfwSetWindowCloseCallback(window, on_window_close);

    std::filesystem::path exe_path = std::filesystem::absolute(argv[0]);
    std::filesystem::path exe_dir = exe_path.parent_path();

    std::array<char, kPathBufferSize> renderer_path_buffer{};
    set_path_buffer(renderer_path_buffer, default_renderer_path(exe_dir));

    std::array<char, kPathBufferSize> midi_path_buffer{};
    std::array<char, kPathBufferSize> audio_path_buffer{};
    std::array<char, kPathBufferSize> ffmpeg_path_buffer{};
    std::array<char, kPathBufferSize> output_dir_buffer{};

    RenderOptions options;

    const std::vector<std::string> codec_items = {
        "libx264",
        "libx265",
        "libvpx-vp9",
        "h264_nvenc",
        "hevc_nvenc",
        "h264_qsv",
        "hevc_qsv",
        "h264_amf",
        "hevc_amf"
    };

    int codec_index = 0;
    for (int i = 0; i < static_cast<int>(codec_items.size()); ++i) {
        if (codec_items[i] == options.video_codec) {
            codec_index = i;
            break;
        }
    }

    std::string validation_message;
    bool log_auto_scroll = true;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 16.0f));

        constexpr ImGuiWindowFlags kRootWindowFlags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus;

        ImGui::Begin("Settings", nullptr, kRootWindowFlags);

        // File Paths Section
        if (ImGui::CollapsingHeader("File Paths", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextUnformatted("MPP Video Renderer executable");
            ImGui::InputText("##renderer_path", renderer_path_buffer.data(), renderer_path_buffer.size());
            ImGui::SameLine();
            if (ImGui::Button("Browse##renderer")) {
                if (auto file = select_renderer_executable(window)) {
                    set_path_buffer(renderer_path_buffer, *file);
                }
            }

            ImGui::TextUnformatted("MIDI file");
            ImGui::InputText("##midi_path", midi_path_buffer.data(), midi_path_buffer.size());
            ImGui::SameLine();
            if (ImGui::Button("Browse##midi")) {
                if (auto file = select_midi_file(window)) {
                    set_path_buffer(midi_path_buffer, *file);
                }
            }

            ImGui::TextUnformatted("Audio file (optional)");
            ImGui::InputText("##audio_path", audio_path_buffer.data(), audio_path_buffer.size());
            ImGui::SameLine();
            if (ImGui::Button("Browse##audio")) {
                if (auto file = select_audio_file(window)) {
                    set_path_buffer(audio_path_buffer, *file);
                }
            }

            ImGui::TextUnformatted("FFmpeg executable (optional)");
            ImGui::InputText("##ffmpeg_path", ffmpeg_path_buffer.data(), ffmpeg_path_buffer.size());
            ImGui::SameLine();
            if (ImGui::Button("Browse##ffmpeg")) {
                if (auto file = select_executable_file(window, "Select FFmpeg executable", {"*.exe"})) {
                    set_path_buffer(ffmpeg_path_buffer, *file);
                }
            }

            ImGui::TextUnformatted("Output directory (optional)");
            ImGui::InputText("##output_dir", output_dir_buffer.data(), output_dir_buffer.size());
            ImGui::SameLine();
            if (ImGui::Button("Browse##output_dir")) {
                if (auto dir = select_directory(window, "Select output directory")) {
                    set_path_buffer(output_dir_buffer, *dir);
                }
            }
        }

        ImGui::Separator();

        // Video Settings Section
        if (ImGui::CollapsingHeader("Video Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Combo("Video codec", &codec_index, [](void* data, int idx, const char** out_text) {
                    const auto& vec = *static_cast<const std::vector<std::string>*>(data);
                    if (idx < 0 || idx >= static_cast<int>(vec.size())) {
                        return false;
                    }
                    *out_text = vec[idx].c_str();
                    return true;
                },
                (void*)&codec_items, static_cast<int>(codec_items.size()))) {
                options.video_codec = codec_items[codec_index];
            }
            ImGui::InputInt("Width", &options.video_width);
            ImGui::InputInt("Height", &options.video_height);
            float bitrate_mbps = options.video_bitrate / 1000000.0f;
            if (bitrate_mbps < 1.0f) {
                bitrate_mbps = 1.0f;
            }
            if (ImGui::DragFloat("Video Bitrate (Mbps)", &bitrate_mbps, 1.0f, 1.0f, 1000.0f, "%.1f")) {
                bitrate_mbps = std::max(1.0f, bitrate_mbps);
                options.video_bitrate = static_cast<int>(bitrate_mbps * 1000000.0f);
            }
            ImGui::Text("Bitrate: %d bps", options.video_bitrate);
            ImGui::Checkbox("Constant Bitrate (CBR)", &options.use_cbr);
            
            const char* color_mode_items[] = {"Channel", "Track", "Both"};
            int color_mode_index_ui = static_cast<int>(options.color_mode);
            if (ImGui::Combo("Blip color mode", &color_mode_index_ui, color_mode_items, IM_ARRAYSIZE(color_mode_items))) {
                if (color_mode_index_ui < 0 || color_mode_index_ui > 2) {
                    color_mode_index_ui = 0;
                }
                options.color_mode = static_cast<RenderOptions::ColorMode>(color_mode_index_ui);
            }
        }

        ImGui::Separator();

        // Debug & Preview Section
        if (ImGui::CollapsingHeader("Debug & Preview", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Debug overlay", &options.debug_overlay);
            ImGui::Checkbox("Show preview window", &options.show_preview);
        }

        ImGui::Separator();

        bool audio_path_non_empty = std::strlen(audio_path_buffer.data()) > 0;
        if (audio_path_non_empty) {
            options.include_audio = true;
        } else {
            options.include_audio = false;
        }

        if (!validation_message.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.2F, 1.0F));
            ImGui::TextWrapped("%s", validation_message.c_str());
            ImGui::PopStyleColor();
        }

        JobStatus status = runner.status();
        ImGui::Separator();
        ImGui::TextUnformatted("Status");
        ImGui::SameLine();
        ImGui::TextColored(status_color(status), "%s", status_text(status));
        if (status == JobStatus::Running) {
            ImGui::SameLine();
            ImGui::Text("(elapsed %s)", format_duration(runner.elapsed()).c_str());
        }

        bool can_start = (status != JobStatus::Running);
        if (!can_start) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Start rendering", ImVec2(200, 0))) {
            validation_message.clear();

            auto renderer_path = buffer_to_path(renderer_path_buffer);
            auto midi_path = buffer_to_path(midi_path_buffer);
            std::optional<std::filesystem::path> audio_path;
            if (options.include_audio) {
                audio_path = buffer_to_path(audio_path_buffer);
            }

            if (renderer_path.empty() || !std::filesystem::exists(renderer_path)) {
                validation_message = "Renderer executable not found.";
            } else if (midi_path.empty() || !std::filesystem::exists(midi_path)) {
                validation_message = "Please select a MIDI file.";
            } else if (options.video_width <= 0 || options.video_height <= 0) {
                validation_message = "Resolution must be positive.";
            } else if (options.video_bitrate <= 0) {
                validation_message = "Bitrate must be positive.";
            } else if (audio_path && !std::filesystem::exists(*audio_path)) {
                validation_message = "Audio file not found.";
            } else {
                auto ffmpeg_path = buffer_to_path(ffmpeg_path_buffer);
                auto output_dir = buffer_to_path(output_dir_buffer);
                
                // Set paths in options
                if (!ffmpeg_path.empty()) {
                    options.ffmpeg_path = ffmpeg_path.string();
                }
                if (!output_dir.empty()) {
                    options.output_directory = output_dir.string();
                }
                
                std::string command = build_command_line(renderer_path, midi_path, audio_path, options);
#ifdef _WIN32
                std::wstring command_w = build_command_line_w(renderer_path, midi_path, audio_path, options);
                runner.start(command_w);
                runner.append_line("Command: " + command);
#else
                runner.start(command);
                runner.append_line("Command: " + command);
#endif
                runner.append_line("Rendering started.");
            }
        }

        if (!can_start) {
            ImGui::EndDisabled();
        }

        if (status == JobStatus::Running) {
            ImGui::SameLine();
            if (ImGui::Button("Stop rendering", ImVec2(200, 0))) {
                runner.terminate();
            }
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Logs");
        ImGui::Checkbox("Auto-scroll", &log_auto_scroll);
        ImGui::BeginChild("log_scroller", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

        auto logs = runner.log_snapshot();
        for (const auto& line : logs) {
            ImGui::TextUnformatted(line.c_str());
        }

        if (log_auto_scroll) {
            if (runner.consume_scroll_request()) {
                ImGui::SetScrollHereY(1.0F);
            }
        }

        ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar(3);

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1F, 0.1F, 0.1F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    runner.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
