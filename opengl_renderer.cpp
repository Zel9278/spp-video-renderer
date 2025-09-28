#include <glad/glad.h>
#include "opengl_renderer.h"
#include "simple_bitmap_font.h"
#include <iostream>
#include <cmath>
#include <vector>
#include <cstring>

OpenGLRenderer::OpenGLRenderer() 
        : window_width_(800), window_height_(600), 
            draw_call_count_(0),
            framebuffer_(0), color_texture_(0), depth_renderbuffer_(0), offscreen_initialized_(false),
      current_pbo_index_(0), pbo_initialized_(false) {
    pbo_[0] = 0;
    pbo_[1] = 0;
}

OpenGLRenderer::~OpenGLRenderer() {
    // Cleanup PBO
    CleanupPBO();
    
    // Cleanup offscreen framebuffer
    if (offscreen_initialized_) {
        if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
        if (color_texture_) glDeleteTextures(1, &color_texture_);
        if (depth_renderbuffer_) glDeleteRenderbuffers(1, &depth_renderbuffer_);
    }
}

void OpenGLRenderer::Initialize(int window_width, int window_height) {
    window_width_ = window_width;
    window_height_ = window_height;
    
    // Enable blending for transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Create offscreen framebuffer for headless rendering
    CreateOffscreenFramebuffer(window_width, window_height);
    
    SetupProjection();
}

void OpenGLRenderer::SetViewport(int width, int height) {
    window_width_ = width;
    window_height_ = height;
    glViewport(0, 0, width, height);
    SetupProjection();
}

void OpenGLRenderer::ResetDrawCallCount() {
    draw_call_count_ = 0;
}

unsigned int OpenGLRenderer::GetDrawCallCount() const {
    return draw_call_count_;
}

void OpenGLRenderer::IncrementDrawCallCount() {
    ++draw_call_count_;
}

void OpenGLRenderer::Clear(const Color& clear_color) {
    glClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a);
    glClear(GL_COLOR_BUFFER_BIT);
}

bool OpenGLRenderer::LoadFont(float font_size) {
    (void)font_size;
    // The renderer uses an embedded bitmap font; nothing to load.
    return true;
}

void OpenGLRenderer::ClearWithImage(const std::string& image_path, float opacity, int scale_mode) {
    (void)image_path;
    (void)opacity;
    (void)scale_mode;
    // Image background rendering is not implemented; fall back to solid clear.
    Clear(Color(0.0f, 0.0f, 0.0f, 1.0f));
}

void OpenGLRenderer::ClearWithRadialGradient(const Color& center_color, const Color& edge_color) {
    // Clear with edge color first
    glClearColor(edge_color.r, edge_color.g, edge_color.b, edge_color.a);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw radial gradient using triangle fan (elliptical to match window aspect ratio)
    const int segments = 100; // Number of segments for smooth ellipse
    const float center_x = window_width_ * 0.5f;
    const float center_y = window_height_ * 0.5f;
    const float radius_x = window_width_ * 0.7f;  // Horizontal radius
    const float radius_y = window_height_ * 0.7f; // Vertical radius

    IncrementDrawCallCount();
    glBegin(GL_TRIANGLE_FAN);
    
    // Center vertex
    glColor4f(center_color.r, center_color.g, center_color.b, center_color.a);
    glVertex2f(center_x, center_y);
    
    // Edge vertices
    glColor4f(edge_color.r, edge_color.g, edge_color.b, edge_color.a);
    for (int i = 0; i <= segments; i++) {
        float angle = 2.0f * 3.14159265359f * i / segments;
        float x = center_x + radius_x * cos(angle);
        float y = center_y + radius_y * sin(angle);
        glVertex2f(x, y);
    }
    
    glEnd();
}

void OpenGLRenderer::DrawRect(const Vec2& position, const Vec2& size, const Color& color) {
    glColor4f(color.r, color.g, color.b, color.a);
    
    IncrementDrawCallCount();
    glBegin(GL_QUADS);
    glVertex2f(position.x, position.y);
    glVertex2f(position.x + size.x, position.y);
    glVertex2f(position.x + size.x, position.y + size.y);
    glVertex2f(position.x, position.y + size.y);
    glEnd();
}

void OpenGLRenderer::DrawRectGradient(const Vec2& position, const Vec2& size,
                                     const Color& top_color, const Color& bottom_color) {
    IncrementDrawCallCount();
    glBegin(GL_QUADS);
    // Top-left
    glColor4f(top_color.r, top_color.g, top_color.b, top_color.a);
    glVertex2f(position.x, position.y);
    // Top-right
    glColor4f(top_color.r, top_color.g, top_color.b, top_color.a);
    glVertex2f(position.x + size.x, position.y);
    // Bottom-right
    glColor4f(bottom_color.r, bottom_color.g, bottom_color.b, bottom_color.a);
    glVertex2f(position.x + size.x, position.y + size.y);
    // Bottom-left
    glColor4f(bottom_color.r, bottom_color.g, bottom_color.b, bottom_color.a);
    glVertex2f(position.x, position.y + size.y);
    glEnd();
}

void OpenGLRenderer::DrawRectGradientRounded(const Vec2& position, const Vec2& size,
                                           const Color& top_color, const Color& bottom_color, 
                                           float corner_radius) {
    static int draw_count = 0;
    draw_count++;
    if (draw_count <= 5) {
        std::cout << "DrawRectGradientRounded #" << draw_count << ": pos(" << position.x << "," << position.y 
                  << "), size(" << size.x << "," << size.y << "), colors(" 
                  << top_color.r << "," << top_color.g << "," << top_color.b << ")" << std::endl;
    }
    
    const int segments = 8; // Number of segments for quarter circle
    const float pi = 3.14159265359f;

    // Ensure corner radius doesn't exceed half the width or height
    float max_radius = std::min(size.x, size.y) * 0.5f;
    corner_radius = std::min(corner_radius, max_radius);

    IncrementDrawCallCount();
    glBegin(GL_TRIANGLE_FAN);

    // Center point for the fan
    Vec2 center = Vec2(position.x + size.x * 0.5f, position.y + size.y * 0.5f);
    Color center_color = Color(
        (top_color.r + bottom_color.r) * 0.5f,
        (top_color.g + bottom_color.g) * 0.5f,
        (top_color.b + bottom_color.b) * 0.5f,
        (top_color.a + bottom_color.a) * 0.5f
    );
    glColor4f(center_color.r, center_color.g, center_color.b, center_color.a);
    glVertex2f(center.x, center.y);

    // Draw the rounded rectangle as a triangle fan
    // Start from top-left corner and go clockwise

    // Top-left corner (rounded)
    for (int i = 0; i <= segments; i++) {
        float angle = pi + (pi * 0.5f * i) / segments;
        float x = position.x + corner_radius + corner_radius * cos(angle);
        float y = position.y + corner_radius + corner_radius * sin(angle);
        
        // Interpolate color based on y position
        float t = (y - position.y) / size.y;
        Color color = Color(
            top_color.r + (bottom_color.r - top_color.r) * t,
            top_color.g + (bottom_color.g - top_color.g) * t,
            top_color.b + (bottom_color.b - top_color.b) * t,
            top_color.a + (bottom_color.a - top_color.a) * t
        );
        glColor4f(color.r, color.g, color.b, color.a);
        glVertex2f(x, y);
    }

    // Top edge
    glColor4f(top_color.r, top_color.g, top_color.b, top_color.a);
    glVertex2f(position.x + size.x - corner_radius, position.y);

    // Top-right corner (rounded)
    for (int i = 0; i <= segments; i++) {
        float angle = 1.5f * pi + (pi * 0.5f * i) / segments;
        float x = position.x + size.x - corner_radius + corner_radius * cos(angle);
        float y = position.y + corner_radius + corner_radius * sin(angle);
        
        // Interpolate color based on y position
        float t = (y - position.y) / size.y;
        Color color = Color(
            top_color.r + (bottom_color.r - top_color.r) * t,
            top_color.g + (bottom_color.g - top_color.g) * t,
            top_color.b + (bottom_color.b - top_color.b) * t,
            top_color.a + (bottom_color.a - top_color.a) * t
        );
        glColor4f(color.r, color.g, color.b, color.a);
        glVertex2f(x, y);
    }

    // Right edge
    glColor4f(bottom_color.r, bottom_color.g, bottom_color.b, bottom_color.a);
    glVertex2f(position.x + size.x, position.y + size.y - corner_radius);

    // Bottom-right corner (rounded)
    for (int i = 0; i <= segments; i++) {
        float angle = 0.0f + (pi * 0.5f * i) / segments;
        float x = position.x + size.x - corner_radius + corner_radius * cos(angle);
        float y = position.y + size.y - corner_radius + corner_radius * sin(angle);
        
        // Interpolate color based on y position
        float t = (y - position.y) / size.y;
        Color color = Color(
            top_color.r + (bottom_color.r - top_color.r) * t,
            top_color.g + (bottom_color.g - top_color.g) * t,
            top_color.b + (bottom_color.b - top_color.b) * t,
            top_color.a + (bottom_color.a - top_color.a) * t
        );
        glColor4f(color.r, color.g, color.b, color.a);
        glVertex2f(x, y);
    }

    // Bottom edge
    glColor4f(bottom_color.r, bottom_color.g, bottom_color.b, bottom_color.a);
    glVertex2f(position.x + corner_radius, position.y + size.y);

    // Bottom-left corner (rounded)
    for (int i = 0; i <= segments; i++) {
        float angle = 0.5f * pi + (pi * 0.5f * i) / segments;
        float x = position.x + corner_radius + corner_radius * cos(angle);
        float y = position.y + size.y - corner_radius + corner_radius * sin(angle);
        
        // Interpolate color based on y position
        float t = (y - position.y) / size.y;
        Color color = Color(
            top_color.r + (bottom_color.r - top_color.r) * t,
            top_color.g + (bottom_color.g - top_color.g) * t,
            top_color.b + (bottom_color.b - top_color.b) * t,
            top_color.a + (bottom_color.a - top_color.a) * t
        );
        glColor4f(color.r, color.g, color.b, color.a);
        glVertex2f(x, y);
    }

    // Left edge
    glColor4f(top_color.r, top_color.g, top_color.b, top_color.a);
    glVertex2f(position.x, position.y + corner_radius);

    glEnd();
}

void OpenGLRenderer::DrawRectWithBorder(const Vec2& position, const Vec2& size,
                                       const Color& fill_color, const Color& border_color,
                                       float border_width) {
    // Draw filled rectangle only if not transparent
    if (fill_color.a > 0.0f) {
        DrawRect(position, size, fill_color);
    }

    // Draw border with smooth lines
    glColor4f(border_color.r, border_color.g, border_color.b, border_color.a);
    glLineWidth(border_width);

    // Enable line smoothing for rounded appearance
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    IncrementDrawCallCount();
    glBegin(GL_LINE_LOOP);
    glVertex2f(position.x, position.y);
    glVertex2f(position.x + size.x, position.y);
    glVertex2f(position.x + size.x, position.y + size.y);
    glVertex2f(position.x, position.y + size.y);
    glEnd();

    glDisable(GL_LINE_SMOOTH);
}

void OpenGLRenderer::DrawRectWithRoundedBorder(const Vec2& position, const Vec2& size,
                                              const Color& fill_color, const Color& border_color,
                                              float border_width, float corner_radius) {
    // Skip fill if transparent (let the gradient handle the fill)
    if (fill_color.a > 0.0f) {
        DrawRectGradientRounded(position, size, fill_color, fill_color, corner_radius);
    }

    // Draw rounded border
    glColor4f(border_color.r, border_color.g, border_color.b, border_color.a);
    glLineWidth(border_width);

    // Enable line smoothing for better appearance
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    const int segments = 8; // Number of segments for quarter circle
    const float pi = 3.14159265359f;

    IncrementDrawCallCount();
    glBegin(GL_LINE_STRIP);

    // Top-left corner
    for (int i = 0; i <= segments; i++) {
        float angle = pi + (pi * 0.5f * i) / segments;
        float x = position.x + corner_radius + corner_radius * cos(angle);
        float y = position.y + corner_radius + corner_radius * sin(angle);
        glVertex2f(x, y);
    }

    // Top-right corner
    for (int i = 0; i <= segments; i++) {
        float angle = 1.5f * pi + (pi * 0.5f * i) / segments;
        float x = position.x + size.x - corner_radius + corner_radius * cos(angle);
        float y = position.y + corner_radius + corner_radius * sin(angle);
        glVertex2f(x, y);
    }

    // Bottom-right corner
    for (int i = 0; i <= segments; i++) {
        float angle = 0.0f + (pi * 0.5f * i) / segments;
        float x = position.x + size.x - corner_radius + corner_radius * cos(angle);
        float y = position.y + size.y - corner_radius + corner_radius * sin(angle);
        glVertex2f(x, y);
    }

    // Bottom-left corner
    for (int i = 0; i <= segments; i++) {
        float angle = 0.5f * pi + (pi * 0.5f * i) / segments;
        float x = position.x + corner_radius + corner_radius * cos(angle);
        float y = position.y + size.y - corner_radius + corner_radius * sin(angle);
        glVertex2f(x, y);
    }

    // Close the loop
    float angle = pi;
    float x = position.x + corner_radius + corner_radius * cos(angle);
    float y = position.y + corner_radius + corner_radius * sin(angle);
    glVertex2f(x, y);

    glEnd();
    glDisable(GL_LINE_SMOOTH);
}

void OpenGLRenderer::BeginBatch() {
    batch_rects_.clear();
}

void OpenGLRenderer::EndBatch() {
    // For this simple implementation, just render immediately
    // In a more complex version, we could sort and optimize the batch
    for (const auto& rect : batch_rects_) {
        DrawRect(rect.position, rect.size, rect.color);
    }
    batch_rects_.clear();
}

void OpenGLRenderer::SetupProjection() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    
    // Use screen coordinates (0,0 top-left to width,height bottom-right)
    glOrtho(0.0, window_width_, window_height_, 0.0, -1.0, 1.0);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

// Stub functions for compatibility
bool OpenGLRenderer::LoadImageTexture(const std::string& path, BackgroundImage& image) {
    return false;
}

void OpenGLRenderer::DrawImageBackground(const BackgroundImage& image, float opacity, int scale_mode) {
    // Not implemented
}

void OpenGLRenderer::LoadFontTexture() {
    // Simple bitmap font - no texture needed
}

void OpenGLRenderer::DrawText(const std::string& text, const Vec2& position, const Color& color, float scale) {
    float char_width = (simple_font::kGlyphWidth + 1) * scale;
    float char_height = simple_font::kGlyphHeight * scale;
    float pixel_size = 1.0f * scale;
    float line_spacing = 2.0f * scale;
    
    float current_x = position.x;
    float current_y = position.y;
    
    for (size_t i = 0; i < text.length(); i++) {
        char c = text[i];
        
        if (c == '\n') {
            current_x = position.x;
            current_y += char_height + line_spacing; // Line spacing
            continue;
        }
        
        // Handle printable ASCII characters (32-122: space to z)
        if (const unsigned char* bitmap = simple_font::GlyphData(c)) {
            
            // Draw each bit in the 5x8 character bitmap
            for (int row = 0; row < simple_font::kGlyphHeight; row++) {
                for (int col = 0; col < simple_font::kGlyphWidth; col++) {
                    if (bitmap[row] & (1 << (4 - col))) {
                        Vec2 pixel_pos(current_x + col * pixel_size, 
                                     current_y + row * pixel_size);
                        Vec2 pixel_size_vec(pixel_size, pixel_size);
                        DrawRect(pixel_pos, pixel_size_vec, color);
                    }
                }
            }
        }
        
        current_x += char_width;
    }
}

Vec2 OpenGLRenderer::GetTextSize(const std::string& text, float scale) {
    float char_width = (simple_font::kGlyphWidth + 1) * scale;
    float line_spacing = 2.0f * scale;
    float char_height = simple_font::kGlyphHeight * scale;
    
    size_t max_line_length = 0;
    size_t current_line_length = 0;
    int line_count = 1;
    
    for (char c : text) {
        if (c == '\n') {
            max_line_length = std::max(max_line_length, current_line_length);
            current_line_length = 0;
            line_count++;
        } else {
            current_line_length++;
        }
    }
    max_line_length = std::max(max_line_length, current_line_length);
    
    float width = max_line_length * char_width;
    float height = line_count * char_height + (line_count - 1) * line_spacing;
    return Vec2(width, height);
}

void OpenGLRenderer::RenderText(const std::string& text, float x, float y, float size, const Color& color) {
    DrawText(text, Vec2(x, y), color, size);
}

// Frame operations for video recording
void OpenGLRenderer::BeginFrame() {
    ResetDrawCallCount();
    BindOffscreenFramebuffer();
}

void OpenGLRenderer::EndFrame() {
    // Keep offscreen framebuffer bound for frame capture
}

// Offscreen rendering implementation
bool OpenGLRenderer::CreateOffscreenFramebuffer(int width, int height) {
    // Generate framebuffer
    glGenFramebuffers(1, &framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    
    // Create color texture
    glGenTextures(1, &color_texture_);
    glBindTexture(GL_TEXTURE_2D, color_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // Attach color texture to framebuffer
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture_, 0);
    
    // Create depth renderbuffer
    glGenRenderbuffers(1, &depth_renderbuffer_);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_renderbuffer_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    
    // Attach depth renderbuffer to framebuffer
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_renderbuffer_);
    
    // Check framebuffer completeness
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Failed to create offscreen framebuffer: " << status << std::endl;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    offscreen_initialized_ = true;
    
    std::cout << "Offscreen framebuffer created: " << width << "x" << height << std::endl;
    return true;
}

void OpenGLRenderer::BindOffscreenFramebuffer() {
    if (offscreen_initialized_) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glViewport(0, 0, window_width_, window_height_);
    }
}

void OpenGLRenderer::UnbindOffscreenFramebuffer() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

std::vector<uint8_t> OpenGLRenderer::ReadFramebuffer(int width, int height) {
    std::vector<uint8_t> pixels(width * height * 4); // RGBA
    
    // Bind the offscreen framebuffer to read from it
    BindOffscreenFramebuffer();
    
    // Read pixels from the framebuffer
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    
    // Flip the image vertically (OpenGL has origin at bottom-left)
    std::vector<uint8_t> flipped_pixels(width * height * 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int src_idx = ((height - 1 - y) * width + x) * 4;
            int dst_idx = (y * width + x) * 4;
            flipped_pixels[dst_idx + 0] = pixels[src_idx + 0]; // R
            flipped_pixels[dst_idx + 1] = pixels[src_idx + 1]; // G
            flipped_pixels[dst_idx + 2] = pixels[src_idx + 2]; // B
            flipped_pixels[dst_idx + 3] = pixels[src_idx + 3]; // A
        }
    }
    
    return flipped_pixels;
}

// GPU-optimized frame capture using PBO (Pixel Buffer Objects)
bool OpenGLRenderer::InitializePBO(int width, int height) {
    if (pbo_initialized_) {
        CleanupPBO();
    }
    
    // Create two PBOs for double buffering
    glGenBuffers(2, pbo_);
    
    size_t buffer_size = width * height * 4; // RGBA
    
    // Initialize both PBOs
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, buffer_size, nullptr, GL_STREAM_READ);
    }
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    
    current_pbo_index_ = 0;
    pbo_initialized_ = true;
    
    std::cout << "PBO initialized for " << width << "x" << height << " framebuffer capture" << std::endl;
    return true;
}

void OpenGLRenderer::CleanupPBO() {
    if (pbo_initialized_) {
        glDeleteBuffers(2, pbo_);
        pbo_[0] = 0;
        pbo_[1] = 0;
        pbo_initialized_ = false;
    }
}

// Asynchronous GPU-optimized readback with double-buffered PBO
std::vector<uint8_t> OpenGLRenderer::ReadFramebufferPBO(int width, int height) {
    if (!pbo_initialized_) {
        InitializePBO(width, height);
    }
    
    BindOffscreenFramebuffer();
    
    // Use double buffering: read from previous frame's PBO while writing to current PBO
    int read_pbo = current_pbo_index_;
    int write_pbo = 1 - current_pbo_index_;
    
    // Start async readback to write PBO
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[write_pbo]);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    
    // Read from the previous frame's PBO (if available)
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[read_pbo]);
    void* mapped_buffer = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    
    std::vector<uint8_t> result;
    if (mapped_buffer) {
        size_t buffer_size = width * height * 4;
        result.resize(buffer_size);
        
        // Fast GPU-assisted vertical flip using pointer arithmetic
        uint8_t* src = static_cast<uint8_t*>(mapped_buffer);
        uint8_t* dst = result.data();
        
        size_t row_size = width * 4; // RGBA
        
        // Flip vertically (OpenGL origin is bottom-left, we need top-left)
        for (int y = 0; y < height; y++) {
            uint8_t* src_row = src + ((height - 1 - y) * row_size);
            uint8_t* dst_row = dst + (y * row_size);
            std::memcpy(dst_row, src_row, row_size);
        }
        
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    } else {
        // Fallback to synchronous read if mapping fails
        std::cerr << "PBO mapping failed, falling back to synchronous read" << std::endl;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return ReadFramebuffer(width, height);
    }
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    
    // Swap PBO indices for next frame
    current_pbo_index_ = write_pbo;
    
    return result;
}

// For even more advanced async operation
void OpenGLRenderer::StartAsyncReadback(int width, int height) {
    if (!pbo_initialized_) {
        InitializePBO(width, height);
    }
    
    BindOffscreenFramebuffer();
    
    // Start async readback
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[current_pbo_index_]);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

std::vector<uint8_t> OpenGLRenderer::GetAsyncReadbackResult(int width, int height) {
    if (!pbo_initialized_) {
        return {}; // No async operation started
    }
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[current_pbo_index_]);
    void* mapped_buffer = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    
    std::vector<uint8_t> result;
    if (mapped_buffer) {
        size_t buffer_size = width * height * 4;
        result.resize(buffer_size);
        
        // Fast vertical flip
        uint8_t* src = static_cast<uint8_t*>(mapped_buffer);
        uint8_t* dst = result.data();
        size_t row_size = width * 4;
        
        for (int y = 0; y < height; y++) {
            std::memcpy(dst + (y * row_size), 
                       src + ((height - 1 - y) * row_size), 
                       row_size);
        }
        
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    
    // Switch to next PBO for next frame
    current_pbo_index_ = 1 - current_pbo_index_;
    
    return result;
}

void OpenGLRenderer::RenderOffscreenTextureToScreen(int screen_width, int screen_height) {
    if (!offscreen_initialized_ || color_texture_ == 0 || window_height_ == 0 || screen_height == 0) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screen_width, screen_height);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(screen_width), static_cast<double>(screen_height), 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // Calculate letterboxed viewport to maintain aspect ratio
    float texture_aspect = static_cast<float>(window_width_) / static_cast<float>(window_height_);
    float screen_aspect = static_cast<float>(screen_width) / static_cast<float>(screen_height);

    float target_width = static_cast<float>(screen_width);
    float target_height = static_cast<float>(screen_height);

    if (screen_aspect > texture_aspect) {
        target_width = target_height * texture_aspect;
    } else {
        target_height = target_width / texture_aspect;
    }

    float x_offset = (static_cast<float>(screen_width) - target_width) * 0.5f;
    float y_offset = (static_cast<float>(screen_height) - target_height) * 0.5f;

    GLboolean blend_enabled = glIsEnabled(GL_BLEND);
    GLboolean depth_enabled = glIsEnabled(GL_DEPTH_TEST);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, color_texture_);

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    IncrementDrawCallCount();
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x_offset, y_offset);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x_offset + target_width, y_offset);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x_offset + target_width, y_offset + target_height);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x_offset, y_offset + target_height);
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);

    if (depth_enabled) {
        glEnable(GL_DEPTH_TEST);
    }
    if (blend_enabled) {
        glEnable(GL_BLEND);
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

void OpenGLRenderer::RenderPreviewOverlay(int screen_width, int screen_height,
                                          const std::vector<std::string>& info_lines,
                                          float progress_ratio) {
    if (screen_width <= 0 || screen_height <= 0 || info_lines.empty()) {
        return;
    }

    float clamped_progress = std::clamp(progress_ratio, 0.0f, 1.0f);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(screen_width), static_cast<double>(screen_height), 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    GLboolean depth_enabled = glIsEnabled(GL_DEPTH_TEST);
    if (depth_enabled) {
        glDisable(GL_DEPTH_TEST);
    }

    GLboolean texture_enabled = glIsEnabled(GL_TEXTURE_2D);
    if (texture_enabled) {
        glDisable(GL_TEXTURE_2D);
    }

    GLboolean blend_enabled = glIsEnabled(GL_BLEND);
    if (!blend_enabled) {
        glEnable(GL_BLEND);
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const float padding = 14.0f;
    const float line_height = 22.0f;
    const float bar_height = 12.0f;
    const float bar_spacing = 10.0f;
    const float panel_width = 460.0f;

    float text_height = static_cast<float>(info_lines.size()) * line_height;
    float panel_height = padding + text_height + bar_spacing + bar_height + padding;

    Vec2 panel_pos(18.0f, 18.0f);

    DrawRectWithBorder(panel_pos,
                       Vec2(panel_width, panel_height),
                       Color(0.05f, 0.05f, 0.05f, 0.75f),
                       Color(1.0f, 1.0f, 1.0f, 0.85f),
                       2.0f);

    Color text_color(1.0f, 1.0f, 1.0f, 0.95f);
    Vec2 text_pos(panel_pos.x + padding, panel_pos.y + padding);

    for (size_t i = 0; i < info_lines.size(); ++i) {
        Vec2 line_pos(text_pos.x, text_pos.y + static_cast<float>(i) * line_height);
        DrawText(info_lines[i], line_pos, text_color, 1.6f);
    }

    float bar_width = panel_width - padding * 2.0f;
    Vec2 bar_pos(panel_pos.x + padding,
                 panel_pos.y + padding + text_height + bar_spacing * 0.5f);

    Color bar_bg(0.15f, 0.15f, 0.15f, 0.9f);
    Color bar_fill(0.18f, 0.55f, 0.95f, 0.95f);
    Color bar_border(1.0f, 1.0f, 1.0f, 0.8f);

    DrawRect(bar_pos, Vec2(bar_width, bar_height), bar_bg);
    DrawRect(Vec2(bar_pos.x, bar_pos.y),
             Vec2(bar_width * clamped_progress, bar_height), bar_fill);

    DrawRectWithBorder(bar_pos, Vec2(bar_width, bar_height),
                       Color(0.0f, 0.0f, 0.0f, 0.0f), bar_border, 1.5f);

    if (!blend_enabled) {
        glDisable(GL_BLEND);
    }
    if (texture_enabled) {
        glEnable(GL_TEXTURE_2D);
    }
    if (depth_enabled) {
        glEnable(GL_DEPTH_TEST);
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

Vec2 OpenGLRenderer::ScreenToGL(const Vec2& screen_pos) const {
    // The renderer operates in screen coordinate space (origin at top-left),
    // so no conversion is necessary.
    return screen_pos;
}

Vec2 OpenGLRenderer::GLToScreen(const Vec2& gl_pos) const {
    return gl_pos;
}