#pragma once

#include <cstddef>

struct GLFWwindow;

namespace window_icon {

// Attempts to set the GLFW window icon using PNG data in memory. Returns true on success.
bool SetWindowIconFromPng(GLFWwindow* window,
                          const unsigned char* data,
                          std::size_t size,
                          bool generate_additional_sizes = true);

} // namespace window_icon
