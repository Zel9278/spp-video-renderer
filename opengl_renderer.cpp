#include <glad/glad.h>
#include "opengl_renderer.h"
#include <iostream>
#include <cmath>
#include <vector>
#include <cstring>

OpenGLRenderer::OpenGLRenderer() 
    : window_width_(800), window_height_(600), 
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
    // Simple bitmap font - no texture needed
}

// Simple 5x8 bitmap font data for ASCII characters (32-122)
static const unsigned char font_5x8[][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // 32: space
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04, 0x00}, // 33: !
    {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00}, // 34: "
    {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A, 0x00}, // 35: #
    {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04, 0x00}, // 36: $
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03, 0x00}, // 37: %
    {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D, 0x00}, // 38: &
    {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00}, // 39: '
    {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02, 0x00}, // 40: (
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08, 0x00}, // 41: )
    {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00, 0x00}, // 42: *
    {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00, 0x00}, // 43: +
    {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08, 0x00}, // 44: ,
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00}, // 45: -
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, // 46: .
    {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}, // 47: /
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E, 0x00}, // 48: 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00}, // 49: 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F, 0x00}, // 50: 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E, 0x00}, // 51: 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02, 0x00}, // 52: 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E, 0x00}, // 53: 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E, 0x00}, // 54: 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08, 0x00}, // 55: 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E, 0x00}, // 56: 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C, 0x00}, // 57: 9
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00, 0x00}, // 58: :
    {0x00, 0x0C, 0x0C, 0x00, 0x04, 0x04, 0x08, 0x00}, // 59: ;
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02, 0x00}, // 60: <
    {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00}, // 61: =
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08, 0x00}, // 62: >
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04, 0x00}, // 63: ?
    {0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E, 0x00}, // 64: @
    {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x00}, // 65: A
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E, 0x00}, // 66: B
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E, 0x00}, // 67: C
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C, 0x00}, // 68: D
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F, 0x00}, // 69: E
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x00}, // 70: F
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F, 0x00}, // 71: G
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x00}, // 72: H
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00}, // 73: I
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C, 0x00}, // 74: J
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11, 0x00}, // 75: K
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F, 0x00}, // 76: L
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11, 0x00}, // 77: M
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x00}, // 78: N
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00}, // 79: O
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10, 0x00}, // 80: P
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D, 0x00}, // 81: Q
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11, 0x00}, // 82: R
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E, 0x00}, // 83: S
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00}, // 84: T
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00}, // 85: U
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04, 0x00}, // 86: V
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A, 0x00}, // 87: W
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11, 0x00}, // 88: X
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x00}, // 89: Y
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F, 0x00}, // 90: Z
    {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E, 0x00}, // 91: [
    {0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00}, // 92: backslash
    {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E, 0x00}, // 93: ]
    {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00}, // 94: ^
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x00}, // 95: _
    {0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00}, // 96: `
    {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F, 0x00}, // 97: a
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E, 0x00}, // 98: b
    {0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E, 0x00}, // 99: c
    {0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F, 0x00}, // 100: d
    {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E, 0x00}, // 101: e
    {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08, 0x00}, // 102: f
    {0x00, 0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}, // 103: g
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11, 0x00}, // 104: h
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E, 0x00}, // 105: i
    {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C, 0x00}, // 106: j
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12, 0x00}, // 107: k
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00}, // 108: l
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11, 0x00}, // 109: m
    {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11, 0x00}, // 110: n
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00}, // 111: o
    {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10, 0x00}, // 112: p
    {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01, 0x00}, // 113: q
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10, 0x00}, // 114: r
    {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E, 0x00}, // 115: s
    {0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06, 0x00}, // 116: t
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D, 0x00}, // 117: u
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04, 0x00}, // 118: v
    {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A, 0x00}, // 119: w
    {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00}, // 120: x
    {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E, 0x00}, // 121: y
    {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F, 0x00}, // 122: z
};

void OpenGLRenderer::DrawText(const std::string& text, const Vec2& position, const Color& color, float scale) {
    float char_width = 6.0f * scale;  // 5 pixels + 1 spacing
    float char_height = 8.0f * scale;
    float pixel_size = 1.0f * scale;
    
    float current_x = position.x;
    float current_y = position.y;
    
    for (size_t i = 0; i < text.length(); i++) {
        char c = text[i];
        
        if (c == '\n') {
            current_x = position.x;
            current_y += char_height + 2.0f * scale; // Line spacing
            continue;
        }
        
        // Handle printable ASCII characters (32-122: space to z)
        if (c >= 32 && c <= 122) {
            const unsigned char* bitmap = font_5x8[c - 32];
            
            // Draw each bit in the 5x8 character bitmap
            for (int row = 0; row < 8; row++) {
                for (int col = 0; col < 5; col++) {
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
    float char_width = 8.0f * scale;
    float char_height = 16.0f * scale;
    
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
    
    return Vec2(max_line_length * char_width, line_count * char_height);
}

void OpenGLRenderer::RenderText(const std::string& text, float x, float y, float size, const Color& color) {
    DrawText(text, Vec2(x, y), color, size);
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