#pragma once

#if defined(_WIN32)
#include <cstdint>
#endif

#include <GL/gl.h>
#include <vector>
#include <string>

#include "renderer.h"

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

class OpenGLRenderer : public RendererBackend {
public:
    OpenGLRenderer();
    ~OpenGLRenderer();
    
    // Initialize the renderer
    void Initialize(int window_width, int window_height) override;
    
    // Set viewport size
    void SetViewport(int width, int height) override;
    
    // Clear screen
    void Clear(const Color& clear_color) override;

    // Clear screen with radial gradient background
    void ClearWithRadialGradient(const Color& center_color, const Color& edge_color) override;



    // Clear screen with background image
    void ClearWithImage(const std::string& image_path, float opacity, int scale_mode) override;

    // Text rendering using embedded font
    bool LoadFont(float font_size = 16.0f) override;
    void DrawText(const std::string& text, const Vec2& position, const Color& color, float scale = 1.0f) override;
    Vec2 GetTextSize(const std::string& text, float scale = 1.0f) override;

    // Draw a filled rectangle
    void DrawRect(const Vec2& position, const Vec2& size, const Color& color) override;

    // Draw a rectangle with vertical gradient (top to bottom)
    void DrawRectGradient(const Vec2& position, const Vec2& size,
                         const Color& top_color, const Color& bottom_color) override;

    // Draw a rectangle with vertical gradient and rounded corners
    void DrawRectGradientRounded(const Vec2& position, const Vec2& size,
                                const Color& top_color, const Color& bottom_color,
                                float corner_radius = 5.0f) override;

    // Draw a rectangle with border
    void DrawRectWithBorder(const Vec2& position, const Vec2& size,
                           const Color& fill_color, const Color& border_color,
                           float border_width = 1.0f) override;

    // Draw a rectangle with rounded border
    void DrawRectWithRoundedBorder(const Vec2& position, const Vec2& size,
                                  const Color& fill_color, const Color& border_color,
                                  float border_width = 1.0f, float corner_radius = 5.0f) override;
    
    // Batch drawing for better performance
    void BeginBatch() override;
    void EndBatch() override;
    
    // Frame operations for video recording
    void BeginFrame() override;
    void EndFrame() override;
    
    // Offscreen rendering for headless mode
    bool CreateOffscreenFramebuffer(int width, int height) override;
    void BindOffscreenFramebuffer() override;
    void UnbindOffscreenFramebuffer() override;
    std::vector<uint8_t> ReadFramebuffer(int width, int height) override;
    
    // GPU-optimized frame capture with PBO
    bool InitializePBO(int width, int height) override;
    void CleanupPBO() override;
    std::vector<uint8_t> ReadFramebufferPBO(int width, int height) override;
    void StartAsyncReadback(int width, int height) override;
    std::vector<uint8_t> GetAsyncReadbackResult(int width, int height) override;

    // Preview rendering
    void RenderOffscreenTextureToScreen(int screen_width, int screen_height) override;
    void RenderPreviewOverlay(int screen_width, int screen_height,
                              const std::vector<std::string>& info_lines,
                              float progress_ratio) override;
    
    // Convert screen coordinates to OpenGL coordinates
    Vec2 ScreenToGL(const Vec2& screen_pos) const override;
    Vec2 GLToScreen(const Vec2& gl_pos) const override;
    
    // Draw call statistics
    void ResetDrawCallCount() override;
    unsigned int GetDrawCallCount() const override;

    const char* GetName() const override { return "OpenGL"; }

private:
    int window_width_;
    int window_height_;
    std::vector<Rect> batch_rects_;

    unsigned int draw_call_count_;

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

    void IncrementDrawCallCount();
};
