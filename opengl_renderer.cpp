#include <glad/glad.h>
#include "opengl_renderer.h"
#include <iostream>
#include <cmath>
#include <vector>

OpenGLRenderer::OpenGLRenderer() 
    : window_width_(800), window_height_(600), 
      framebuffer_(0), color_texture_(0), depth_renderbuffer_(0), offscreen_initialized_(false) {
}

OpenGLRenderer::~OpenGLRenderer() {
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

void OpenGLRenderer::Clear(const Color& clear_color) {
    glClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a);
    glClear(GL_COLOR_BUFFER_BIT);
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
    
    glBegin(GL_QUADS);
    glVertex2f(position.x, position.y);
    glVertex2f(position.x + size.x, position.y);
    glVertex2f(position.x + size.x, position.y + size.y);
    glVertex2f(position.x, position.y + size.y);
    glEnd();
}

void OpenGLRenderer::DrawRectGradient(const Vec2& position, const Vec2& size,
                                     const Color& top_color, const Color& bottom_color) {
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
    // Not implemented
}

void OpenGLRenderer::RenderText(const std::string& text, float x, float y, float size, const Color& color) {
    // Not implemented
}

// Frame operations for video recording
void OpenGLRenderer::BeginFrame() {
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