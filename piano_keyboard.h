#pragma once

#include <vector>
#include <chrono>
#include "opengl_renderer.h"

// Options structure for video output settings
struct PianoOptions {
    // All visual colors are now fixed:
    // White keys: Pure white (255, 255, 255)
    // Black keys: Pure black (0, 0, 0) 
    // Border: Dark gray (10, 10, 10)
    
    // Animation settings
    float key_press_animation_ms = 150.0f;
    float key_release_animation_ms = 200.0f;
    float key_press_scale = 0.95f;
    float key_press_y_offset = 2.0f;
    
    // UI settings (for debugging only)
    bool show_debug_info = false;
};

struct KeyBlip {
    std::chrono::steady_clock::time_point time;  // When the blip was created
    Color color;                                 // Color of the blip
    float y_offset;                             // Vertical offset from key position
};

struct PianoKey {
    int note;           // MIDI note number (0-127)
    bool is_black;      // true if black key, false if white key
    bool is_pressed;    // true if key is currently pressed (mouse input)
    Vec2 position;      // Position on screen
    Vec2 size;          // Size of the key
    Color color;        // Current color of the key
    std::vector<KeyBlip> blips;  // Visual effect blips for this key
    std::chrono::steady_clock::time_point time_played;  // Last time this key was played

    // Animation properties
    bool was_pressed;   // Previous frame pressed state for detecting press/release
    std::chrono::steady_clock::time_point press_time;    // When key was pressed
    std::chrono::steady_clock::time_point release_time;  // When key was released
    float animation_progress; // 0.0 to 1.0, animation progress
    bool is_animating;  // true if currently animating
};

class PianoKeyboard {
public:
    PianoKeyboard();
    ~PianoKeyboard();

    // Initialize the keyboard with 128 keys (full MIDI range: C-1 to G9, MIDI notes 0-127)
    void Initialize();
    
    // Update keyboard state
    void Update();
    
    // Render the keyboard using OpenGL
    void Render(OpenGLRenderer& renderer);

    // Handle mouse input
    void HandleInput(double mouse_x, double mouse_y, bool mouse_is_down);
    
    // Get/Set key state
    bool IsKeyPressed(int note) const;
    void SetKeyPressed(int note, bool pressed);
    
    // Configuration
    void SetKeyboardPosition(const Vec2& position);
    void SetKeyboardSize(const Vec2& size);
    void SetWhiteKeySize(const Vec2& size);
    void SetBlackKeySize(const Vec2& size);

    // Auto layout based on window size
    void UpdateLayout(int window_width, int window_height);
    void SetAutoLayout(bool enabled);
    void SetKeyboardMargin(float margin);
    
    // Get keyboard info
    int GetPressedKeyCount() const;
    std::vector<int> GetPressedKeys() const;
    int GetTotalBlipCount() const;

    // Visual effects
    void AddKeyBlip(int note, const Color& color);
    void UpdateBlips();
    void UpdateKeyAnimations();

private:
    std::vector<PianoKey> keys_;
    Vec2 keyboard_position_;
    Vec2 keyboard_size_;
    Vec2 white_key_size_;
    Vec2 black_key_size_;

    // Auto layout settings
    bool auto_layout_enabled_;
    float keyboard_margin_;
    int current_window_width_;
    int current_window_height_;

    // Colors
    Color white_key_color_;
    Color black_key_color_;
    Color key_border_color_;

    // Mouse input tracking
    int last_hovered_key_;

    // Blip effect settings
    float white_blip_width_;
    float white_blip_height_;
    float white_blip_x_offset_;
    float white_blip_y_offset_;
    float black_blip_width_;
    float black_blip_height_;
    float black_blip_x_offset_;
    float black_blip_y_offset_;
    float blip_fade_duration_ms_;
    float blip_spacing_factor_;

    // Key animation settings
    float key_press_animation_duration_ms_;  // Duration of press animation
    float key_release_animation_duration_ms_; // Duration of release animation
    float key_press_scale_factor_;           // How much to scale key when pressed (0.9 = 90% size)
    float key_press_y_offset_;               // How much to move key down when pressed

    // Options
    PianoOptions options_;
    
    // Helper functions
    bool IsBlackKey(int note) const;
    void CalculateKeyPositions();
    int GetWhiteKeyIndex(int note) const;
    void RenderWhiteKeys(OpenGLRenderer& renderer);
    void RenderBlackKeys(OpenGLRenderer& renderer);
    void RenderWhiteKeyBlips(OpenGLRenderer& renderer);
    void RenderBlackKeyBlips(OpenGLRenderer& renderer);
    int GetKeyAtPosition(const Vec2& pos) const;

    // Layout calculation helpers
    void CalculateAutoLayout(int window_width, int window_height);
    int GetTotalWhiteKeys() const;
};
