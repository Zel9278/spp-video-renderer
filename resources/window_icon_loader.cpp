#include "window_icon_loader.h"

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace window_icon {
namespace {

std::vector<unsigned char> ResizeNearest(const unsigned char* src,
                                         int src_width,
                                         int src_height,
                                         int dst_width,
                                         int dst_height) {
    std::vector<unsigned char> dst(
        static_cast<std::size_t>(dst_width) * static_cast<std::size_t>(dst_height) * 4u);

    for (int y = 0; y < dst_height; ++y) {
        int src_y = static_cast<int>(static_cast<std::int64_t>(y) * src_height / dst_height);
        if (src_y >= src_height) {
            src_y = src_height - 1;
        }
        for (int x = 0; x < dst_width; ++x) {
            int src_x = static_cast<int>(static_cast<std::int64_t>(x) * src_width / dst_width);
            if (src_x >= src_width) {
                src_x = src_width - 1;
            }
            std::size_t dst_index =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_width) + static_cast<std::size_t>(x)) * 4u;
            std::size_t src_index =
                (static_cast<std::size_t>(src_y) * static_cast<std::size_t>(src_width) + static_cast<std::size_t>(src_x)) * 4u;
            std::copy_n(src + src_index, 4, dst.data() + dst_index);
        }
    }

    return dst;
}

} // namespace

bool SetWindowIconFromPng(GLFWwindow* window,
                          const unsigned char* data,
                          std::size_t size,
                          bool generate_additional_sizes) {
    if (!window || !data || size == 0) {
        return false;
    }

    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(
        data,
        static_cast<int>(size),
        &width,
        &height,
        &channels,
        STBI_rgb_alpha);
    if (!decoded) {
        return false;
    }

    if (width <= 0 || height <= 0) {
        stbi_image_free(decoded);
        return false;
    }

    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (pixel_count > static_cast<std::size_t>(std::numeric_limits<std::size_t>::max() / 4)) {
        stbi_image_free(decoded);
        return false;
    }

    std::vector<std::vector<unsigned char>> storage;
    storage.reserve(6);
    storage.emplace_back(decoded, decoded + pixel_count * 4);
    stbi_image_free(decoded);

    std::vector<GLFWimage> images;
    images.reserve(1 + (generate_additional_sizes ? 5 : 0));

    GLFWimage original{};
    original.width = width;
    original.height = height;
    original.pixels = storage.back().data();
    images.push_back(original);

    if (generate_additional_sizes) {
        const int target_sizes[] = {128, 64, 48, 32, 24, 16};
        for (int size_px : target_sizes) {
            if (width < size_px || height < size_px) {
                continue;
            }
            storage.emplace_back(
                ResizeNearest(storage.front().data(), width, height, size_px, size_px));
            GLFWimage scaled{};
            scaled.width = size_px;
            scaled.height = size_px;
            scaled.pixels = storage.back().data();
            images.push_back(scaled);
        }
    }

    glfwSetWindowIcon(window, static_cast<int>(images.size()), images.data());
    return true;
}

} // namespace window_icon
