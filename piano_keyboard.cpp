#include "piano_keyboard.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <imgui.h>

// Piano keyboard range: Full MIDI range (MIDI notes 0-127, 128 keys total)
constexpr int PIANO_START_NOTE = 0;    // C-1 (lowest MIDI note)
constexpr int PIANO_END_NOTE = 127;    // G9 (highest MIDI note)
constexpr int PIANO_KEY_COUNT = PIANO_END_NOTE - PIANO_START_NOTE + 1;  // 128 keys

PianoKeyboard::PianoKeyboard()
    : keyboard_position_(50.0f, 80.0f)
    , keyboard_size_(1200.0f, 200.0f)
    , white_key_size_(20.0f, 260.0f)
    , black_key_size_(16.0f, 160.0f)
    , auto_layout_enabled_(true)
    , keyboard_margin_(50.0f)
    , current_window_width_(1280)
    , current_window_height_(720)
    , white_key_color_(Color::FromRGB(255, 255, 255))
    , black_key_color_(Color::FromRGB(30, 30, 30))
    , key_border_color_(Color::FromRGB(10, 10, 10))
    , last_hovered_key_(-1)
    , white_blip_width_(0.0f)  // Will be set to match key width
    , white_blip_height_(10.0f)
    , white_blip_x_offset_(0.0f)  // Will be set to match key position
    , white_blip_y_offset_(0.0f)  // Will be set to match key position
    , black_blip_width_(0.0f)  // Will be set to match key width
    , black_blip_height_(8.0f)
    , black_blip_x_offset_(0.0f)  // Will be set to match key position
    , black_blip_y_offset_(0.0f)  // Will be set to match key position
    , blip_fade_duration_ms_(1000.0f)
    , blip_spacing_factor_(1.2f)
    , key_press_animation_duration_ms_(80.0f)   // 80ms animation duration
    , key_release_animation_duration_ms_(90.0f) // 90ms animation duration
    , key_press_scale_factor_(0.95f)             // Scale to 95% when pressed
    , key_press_y_offset_(2.0f)                  // Move 2px down when pressed
    , options_()
{
}

PianoKeyboard::~PianoKeyboard() {
}

void PianoKeyboard::Initialize() {
    keys_.clear();
    keys_.reserve(PIANO_KEY_COUNT);

    // Initialize 128 keys (Full MIDI range: C-1 to G9, MIDI notes 0-127)
    for (int note = PIANO_START_NOTE; note <= PIANO_END_NOTE; ++note) {
        PianoKey key;
        key.note = note;
        key.is_black = IsBlackKey(note);
        key.is_pressed = false;
        key.color = key.is_black ? black_key_color_ : white_key_color_;

        // Initialize animation properties
        key.was_pressed = false;
        key.press_time = std::chrono::steady_clock::now();
        key.release_time = std::chrono::steady_clock::now();
        key.animation_progress = 0.0f;
        key.is_animating = false;

        keys_.push_back(key);
    }

    CalculateKeyPositions();
}

void PianoKeyboard::Update() {
    // Keep all keys at default color (no color change on press)
    for (auto& key : keys_) {
        // Always use default colors, regardless of pressed state
        key.color = key.is_black ? black_key_color_ : white_key_color_;
    }

    // Update blips
    UpdateBlips();

    // Update key animations
    UpdateKeyAnimations();
}

void PianoKeyboard::Render(OpenGLRenderer& renderer) {
    // Layer 1: Render white keys (background)
    RenderWhiteKeys(renderer);

    // Layer 2: Render white key blips
    RenderWhiteKeyBlips(renderer);

    // Layer 3: Render black keys
    RenderBlackKeys(renderer);

    // Layer 4: Render black key blips (top layer)
    RenderBlackKeyBlips(renderer);
}

void PianoKeyboard::HandleInput(double mouse_x, double mouse_y, bool mouse_is_down) {
    // 動画出力のためマウス入力は無効化
    (void)mouse_x;
    (void)mouse_y;
    (void)mouse_is_down;
}

bool PianoKeyboard::IsKeyPressed(int note) const {
    // Find the key in our 128-key range
    if (note >= PIANO_START_NOTE && note <= PIANO_END_NOTE) {
        int index = note - PIANO_START_NOTE;
        if (index >= 0 && index < static_cast<int>(keys_.size())) {
            return keys_[index].is_pressed;
        }
    }
    return false;
}

void PianoKeyboard::SetKeyPressed(int note, bool pressed) {
    // Find the key in our 128-key range
    if (note >= PIANO_START_NOTE && note <= PIANO_END_NOTE) {
        int index = note - PIANO_START_NOTE;
        if (index >= 0 && index < static_cast<int>(keys_.size())) {
            keys_[index].is_pressed = pressed;
        }
    }
}

void PianoKeyboard::SetKeyboardPosition(const Vec2& position) {
    keyboard_position_ = position;
    CalculateKeyPositions();
}

void PianoKeyboard::SetKeyboardSize(const Vec2& size) {
    keyboard_size_ = size;
    CalculateKeyPositions();
}

void PianoKeyboard::SetWhiteKeySize(const Vec2& size) {
    white_key_size_ = size;
    CalculateKeyPositions();
}

void PianoKeyboard::SetBlackKeySize(const Vec2& size) {
    black_key_size_ = size;
    CalculateKeyPositions();
}

void PianoKeyboard::UpdateLayout(int window_width, int window_height) {
    current_window_width_ = window_width;
    current_window_height_ = window_height;

    if (auto_layout_enabled_) {
        CalculateAutoLayout(window_width, window_height);
    }
}

void PianoKeyboard::SetAutoLayout(bool enabled) {
    auto_layout_enabled_ = enabled;
    if (enabled) {
        CalculateAutoLayout(current_window_width_, current_window_height_);
    }
}

void PianoKeyboard::SetKeyboardMargin(float margin) {
    keyboard_margin_ = margin;
    if (auto_layout_enabled_) {
        CalculateAutoLayout(current_window_width_, current_window_height_);
    }
}

int PianoKeyboard::GetPressedKeyCount() const {
    return std::count_if(keys_.begin(), keys_.end(), 
                        [](const PianoKey& key) { return key.is_pressed; });
}

std::vector<int> PianoKeyboard::GetPressedKeys() const {
    std::vector<int> pressed_keys;
    for (const auto& key : keys_) {
        if (key.is_pressed) {
            pressed_keys.push_back(key.note);
        }
    }
    return pressed_keys;
}

int PianoKeyboard::GetTotalBlipCount() const {
    int total_blips = 0;
    for (const auto& key : keys_) {
        total_blips += static_cast<int>(key.blips.size());
    }
    return total_blips;
}

bool PianoKeyboard::IsBlackKey(int note) const {
    int octave_note = note % 12;
    return (octave_note == 1 || octave_note == 3 || octave_note == 6 || 
            octave_note == 8 || octave_note == 10);
}

int PianoKeyboard::GetWhiteKeyIndex(int note) const {
    // Count how many white keys come before this note
    int white_key_count = 0;
    for (int i = 0; i < note; i++) {
        if (!IsBlackKey(i)) {
            white_key_count++;
        }
    }
    return white_key_count;
}

void PianoKeyboard::CalculateKeyPositions() {
    // Calculate white key positions first
    float white_key_x = keyboard_position_.x;
    int white_key_count = 0;

    for (auto& key : keys_) {
        if (!key.is_black) {
            key.position = Vec2(white_key_x, keyboard_position_.y);
            key.size = white_key_size_;
            white_key_x += white_key_size_.x;
            white_key_count++;
        }
    }

    // Calculate black key positions
    for (auto& key : keys_) {
        if (key.is_black) {
            int white_key_index = GetWhiteKeyIndex(key.note);
            if (white_key_index >= 0) {
                float black_key_x = keyboard_position_.x + (white_key_index * white_key_size_.x) - (black_key_size_.x * 0.5f);
                key.position = Vec2(black_key_x, keyboard_position_.y);
                key.size = black_key_size_;
            }
        }
    }
}

void PianoKeyboard::RenderWhiteKeys(OpenGLRenderer& renderer) {
    static bool debug_printed = false;
    static int keys_rendered = 0;
    
    for (const auto& key : keys_) {
        if (!key.is_black) {
            keys_rendered++;
            
            // Debug output (only for first few keys)
            if (!debug_printed || keys_rendered <= 5) {
                std::cout << "Rendering white key " << keys_rendered << " - Position: (" << key.position.x << ", " << key.position.y 
                         << "), Size: (" << key.size.x << ", " << key.size.y << ")" << std::endl;
                if (keys_rendered == 5) debug_printed = true;
            }
            
            // Calculate animated position and size
            Vec2 animated_position = key.position;
            Vec2 animated_size = key.size;

            if (key.is_animating && key.animation_progress > 0.0f) {
                // Apply scale animation
                float scale = 1.0f - (1.0f - key_press_scale_factor_) * key.animation_progress;
                animated_size.x *= scale;
                animated_size.y *= scale;

                // Center the scaled key
                animated_position.x += (key.size.x - animated_size.x) * 0.5f;
                animated_position.y += (key.size.y - animated_size.y) * 0.5f;

                // Apply vertical offset
                animated_position.y += key_press_y_offset_ * key.animation_progress;
            }

            // Render the white key with gradient (fixed colors)
            Color white_top_color = Color::FromRGB(255, 255, 255);    // Pure white
            Color white_bottom_color = Color::FromRGB(240, 240, 240); // Light gray
            
            // No color animation - always use fixed colors
            
            renderer.DrawRectGradientRounded(animated_position, animated_size,
                                    white_top_color, white_bottom_color, 6.0f);

            // Draw border
            renderer.DrawRectWithRoundedBorder(animated_position, animated_size,
                                      Color(0, 0, 0, 0), key_border_color_, 2.5f, 6.0f);
        }
    }
}

void PianoKeyboard::RenderBlackKeys(OpenGLRenderer& renderer) {
    for (const auto& key : keys_) {
        if (key.is_black) {
            // Calculate animated position and size
            Vec2 animated_position = key.position;
            Vec2 animated_size = key.size;

            if (key.is_animating && key.animation_progress > 0.0f) {
                // Apply scale animation
                float scale = 1.0f - (1.0f - key_press_scale_factor_) * key.animation_progress;
                animated_size.x *= scale;
                animated_size.y *= scale;

                // Center the scaled key
                animated_position.x += (key.size.x - animated_size.x) * 0.5f;
                animated_position.y += (key.size.y - animated_size.y) * 0.5f;

                // Apply vertical offset
                animated_position.y += key_press_y_offset_ * key.animation_progress;
            }

            // Render the black key with gradient (fixed colors)
            Color black_top_color = Color::FromRGB(0, 0, 0);       // Pure black
            Color black_bottom_color = Color::FromRGB(68, 68, 68); // Dark gray
            
            // No color animation - always use fixed colors
            
            renderer.DrawRectGradientRounded(animated_position, animated_size,
                                    black_top_color, black_bottom_color, 6.0f);

            // Draw border
            renderer.DrawRectWithRoundedBorder(animated_position, animated_size,
                                      Color(0, 0, 0, 0), key_border_color_, 1.5f, 6.0f);
        }
    }
}

int PianoKeyboard::GetKeyAtPosition(const Vec2& pos) const {
    // Check black keys first (they're on top)
    for (const auto& key : keys_) {
        if (key.is_black) {
            if (pos.x >= key.position.x && pos.x <= key.position.x + key.size.x &&
                pos.y >= key.position.y && pos.y <= key.position.y + key.size.y) {
                return key.note;
            }
        }
    }

    // Then check white keys
    for (const auto& key : keys_) {
        if (!key.is_black) {
            if (pos.x >= key.position.x && pos.x <= key.position.x + key.size.x &&
                pos.y >= key.position.y && pos.y <= key.position.y + key.size.y) {
                return key.note;
            }
        }
    }

    return -1; // No key found
}

void PianoKeyboard::CalculateAutoLayout(int window_width, int window_height) {
    // Calculate total number of white keys in our 128-key range
    int total_white_keys = GetTotalWhiteKeys();

    // Calculate available width for keyboard (minus margins)
    float available_width = window_width - (keyboard_margin_ * 2.0f);

    // Calculate white key width based on available space
    float white_key_width = available_width / total_white_keys;

    // Limit minimum and maximum key sizes for usability
    white_key_width = std::max(10.0f, std::min(white_key_width, 50.0f));

    // Use fixed keyboard height (don't change with window height)
    float white_key_height = 260.0f;
    float black_key_height = 140.0f;

    // Update key sizes
    white_key_size_ = Vec2(white_key_width, white_key_height);
    black_key_size_ = Vec2(white_key_width * 0.75f, black_key_height);

    // Calculate total keyboard width
    float total_keyboard_width = total_white_keys * white_key_width;

    // Center the keyboard horizontally
    float keyboard_x = (window_width - total_keyboard_width) * 0.5f;

    // Position keyboard in the center of the window
    float keyboard_y = (window_height - white_key_height) * 0.5f;

    keyboard_position_ = Vec2(keyboard_x, keyboard_y);
    keyboard_size_ = Vec2(total_keyboard_width, white_key_height);

    // Debug output
    std::cout << "PianoKeyboard Layout Debug:" << std::endl;
    std::cout << "  Window: " << window_width << "x" << window_height << std::endl;
    std::cout << "  Total white keys: " << total_white_keys << std::endl;
    std::cout << "  Available width: " << available_width << std::endl;
    std::cout << "  White key size: " << white_key_width << "x" << white_key_height << std::endl;
    std::cout << "  Keyboard position: (" << keyboard_x << ", " << keyboard_y << ")" << std::endl;
    std::cout << "  Keyboard size: " << total_keyboard_width << "x" << white_key_height << std::endl;

    // Recalculate key positions
    CalculateKeyPositions();
}

int PianoKeyboard::GetTotalWhiteKeys() const {
    int count = 0;
    for (int note = PIANO_START_NOTE; note <= PIANO_END_NOTE; ++note) {
        if (!IsBlackKey(note)) {
            count++;
        }
    }
    return count;
}

void PianoKeyboard::AddKeyBlip(int note, const Color& color) {
    // Find the key in our 128-key range
    if (note >= PIANO_START_NOTE && note <= PIANO_END_NOTE) {
        int index = note - PIANO_START_NOTE;
        if (index >= 0 && index < static_cast<int>(keys_.size())) {
            // Calculate maximum blips based on keyboard height and blip spacing
            float key_height = keys_[index].is_black ? black_key_size_.y : white_key_size_.y;
            float blip_height = keys_[index].is_black ? black_blip_height_ : white_blip_height_;
            float spacing = blip_height * blip_spacing_factor_;

            // Calculate how many blips can fit in the key height
            size_t max_blips_for_key = static_cast<size_t>(std::max(1.0f, key_height / spacing));

            // Ensure we don't exceed a reasonable maximum to prevent memory issues
            const size_t ABSOLUTE_MAX_BLIPS = 50;
            max_blips_for_key = std::min(max_blips_for_key, ABSOLUTE_MAX_BLIPS);

            if (keys_[index].blips.size() >= max_blips_for_key) {
                // Remove oldest blips if we exceed the limit
                keys_[index].blips.erase(keys_[index].blips.begin(),
                                       keys_[index].blips.begin() + (keys_[index].blips.size() - max_blips_for_key + 1));
            }

            KeyBlip blip;
            blip.time = std::chrono::steady_clock::now();
            blip.color = color;
            blip.y_offset = 0.0f; // Not used in the new implementation, but kept for compatibility

            keys_[index].blips.push_back(blip);
            keys_[index].time_played = blip.time;
        }
    }
}

void PianoKeyboard::UpdateBlips() {
    auto now = std::chrono::steady_clock::now();

    for (auto& key : keys_) {
        if (key.blips.empty()) continue;

        // Use remove_if for more efficient removal of expired blips
        key.blips.erase(
            std::remove_if(key.blips.begin(), key.blips.end(),
                [now, this](const KeyBlip& blip) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - blip.time);
                    return elapsed.count() > blip_fade_duration_ms_;
                }),
            key.blips.end()
        );

        // Additional safety: limit total blips per key based on key height
        float key_height = key.is_black ? black_key_size_.y : white_key_size_.y;
        float blip_height = key.is_black ? black_blip_height_ : white_blip_height_;
        float spacing = blip_height * blip_spacing_factor_;

        size_t max_blips_for_key = static_cast<size_t>(std::max(1.0f, key_height / spacing));
        const size_t ABSOLUTE_MAX_BLIPS = 50;
        max_blips_for_key = std::min(max_blips_for_key, ABSOLUTE_MAX_BLIPS);

        if (key.blips.size() > max_blips_for_key) {
            key.blips.erase(key.blips.begin(),
                           key.blips.begin() + (key.blips.size() - max_blips_for_key));
        }
    }
}

void PianoKeyboard::RenderWhiteKeyBlips(OpenGLRenderer& renderer) {
    auto now = std::chrono::steady_clock::now();

    for (const auto& key : keys_) {
        // Only render blips for white keys
        if (key.is_black || key.blips.empty()) continue;

        // Add 4px margin around blips
        const float margin = 4.0f;

        // Use the key's actual position and size for blips with margin
        float blip_x = key.position.x + margin;
        float blip_width = key.size.x - (margin * 2.0f);  // Reduce width by margin on both sides
        float blip_height = white_blip_height_;

        // Start blips from the bottom of the key (on top of the key surface) with margin
        float blip_y = key.position.y + key.size.y - blip_height - margin;

        // Calculate the top boundary of the piano keyboard (top of the keys)
        float piano_top = keyboard_position_.y;

        // Render each blip for this key
        float current_y = blip_y;
        for (size_t i = 0; i < key.blips.size(); ++i) {
            const auto& blip = key.blips[i];
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - blip.time);
            float time_ratio = std::min(1.0f, elapsed.count() / blip_fade_duration_ms_);

            // Calculate alpha based on time - simple linear fade
            float alpha = 1.0f - time_ratio;

            // Skip if completely faded
            if (alpha <= 0.0f) continue;

            // Clamp blip position to not go above the top of the piano
            if (current_y < piano_top) {
                current_y -= blip_height * blip_spacing_factor_;
                continue;
            }

            // Create color with alpha
            Color blip_color = blip.color;
            blip_color.a = std::max(0.0f, std::min(1.0f, alpha));

            // Calculate position - newer blips appear higher up
            Vec2 blip_pos(blip_x, current_y);
            Vec2 blip_size(blip_width, blip_height);

            // If the blip would extend above the piano top, clip its height
            if (current_y < piano_top) {
                float visible_height = current_y + blip_height - piano_top;
                if (visible_height > 0) {
                    blip_pos.y = piano_top;
                    blip_size.y = visible_height;
                } else {
                    // Blip is completely above the piano, skip it
                    current_y -= blip_height * blip_spacing_factor_;
                    continue;
                }
            }

            // Render the blip
            renderer.DrawRect(blip_pos, blip_size, blip_color);

            // Move to next blip position (stack them vertically upward)
            current_y -= blip_height * blip_spacing_factor_;
        }
    }
}

void PianoKeyboard::RenderBlackKeyBlips(OpenGLRenderer& renderer) {
    auto now = std::chrono::steady_clock::now();

    for (const auto& key : keys_) {
        // Only render blips for black keys
        if (!key.is_black || key.blips.empty()) continue;

        // Add 3px margin around blips
        const float margin = 3.0f;

        // Use the key's actual position and size for blips with margin
        float blip_x = key.position.x + margin;
        float blip_width = key.size.x - (margin * 2.0f);  // Reduce width by margin on both sides
        float blip_height = black_blip_height_;

        // Start blips from the bottom of the key (on top of the key surface) with margin
        float blip_y = key.position.y + key.size.y - blip_height - margin;

        // Calculate the top boundary of the piano keyboard (top of the keys)
        float piano_top = keyboard_position_.y;

        // Render each blip for this key
        float current_y = blip_y;
        for (size_t i = 0; i < key.blips.size(); ++i) {
            const auto& blip = key.blips[i];
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - blip.time);
            float time_ratio = std::min(1.0f, elapsed.count() / blip_fade_duration_ms_);

            // Calculate alpha based on time - simple linear fade
            float alpha = 1.0f - time_ratio;

            // Skip if completely faded
            if (alpha <= 0.0f) continue;

            // Clamp blip position to not go above the top of the piano
            if (current_y < piano_top) {
                current_y -= blip_height * blip_spacing_factor_;
                continue;
            }

            // Create color with alpha
            Color blip_color = blip.color;
            blip_color.a = std::max(0.0f, std::min(1.0f, alpha));

            // Calculate position - newer blips appear higher up
            Vec2 blip_pos(blip_x, current_y);
            Vec2 blip_size(blip_width, blip_height);

            // If the blip would extend above the piano top, clip its height
            if (current_y < piano_top) {
                float visible_height = current_y + blip_height - piano_top;
                if (visible_height > 0) {
                    blip_pos.y = piano_top;
                    blip_size.y = visible_height;
                } else {
                    // Blip is completely above the piano, skip it
                    current_y -= blip_height * blip_spacing_factor_;
                    continue;
                }
            }

            // Render the blip
            renderer.DrawRect(blip_pos, blip_size, blip_color);

            // Move to next blip position (stack them vertically upward)
            current_y -= blip_height * blip_spacing_factor_;
        }
    }
}

void PianoKeyboard::UpdateKeyAnimations() {
    auto now = std::chrono::steady_clock::now();

    for (auto& key : keys_) {
        bool currently_pressed = key.is_pressed;
        bool was_pressed_before = key.was_pressed;

        // Detect press events - only trigger animation on press
        if (currently_pressed && !was_pressed_before) {
            // Key was just pressed - start press animation
            key.press_time = now;
            key.is_animating = true;
            key.animation_progress = 0.0f;
        }

        // Update animation progress (only during the brief press animation)
        if (key.is_animating) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - key.press_time);
            float progress = elapsed.count() / key_press_animation_duration_ms_;

            if (progress >= 1.0f) {
                // Animation complete - stop animating and return to normal
                key.is_animating = false;
                key.animation_progress = 0.0f;
            } else {
                // Animation in progress
                key.animation_progress = progress;
            }
        }

        // Update previous state for next frame
        key.was_pressed = key.is_pressed;
    }
}

