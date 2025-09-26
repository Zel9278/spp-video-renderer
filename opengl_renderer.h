#pragma once

#if defined(_WIN32)
#include <windows.h>
#include <cstdint>
#endif

#include <GL/gl.h>
#include <vector>
#include <string>
#include <algorithm>

struct Vec2 {
    float x, y;
    Vec2() : x(0), y(0) {}
    Vec2(float x_, float y_) : x(x_), y(y_) {}
};

struct Color {
    float r, g, b, a;
    Color() : r(1.0f), g(1.0f), b(1.0f), a(1.0f) {}
    Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    
    // Convert from 0-255 range to 0-1 range
    static Color FromRGB(int r, int g, int b, int a = 255) {
        return Color(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
    }

    // Convert from uint32_t hex color (0xRRGGBB or 0xAARRGGBB)
    static Color FromHex(uint32_t hex) {
        if (hex <= 0xFFFFFF) {
            // RGB format (0xRRGGBB)
            int r = (hex >> 16) & 0xFF;
            int g = (hex >> 8) & 0xFF;
            int b = hex & 0xFF;
            return FromRGB(r, g, b, 255);
        } else {
            // ARGB format (0xAARRGGBB)
            int a = (hex >> 24) & 0xFF;
            int r = (hex >> 16) & 0xFF;
            int g = (hex >> 8) & 0xFF;
            int b = hex & 0xFF;
            return FromRGB(r, g, b, a);
        }
    }

    // Convert to uint32_t hex color (0xRRGGBB format)
    uint32_t ToHex() const {
        int r_int = static_cast<int>(r * 255.0f);
        int g_int = static_cast<int>(g * 255.0f);
        int b_int = static_cast<int>(b * 255.0f);

        // Clamp values to 0-255 range
        r_int = std::max(0, std::min(255, r_int));
        g_int = std::max(0, std::min(255, g_int));
        b_int = std::max(0, std::min(255, b_int));

        return (static_cast<uint32_t>(r_int) << 16) |
               (static_cast<uint32_t>(g_int) << 8) |
               static_cast<uint32_t>(b_int);
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

// Font atlas for text rendering
struct FontAtlas {
    unsigned int texture_id;
    int atlas_width;
    int atlas_height;
    float font_size;
    struct CharInfo {
        float x0, y0, x1, y1; // texture coordinates
        float xoff, yoff;     // offset when drawing
        float xadvance;       // how much to advance cursor
    };
    CharInfo chars[128];      // ASCII characters
    bool loaded;

    FontAtlas() : texture_id(0), atlas_width(0), atlas_height(0), font_size(16.0f), loaded(false) {}
};

class OpenGLRenderer {
public:
    OpenGLRenderer();
    ~OpenGLRenderer();
    
    // Initialize the renderer
    void Initialize(int window_width, int window_height);
    
    // Set viewport size
    void SetViewport(int width, int height);
    
    // Clear screen
    void Clear(const Color& clear_color);

    // Clear screen with radial gradient background
    void ClearWithRadialGradient(const Color& center_color, const Color& edge_color);



    // Clear screen with background image
    void ClearWithImage(const std::string& image_path, float opacity, int scale_mode);

    // Text rendering using embedded font
    bool LoadFont(float font_size = 16.0f);
    void DrawText(const std::string& text, const Vec2& position, const Color& color, float scale = 1.0f);
    Vec2 GetTextSize(const std::string& text, float scale = 1.0f);

    // Draw a filled rectangle
    void DrawRect(const Vec2& position, const Vec2& size, const Color& color);

    // Draw a rectangle with vertical gradient (top to bottom)
    void DrawRectGradient(const Vec2& position, const Vec2& size,
                         const Color& top_color, const Color& bottom_color);

    // Draw a rectangle with vertical gradient and rounded corners
    void DrawRectGradientRounded(const Vec2& position, const Vec2& size,
                                const Color& top_color, const Color& bottom_color,
                                float corner_radius = 5.0f);

    // Draw a rectangle with border
    void DrawRectWithBorder(const Vec2& position, const Vec2& size,
                           const Color& fill_color, const Color& border_color,
                           float border_width = 1.0f);

    // Draw a rectangle with rounded border
    void DrawRectWithRoundedBorder(const Vec2& position, const Vec2& size,
                                  const Color& fill_color, const Color& border_color,
                                  float border_width = 1.0f, float corner_radius = 5.0f);
    
    // Batch drawing for better performance
    void BeginBatch();
    void EndBatch();
    
    // Frame operations for video recording
    void BeginFrame();
    void EndFrame();
    
    // Offscreen rendering for headless mode
    bool CreateOffscreenFramebuffer(int width, int height);
    void BindOffscreenFramebuffer();
    void UnbindOffscreenFramebuffer();
    std::vector<uint8_t> ReadFramebuffer(int width, int height);
    
    // GPU-optimized frame capture with PBO
    bool InitializePBO(int width, int height);
    void CleanupPBO();
    std::vector<uint8_t> ReadFramebufferPBO(int width, int height);
    void StartAsyncReadback(int width, int height);
    std::vector<uint8_t> GetAsyncReadbackResult(int width, int height);

    // Preview rendering
    void RenderOffscreenTextureToScreen(int screen_width, int screen_height);
    void RenderPreviewOverlay(int screen_width, int screen_height,
                              const std::vector<std::string>& info_lines,
                              float progress_ratio);
    
    // Convert screen coordinates to OpenGL coordinates
    Vec2 ScreenToGL(const Vec2& screen_pos) const;
    Vec2 GLToScreen(const Vec2& gl_pos) const;
    
private:
    int window_width_;
    int window_height_;
    std::vector<Rect> batch_rects_;

    // Offscreen rendering
    unsigned int framebuffer_;
    unsigned int color_texture_;
    unsigned int depth_renderbuffer_;
    bool offscreen_initialized_;
    
    // PBO for GPU-optimized frame capture
    unsigned int pbo_[2];  // Double buffering PBOs
    int current_pbo_index_;
    bool pbo_initialized_;

    // Background image cache
    struct BackgroundImage {
        unsigned int texture_id;
        int width;
        int height;
        std::string path;
        bool loaded;

        BackgroundImage() : texture_id(0), width(0), height(0), loaded(false) {}
    };
    BackgroundImage background_image_;

    // Helper functions
    void SetupProjection();
    bool LoadImageTexture(const std::string& path, BackgroundImage& image);
    void DrawImageBackground(const BackgroundImage& image, float opacity, int scale_mode);
    void LoadFontTexture();
    void RenderText(const std::string& text, float x, float y, float size, const Color& color);
};
