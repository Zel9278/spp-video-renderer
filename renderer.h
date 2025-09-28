#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#if defined(_WIN32)
#ifdef DrawText
#pragma push_macro("DrawText")
#undef DrawText
#define RENDERER_RESTORE_DRAWTEXT 1
#endif
#ifdef GetCurrentTime
#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime
#define RENDERER_RESTORE_GETCURRENTTIME 1
#endif
#endif

struct Vec2 {
    float x;
    float y;

    Vec2() : x(0.0f), y(0.0f) {}
    Vec2(float x_, float y_) : x(x_), y(y_) {}
};

struct Color {
    float r;
    float g;
    float b;
    float a;

    Color() : r(1.0f), g(1.0f), b(1.0f), a(1.0f) {}
    Color(float r_, float g_, float b_, float a_ = 1.0f)
        : r(r_), g(g_), b(b_), a(a_) {}

    static Color FromRGB(int r, int g, int b, int a = 255) {
        return Color(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
    }

    static Color FromHex(std::uint32_t hex) {
        if (hex <= 0xFFFFFFu) {
            const int r = static_cast<int>((hex >> 16) & 0xFFu);
            const int g = static_cast<int>((hex >> 8) & 0xFFu);
            const int b = static_cast<int>(hex & 0xFFu);
            return FromRGB(r, g, b, 255);
        }
        const int a = static_cast<int>((hex >> 24) & 0xFFu);
        const int r = static_cast<int>((hex >> 16) & 0xFFu);
        const int g = static_cast<int>((hex >> 8) & 0xFFu);
        const int b = static_cast<int>(hex & 0xFFu);
        return FromRGB(r, g, b, a);
    }

    std::uint32_t ToHex() const {
        auto clamp_channel = [](float value) {
            value = std::max(0.0f, std::min(1.0f, value));
            return static_cast<std::uint32_t>(value * 255.0f + 0.5f);
        };

        const std::uint32_t r8 = clamp_channel(r);
        const std::uint32_t g8 = clamp_channel(g);
        const std::uint32_t b8 = clamp_channel(b);
        return (r8 << 16) | (g8 << 8) | b8;
    }
};

struct Rect {
    Vec2 position;
    Vec2 size;
    Color color;
    Color border_color;
    float border_width;

    Rect() : border_width(1.0f) {}
    Rect(Vec2 pos, Vec2 sz, Color col)
        : position(pos), size(sz), color(col), border_width(1.0f) {}
};

class RendererBackend {
public:
    virtual ~RendererBackend() = default;

    virtual const char* GetName() const = 0;

    virtual void Initialize(int window_width, int window_height) = 0;
    virtual void SetViewport(int width, int height) = 0;

    virtual void Clear(const Color& clear_color) = 0;
    virtual void ClearWithRadialGradient(const Color& center_color, const Color& edge_color) = 0;
    virtual void ClearWithImage(const std::string& image_path, float opacity, int scale_mode) = 0;

    virtual bool LoadFont(float font_size = 16.0f) = 0;
    virtual void DrawText(const std::string& text, const Vec2& position, const Color& color, float scale = 1.0f) = 0;
    virtual Vec2 GetTextSize(const std::string& text, float scale = 1.0f) = 0;

    virtual void DrawRect(const Vec2& position, const Vec2& size, const Color& color) = 0;
    virtual void DrawRectGradient(const Vec2& position, const Vec2& size,
                                  const Color& top_color, const Color& bottom_color) = 0;
    virtual void DrawRectGradientRounded(const Vec2& position, const Vec2& size,
                                         const Color& top_color, const Color& bottom_color,
                                         float corner_radius = 5.0f) = 0;
    virtual void DrawRectWithBorder(const Vec2& position, const Vec2& size,
                                    const Color& fill_color, const Color& border_color,
                                    float border_width = 1.0f) = 0;
    virtual void DrawRectWithRoundedBorder(const Vec2& position, const Vec2& size,
                                           const Color& fill_color, const Color& border_color,
                                           float border_width = 1.0f, float corner_radius = 5.0f) = 0;

    virtual void BeginBatch() = 0;
    virtual void EndBatch() = 0;

    virtual void BeginFrame() = 0;
    virtual void EndFrame() = 0;

    virtual bool CreateOffscreenFramebuffer(int width, int height) = 0;
    virtual void BindOffscreenFramebuffer() = 0;
    virtual void UnbindOffscreenFramebuffer() = 0;

    virtual bool InitializePBO(int width, int height) = 0;
    virtual void CleanupPBO() = 0;

    virtual std::vector<std::uint8_t> ReadFramebuffer(int width, int height) = 0;
    virtual std::vector<std::uint8_t> ReadFramebufferPBO(int width, int height) = 0;
    virtual void StartAsyncReadback(int width, int height) = 0;
    virtual std::vector<std::uint8_t> GetAsyncReadbackResult(int width, int height) = 0;

    virtual void RenderOffscreenTextureToScreen(int screen_width, int screen_height) = 0;
    virtual void RenderPreviewOverlay(int screen_width, int screen_height,
                                      const std::vector<std::string>& info_lines,
                                      float progress_ratio) = 0;

    virtual Vec2 ScreenToGL(const Vec2& screen_pos) const = 0;
    virtual Vec2 GLToScreen(const Vec2& gl_pos) const = 0;

    virtual void ResetDrawCallCount() = 0;
    virtual unsigned int GetDrawCallCount() const = 0;

    virtual bool SupportsPreview() const { return true; }
    virtual bool SupportsAsyncReadback() const { return true; }
};

#if defined(_WIN32)
#ifdef RENDERER_RESTORE_DRAWTEXT
#pragma pop_macro("DrawText")
#undef RENDERER_RESTORE_DRAWTEXT
#endif
#ifdef RENDERER_RESTORE_GETCURRENTTIME
#pragma pop_macro("GetCurrentTime")
#undef RENDERER_RESTORE_GETCURRENTTIME
#endif
#endif
