#include "vulkan_renderer.h"
#include "simple_bitmap_font.h"

#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

inline float Clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

inline std::array<float, 4> ColorToVec4(const Color& color) {
    return {color.r, color.g, color.b, color.a};
}

inline float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

const char* kShapeVertexShaderGLSL = R"(#version 450
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inLocal;

layout(location = 0) out vec2 vLocal;

void main() {
    vLocal = inLocal;
    gl_Position = vec4(inPosition, 0.0, 1.0);
}
)";

const char* kShapeFragmentShaderGLSL = R"(#version 450
layout(location = 0) in vec2 vLocal;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    vec4 color0;
    vec4 color1;
    vec4 color2;
    vec4 params0; // width, height, radius, borderWidth
    vec4 params1; // type, extra0, extra1, extra2
} pc;

float roundedDistance(vec2 pixelSize, float radius, vec2 pixelCoord) {
    if (radius <= 0.0) {
        return -1.0;
    }
    vec2 halfSize = pixelSize * 0.5;
    vec2 pos = pixelCoord - halfSize;
    vec2 q = abs(pos) - (halfSize - vec2(radius));
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

void main() {
    float type = pc.params1.x;
    vec2 pixelSize = vec2(pc.params0.x, pc.params0.y);
    vec2 pixelCoord = vec2(vLocal.x * pixelSize.x, vLocal.y * pixelSize.y);
    vec4 color = vec4(0.0);
    float radius = pc.params0.z;
    float borderWidth = pc.params0.w;

    if (type == 0.0) {
        color = pc.color0;
    } else if (type == 1.0) {
        float t = clamp(vLocal.y, 0.0, 1.0);
        color = mix(pc.color0, pc.color1, t);
    } else if (type == 2.0) {
        float dist = roundedDistance(pixelSize, radius, pixelCoord);
        if (dist > 0.0) {
            discard;
        }
        float t = clamp(vLocal.y, 0.0, 1.0);
        color = mix(pc.color0, pc.color1, t);
    } else if (type == 3.0) {
        float dx = min(pixelCoord.x, pixelSize.x - pixelCoord.x);
        float dy = min(pixelCoord.y, pixelSize.y - pixelCoord.y);
        float dist = min(dx, dy);
        if (dist > borderWidth) {
            discard;
        }
        color = pc.color0;
    } else if (type == 4.0) {
        float distOuter = roundedDistance(pixelSize, radius, pixelCoord);
        if (distOuter > 0.0) {
            discard;
        }
        float innerRadius = max(radius - borderWidth, 0.0);
        float distInner = roundedDistance(pixelSize - vec2(borderWidth * 2.0), innerRadius, pixelCoord - vec2(borderWidth));
        if (distInner <= 0.0) {
            discard;
        }
        color = pc.color0;
    } else if (type == 5.0) {
        vec2 center = vec2(pc.color2.r, pc.color2.g);
        float radiusPixels = pc.color2.b;
        if (radiusPixels <= 0.0) {
            radiusPixels = length(pixelSize);
        }
        float dist = length(pixelCoord - center);
        float t = clamp(dist / radiusPixels, 0.0, 1.0);
        color = mix(pc.color0, pc.color1, t);
    } else {
        discard;
    }

    if (color.a <= 0.0) {
        discard;
    }
    outColor = color;
}
)";

const char* kTextVertexShaderGLSL = R"(#version 450
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    vUV = inUV;
    vColor = inColor;
    gl_Position = vec4(inPosition, 0.0, 1.0);
}
)";

const char* kTextFragmentShaderGLSL = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D fontSampler;

void main() {
    float alpha = texture(fontSampler, vUV).r;
    if (alpha <= 0.0) {
        discard;
    }
    outColor = vec4(vColor.rgb, vColor.a * alpha);
}
)";

struct ShapePushConstants {
    std::array<float, 4> color0;
    std::array<float, 4> color1;
    std::array<float, 4> color2;
    std::array<float, 4> params0;
    std::array<float, 4> params1;
};
static_assert(sizeof(ShapePushConstants) <= 128, "Shape push constants exceed Vulkan limit");

std::vector<uint32_t> CompileShader(const std::string& source, shaderc_shader_kind kind, const char* name) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    shaderc::SpvCompilationResult module = compiler.CompileGlslToSpv(source, kind, name, options);
    if (module.GetCompilationStatus() != shaderc_compilation_status_success) {
        throw std::runtime_error(std::string("Shader compilation failed for ") + name + ": " + module.GetErrorMessage());
    }

    return {module.cbegin(), module.cend()};
}

const char* VkResultToString(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        default: return "VK_UNKNOWN_ERROR";
    }
}

#define VK_CHECK(call)                                                                                                      \
    do {                                                                                                                   \
        VkResult _vk_result = (call);                                                                                      \
        if (_vk_result != VK_SUCCESS) {                                                                                    \
            throw std::runtime_error(std::string("Vulkan error: ") + VkResultToString(_vk_result));                       \
        }                                                                                                                  \
    } while (false)

} // namespace

VulkanRenderer::VulkanRenderer() = default;

VulkanRenderer::~VulkanRenderer() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    CleanupVulkan();
}

void VulkanRenderer::Initialize(int window_width, int window_height) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    window_width_ = window_width;
    window_height_ = window_height;

    InitializeVulkan();
    EnsureFramebufferResources(window_width, window_height);
    ResetDrawCallCount();
    ResetBatches();
}

void VulkanRenderer::SetViewport(int width, int height) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    window_width_ = width;
    window_height_ = height;
    EnsureFramebufferResources(width, height);
}

void VulkanRenderer::InitializeVulkan() {
    if (instance_ != VK_NULL_HANDLE) {
        return;
    }

    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "SPP Video Renderer";
    app_info.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    app_info.pEngineName = "spp-video";
    app_info.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app_info;

    VK_CHECK(vkCreateInstance(&instance_info, nullptr, &instance_));

    uint32_t device_count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &device_count, nullptr));
    if (device_count == 0) {
        throw std::runtime_error("No Vulkan-capable GPU found");
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance_, &device_count, devices.data()));

    for (VkPhysicalDevice device : devices) {
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> properties(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, properties.data());

        for (uint32_t i = 0; i < queue_family_count; ++i) {
            if ((properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                physical_device_ = device;
                graphics_queue_family_index_ = i;
                break;
            }
        }
        if (physical_device_ != VK_NULL_HANDLE) {
            break;
        }
    }

    if (physical_device_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Failed to find graphics queue family for Vulkan device");
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = graphics_queue_family_index_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    VK_CHECK(vkCreateDevice(physical_device_, &device_info, nullptr, &device_));
    vkGetDeviceQueue(device_, graphics_queue_family_index_, 0, &graphics_queue_);

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = graphics_queue_family_index_;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_));

    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = command_pool_;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device_, &command_info, &command_buffer_));

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(device_, &fence_info, nullptr, &render_fence_));

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 8;

    VkDescriptorPoolCreateInfo descriptor_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_pool_info.maxSets = 8;
    descriptor_pool_info.poolSizeCount = 1;
    descriptor_pool_info.pPoolSizes = &pool_size;
    VK_CHECK(vkCreateDescriptorPool(device_, &descriptor_pool_info, nullptr, &descriptor_pool_));

}

void VulkanRenderer::CleanupVulkan() {
    vkDeviceWaitIdle(device_);

    DestroyFontResources();
    DestroyPipelines();
    ReleaseFramebufferResources();

    if (readback_buffer_.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, readback_buffer_.buffer, nullptr);
        vkFreeMemory(device_, readback_buffer_.memory, nullptr);
        readback_buffer_ = {};
    }
    if (command_pool_ != VK_NULL_HANDLE && command_buffer_ != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer_);
        command_buffer_ = VK_NULL_HANDLE;
    }
    if (render_fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, render_fence_, nullptr);
        render_fence_ = VK_NULL_HANDLE;
    }
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    physical_device_ = VK_NULL_HANDLE;
    graphics_queue_ = VK_NULL_HANDLE;
}

void VulkanRenderer::EnsureFramebufferResources(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }

    if (framebuffer_width_ == width && framebuffer_height_ == height && offscreen_initialized_) {
        return;
    }

    DestroyFontResources();
    DestroyPipelines();
    ReleaseFramebufferResources();

    framebuffer_width_ = width;
    framebuffer_height_ = height;

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = static_cast<uint32_t>(width);
    image_info.extent.height = static_cast<uint32_t>(height);
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = color_format_;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;

    VK_CHECK(vkCreateImage(device_, &image_info, nullptr, &color_image_));

    VkMemoryRequirements mem_requirements;
    vkGetImageMemoryRequirements(device_, color_image_, &mem_requirements);

    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = FindMemoryType(mem_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &alloc_info, nullptr, &color_image_memory_));
    VK_CHECK(vkBindImageMemory(device_, color_image_, color_image_memory_, 0));

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = color_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = color_format_;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device_, &view_info, nullptr, &color_image_view_));

    VkAttachmentDescription color_attachment{};
    color_attachment.format = color_format_;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo render_pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &color_attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    VK_CHECK(vkCreateRenderPass(device_, &render_pass_info, nullptr, &render_pass_));

    VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = render_pass_;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &color_image_view_;
    framebuffer_info.width = static_cast<uint32_t>(width);
    framebuffer_info.height = static_cast<uint32_t>(height);
    framebuffer_info.layers = 1;
    VK_CHECK(vkCreateFramebuffer(device_, &framebuffer_info, nullptr, &framebuffer_));

    color_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    offscreen_initialized_ = true;
    framebuffer_bound_ = true;
    readback_cache_.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);

    CreatePipelines();
}

void VulkanRenderer::ReleaseFramebufferResources() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    if (framebuffer_ != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }
    if (color_image_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, color_image_view_, nullptr);
        color_image_view_ = VK_NULL_HANDLE;
    }
    if (color_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, color_image_, nullptr);
        color_image_ = VK_NULL_HANDLE;
    }
    if (color_image_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, color_image_memory_, nullptr);
        color_image_memory_ = VK_NULL_HANDLE;
    }

    offscreen_initialized_ = false;
    framebuffer_bound_ = false;
}

void VulkanRenderer::CreatePipelines() {
    std::vector<uint32_t> shape_vert_spv = CompileShader(kShapeVertexShaderGLSL, shaderc_glsl_vertex_shader, "shape.vert");
    std::vector<uint32_t> shape_frag_spv = CompileShader(kShapeFragmentShaderGLSL, shaderc_glsl_fragment_shader, "shape.frag");
    std::vector<uint32_t> text_vert_spv = CompileShader(kTextVertexShaderGLSL, shaderc_glsl_vertex_shader, "text.vert");
    std::vector<uint32_t> text_frag_spv = CompileShader(kTextFragmentShaderGLSL, shaderc_glsl_fragment_shader, "text.frag");

    VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_info.codeSize = shape_vert_spv.size() * sizeof(uint32_t);
    shader_info.pCode = shape_vert_spv.data();
    VkShaderModule shape_vert_module;
    VK_CHECK(vkCreateShaderModule(device_, &shader_info, nullptr, &shape_vert_module));

    shader_info.codeSize = shape_frag_spv.size() * sizeof(uint32_t);
    shader_info.pCode = shape_frag_spv.data();
    VkShaderModule shape_frag_module;
    VK_CHECK(vkCreateShaderModule(device_, &shader_info, nullptr, &shape_frag_module));

    shader_info.codeSize = text_vert_spv.size() * sizeof(uint32_t);
    shader_info.pCode = text_vert_spv.data();
    VkShaderModule text_vert_module;
    VK_CHECK(vkCreateShaderModule(device_, &shader_info, nullptr, &text_vert_module));

    shader_info.codeSize = text_frag_spv.size() * sizeof(uint32_t);
    shader_info.pCode = text_frag_spv.data();
    VkShaderModule text_frag_module;
    VK_CHECK(vkCreateShaderModule(device_, &shader_info, nullptr, &text_frag_module));

    VkPushConstantRange shape_push{};
    shape_push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    shape_push.offset = 0;
    shape_push.size = sizeof(ShapePushConstants);

    VkPipelineLayoutCreateInfo shape_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    shape_layout_info.pushConstantRangeCount = 1;
    shape_layout_info.pPushConstantRanges = &shape_push;
    VK_CHECK(vkCreatePipelineLayout(device_, &shape_layout_info, nullptr, &shape_pipeline_layout_));

    VkDescriptorSetLayoutBinding font_binding{};
    font_binding.binding = 0;
    font_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    font_binding.descriptorCount = 1;
    font_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo text_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    text_layout_info.bindingCount = 1;
    text_layout_info.pBindings = &font_binding;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &text_layout_info, nullptr, &text_descriptor_set_layout_));

    VkPipelineLayoutCreateInfo text_pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    text_pipeline_layout_info.setLayoutCount = 1;
    text_pipeline_layout_info.pSetLayouts = &text_descriptor_set_layout_;
    VK_CHECK(vkCreatePipelineLayout(device_, &text_pipeline_layout_info, nullptr, &text_pipeline_layout_));

    auto create_pipeline = [&](VkShaderModule vert_module, VkShaderModule frag_module, VkPipelineLayout layout,
                               const std::vector<VkVertexInputBindingDescription>& bindings,
                               const std::vector<VkVertexInputAttributeDescription>& attributes,
                               VkPipeline& pipeline_out) {
        VkPipelineShaderStageCreateInfo stages[2];
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert_module;
        stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag_module;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertex_input.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
        vertex_input.pVertexBindingDescriptions = bindings.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo input_assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        std::array<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic_state{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
        dynamic_state.pDynamicStates = dynamic_states.data();

        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState color_blend_attachment{};
        color_blend_attachment.blendEnable = VK_TRUE;
        color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color_blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color_blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo color_blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        color_blend.attachmentCount = 1;
        color_blend.pAttachments = &color_blend_attachment;

        VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &color_blend;
        pipeline_info.pDynamicState = &dynamic_state;
        pipeline_info.layout = layout;
        pipeline_info.renderPass = render_pass_;
        pipeline_info.subpass = 0;

        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_out));
    };

    VkVertexInputBindingDescription shape_binding{};
    shape_binding.binding = 0;
    shape_binding.stride = sizeof(ShapeVertex);
    shape_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription shape_attributes[2];
    shape_attributes[0].binding = 0;
    shape_attributes[0].location = 0;
    shape_attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    shape_attributes[0].offset = 0;
    shape_attributes[1].binding = 0;
    shape_attributes[1].location = 1;
    shape_attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    shape_attributes[1].offset = sizeof(float) * 2;

    create_pipeline(shape_vert_module, shape_frag_module, shape_pipeline_layout_, {shape_binding},
                    {shape_attributes[0], shape_attributes[1]}, shape_pipeline_);

    VkVertexInputBindingDescription text_binding{};
    text_binding.binding = 0;
    text_binding.stride = sizeof(TextVertex);
    text_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription text_attributes[3];
    text_attributes[0].binding = 0;
    text_attributes[0].location = 0;
    text_attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    text_attributes[0].offset = 0;
    text_attributes[1].binding = 0;
    text_attributes[1].location = 1;
    text_attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    text_attributes[1].offset = sizeof(float) * 2;
    text_attributes[2].binding = 0;
    text_attributes[2].location = 2;
    text_attributes[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    text_attributes[2].offset = sizeof(float) * 4;

    create_pipeline(text_vert_module, text_frag_module, text_pipeline_layout_, {text_binding},
                    {text_attributes[0], text_attributes[1], text_attributes[2]}, text_pipeline_);

    vkDestroyShaderModule(device_, shape_vert_module, nullptr);
    vkDestroyShaderModule(device_, shape_frag_module, nullptr);
    vkDestroyShaderModule(device_, text_vert_module, nullptr);
    vkDestroyShaderModule(device_, text_frag_module, nullptr);
}

void VulkanRenderer::DestroyPipelines() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    if (shape_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, shape_pipeline_, nullptr);
        shape_pipeline_ = VK_NULL_HANDLE;
    }
    if (text_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, text_pipeline_, nullptr);
        text_pipeline_ = VK_NULL_HANDLE;
    }
    if (shape_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, shape_pipeline_layout_, nullptr);
        shape_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (text_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, text_pipeline_layout_, nullptr);
        text_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (text_descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, text_descriptor_set_layout_, nullptr);
        text_descriptor_set_layout_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::EnsureFontResources(float font_size) {
    if (font_uploaded_ && std::abs(requested_font_size_ - font_size) < 0.5f) {
        return;
    }

    DestroyFontResources();

    const int glyph_count = simple_font::kCharCount;
    const int columns = 16;
    const int rows = (glyph_count + columns - 1) / columns;
    const int glyph_width = simple_font::kGlyphWidth;
    const int glyph_height = simple_font::kGlyphHeight;
    const int padding = 1;

    const int atlas_width = columns * (glyph_width + padding) + padding;
    const int atlas_height = rows * (glyph_height + padding) + padding;
    std::vector<std::uint8_t> atlas(static_cast<std::size_t>(atlas_width) * static_cast<std::size_t>(atlas_height), 0);

    glyph_infos_.assign(glyph_count, {});

    for (int index = 0; index < glyph_count; ++index) {
        int column = index % columns;
        int row = index / columns;
        int x_offset = padding + column * (glyph_width + padding);
        int y_offset = padding + row * (glyph_height + padding);

        for (int y = 0; y < glyph_height; ++y) {
            unsigned char bits = simple_font::kFont5x8[index][y];
            for (int x = 0; x < glyph_width; ++x) {
                bool set = (bits & (1u << (glyph_width - 1 - x))) != 0;
                atlas[(y_offset + y) * atlas_width + (x_offset + x)] = set ? 255 : 0;
            }
        }

        float u0 = static_cast<float>(x_offset) / static_cast<float>(atlas_width);
        float v0 = static_cast<float>(y_offset) / static_cast<float>(atlas_height);
        float u1 = static_cast<float>(x_offset + glyph_width) / static_cast<float>(atlas_width);
        float v1 = static_cast<float>(y_offset + glyph_height) / static_cast<float>(atlas_height);

        glyph_infos_[index] = {u0, v0, u1, v1, static_cast<float>(glyph_width + 1)};
    }

    VulkanBuffer staging;
    VkDeviceSize image_size = static_cast<VkDeviceSize>(atlas.size());
    EnsureBufferCapacity(staging, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device_, staging.memory, 0, image_size, 0, &mapped));
    std::memcpy(mapped, atlas.data(), static_cast<std::size_t>(image_size));
    vkUnmapMemory(device_, staging.memory);

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = static_cast<uint32_t>(atlas_width);
    image_info.extent.height = static_cast<uint32_t>(atlas_height);
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = VK_FORMAT_R8_UNORM;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;

    VK_CHECK(vkCreateImage(device_, &image_info, nullptr, &font_image_));

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device_, font_image_, &mem_req);

    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = FindMemoryType(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device_, &alloc_info, nullptr, &font_image_memory_));
    VK_CHECK(vkBindImageMemory(device_, font_image_, font_image_memory_, 0));

    TransitionImageLayout(font_image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(command_buffer_, 0);
    VK_CHECK(vkBeginCommandBuffer(command_buffer_, &begin_info));

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {static_cast<uint32_t>(atlas_width), static_cast<uint32_t>(atlas_height), 1};

    vkCmdCopyBufferToImage(command_buffer_, staging.buffer, font_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VK_CHECK(vkEndCommandBuffer(command_buffer_));

    SubmitAndWait();

    TransitionImageLayout(font_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = font_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device_, &view_info, nullptr, &font_image_view_));

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    VK_CHECK(vkCreateSampler(device_, &sampler_info, nullptr, &font_sampler_));

    VkDescriptorSetAllocateInfo alloc_desc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc_desc.descriptorPool = descriptor_pool_;
    alloc_desc.descriptorSetCount = 1;
    alloc_desc.pSetLayouts = &text_descriptor_set_layout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &alloc_desc, &text_descriptor_set_));

    VkDescriptorImageInfo image_desc{};
    image_desc.sampler = font_sampler_;
    image_desc.imageView = font_image_view_;
    image_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = text_descriptor_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_desc;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    requested_font_size_ = font_size;
    font_pixel_scale_ = font_size / static_cast<float>(simple_font::kGlyphHeight);
    font_uploaded_ = true;

    vkDestroyBuffer(device_, staging.buffer, nullptr);
    vkFreeMemory(device_, staging.memory, nullptr);
    staging.buffer = VK_NULL_HANDLE;
    staging.memory = VK_NULL_HANDLE;
    staging.size = 0;
}

void VulkanRenderer::DestroyFontResources() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    if (text_descriptor_set_ != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device_, descriptor_pool_, 1, &text_descriptor_set_);
        text_descriptor_set_ = VK_NULL_HANDLE;
    }
    if (font_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, font_sampler_, nullptr);
        font_sampler_ = VK_NULL_HANDLE;
    }
    if (font_image_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, font_image_view_, nullptr);
        font_image_view_ = VK_NULL_HANDLE;
    }
    if (font_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, font_image_, nullptr);
        font_image_ = VK_NULL_HANDLE;
    }
    if (font_image_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, font_image_memory_, nullptr);
        font_image_memory_ = VK_NULL_HANDLE;
    }
    font_uploaded_ = false;
}

void VulkanRenderer::ResetBatches() {
    shape_commands_.clear();
    text_commands_.clear();
    shape_vertices_.clear();
    text_vertices_.clear();
    frame_dirty_ = false;
}

void VulkanRenderer::Clear(const Color& clear_color) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    ResetBatches();
    clear_color_ = clear_color;
    has_pending_clear_ = true;
    frame_dirty_ = true;
}

void VulkanRenderer::ClearWithRadialGradient(const Color& center_color, const Color& edge_color) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    ResetBatches();
    clear_color_ = center_color;
    has_pending_clear_ = true;

    ShapeCommand command{};
    command.position = Vec2(0.0f, 0.0f);
    command.size = Vec2(static_cast<float>(framebuffer_width_), static_cast<float>(framebuffer_height_));
    command.color0 = center_color;
    command.color1 = edge_color;
    float width_f = static_cast<float>(framebuffer_width_);
    float height_f = static_cast<float>(framebuffer_height_);
    command.color2 = Color(width_f * 0.5f,
                           height_f * 0.5f,
                           std::sqrt(width_f * width_f + height_f * height_f) * 0.5f,
                           1.0f);
    command.type = ShapeType::RadialGradient;
    PushShapeCommand(command);
    frame_dirty_ = true;
}

void VulkanRenderer::ClearWithImage(const std::string& image_path, float opacity, int scale_mode) {
    (void)image_path;
    (void)opacity;
    (void)scale_mode;
    Clear(Color(0.0f, 0.0f, 0.0f, 1.0f));
}

void VulkanRenderer::PushShapeCommand(const ShapeCommand& command) {
    shape_commands_.push_back(command);
    frame_dirty_ = true;
}

void VulkanRenderer::DrawRect(const Vec2& position, const Vec2& size, const Color& color) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    ShapeCommand command{};
    command.position = position;
    command.size = size;
    command.color0 = color;
    command.type = ShapeType::Solid;
    PushShapeCommand(command);
    ++draw_call_count_;
}

void VulkanRenderer::DrawRectGradient(const Vec2& position, const Vec2& size,
                                      const Color& top_color, const Color& bottom_color) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    ShapeCommand command{};
    command.position = position;
    command.size = size;
    command.color0 = top_color;
    command.color1 = bottom_color;
    command.type = ShapeType::VerticalGradient;
    PushShapeCommand(command);
    ++draw_call_count_;
}

void VulkanRenderer::DrawRectGradientRounded(const Vec2& position, const Vec2& size,
                                             const Color& top_color, const Color& bottom_color,
                                             float corner_radius) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    ShapeCommand command{};
    command.position = position;
    command.size = size;
    command.color0 = top_color;
    command.color1 = bottom_color;
    command.radius = corner_radius;
    command.type = ShapeType::RoundedGradient;
    PushShapeCommand(command);
    ++draw_call_count_;
}

void VulkanRenderer::DrawRectWithBorder(const Vec2& position, const Vec2& size,
                                        const Color& fill_color, const Color& border_color,
                                        float border_width) {
    if (fill_color.a > 0.0f) {
        DrawRect(position, size, fill_color);
    }
    if (border_width <= 0.0f || border_color.a <= 0.0f) {
        return;
    }
    std::lock_guard<std::mutex> lock(render_mutex_);
    ShapeCommand border{};
    border.position = position;
    border.size = size;
    border.color0 = border_color;
    border.border_width = border_width;
    border.type = ShapeType::Border;
    PushShapeCommand(border);
    ++draw_call_count_;
}

void VulkanRenderer::DrawRectWithRoundedBorder(const Vec2& position, const Vec2& size,
                                               const Color& fill_color, const Color& border_color,
                                               float border_width, float corner_radius) {
    if (fill_color.a > 0.0f) {
        DrawRectGradientRounded(position, size, fill_color, fill_color, corner_radius);
    }
    if (border_width <= 0.0f || border_color.a <= 0.0f) {
        return;
    }
    std::lock_guard<std::mutex> lock(render_mutex_);
    ShapeCommand border{};
    border.position = position;
    border.size = size;
    border.color0 = border_color;
    border.border_width = border_width;
    border.radius = corner_radius;
    border.type = ShapeType::RoundedBorder;
    PushShapeCommand(border);
    ++draw_call_count_;
}

bool VulkanRenderer::LoadFont(float font_size) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    EnsureFontResources(font_size);
    font_loaded_ = true;
    return true;
}

void VulkanRenderer::DrawText(const std::string& text, const Vec2& position, const Color& color, float scale) {
    if (text.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(render_mutex_);
    if (!font_loaded_) {
        LoadFont(requested_font_size_);
    }

    float effective_scale = font_pixel_scale_ * scale;
    float cursor_x = position.x;
    float cursor_y = position.y;

    for (char c : text) {
        if (c == '\n') {
            cursor_x = position.x;
            cursor_y += static_cast<float>(simple_font::kGlyphHeight + 1) * effective_scale;
            continue;
        }

        if (c < simple_font::kFirstChar || c > simple_font::kLastChar) {
            cursor_x += static_cast<float>(simple_font::kGlyphWidth + 1) * effective_scale;
            continue;
        }

        int index = c - simple_font::kFirstChar;
        const GlyphInfo& glyph = glyph_infos_[index];

        TextCommand command{};
        command.position = Vec2(cursor_x, cursor_y);
        command.size = Vec2(static_cast<float>(simple_font::kGlyphWidth) * effective_scale,
                            static_cast<float>(simple_font::kGlyphHeight) * effective_scale);
        command.color = color;
        command.uv0 = Vec2(glyph.u0, glyph.v0);
        command.uv1 = Vec2(glyph.u1, glyph.v1);
        text_commands_.push_back(command);

        cursor_x += glyph.advance * effective_scale;
    }

    frame_dirty_ = true;
    ++draw_call_count_;
}

Vec2 VulkanRenderer::GetTextSize(const std::string& text, float scale) {
    if (text.empty()) {
        return Vec2(0.0f, 0.0f);
    }

    float effective_scale = font_pixel_scale_ * scale;
    float max_width = 0.0f;
    float line_width = 0.0f;
    float total_height = static_cast<float>(simple_font::kGlyphHeight) * effective_scale;

    for (char c : text) {
        if (c == '\n') {
            max_width = std::max(max_width, line_width);
            line_width = 0.0f;
            total_height += static_cast<float>(simple_font::kGlyphHeight) * effective_scale;
            continue;
        }

        line_width += static_cast<float>(simple_font::kGlyphWidth + 1) * effective_scale;
    }

    max_width = std::max(max_width, line_width);
    return Vec2(max_width, total_height);
}

void VulkanRenderer::BeginBatch() {}

void VulkanRenderer::EndBatch() {}

void VulkanRenderer::BeginFrame() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    ResetDrawCallCount();
    ResetBatches();
}

void VulkanRenderer::EndFrame() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    FlushIfNeeded();
}

bool VulkanRenderer::CreateOffscreenFramebuffer(int width, int height) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    EnsureFramebufferResources(width, height);
    return offscreen_initialized_;
}

void VulkanRenderer::BindOffscreenFramebuffer() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    framebuffer_bound_ = true;
}

void VulkanRenderer::UnbindOffscreenFramebuffer() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    framebuffer_bound_ = false;
}

bool VulkanRenderer::InitializePBO(int width, int height) {
    (void)width;
    (void)height;
    return true;
}

void VulkanRenderer::CleanupPBO() {}

void VulkanRenderer::FlushIfNeeded() {
    if (!frame_dirty_) {
        return;
    }
    Flush();
}

void VulkanRenderer::Flush() {
    if (!HasRenderableContent()) {
        frame_dirty_ = false;
        return;
    }

    shape_vertices_.clear();
    text_vertices_.clear();

    auto append_shape_quad = [&](const ShapeCommand& command) {
        float x0 = command.position.x;
        float y0 = command.position.y;
        float x1 = command.position.x + command.size.x;
        float y1 = command.position.y + command.size.y;

        auto to_clip = [&](float x, float y) {
            ShapeVertex vertex{};
            vertex.position[0] = (x / static_cast<float>(framebuffer_width_)) * 2.0f - 1.0f;
            vertex.position[1] = 1.0f - (y / static_cast<float>(framebuffer_height_)) * 2.0f;
            return vertex;
        };

        ShapeVertex v0 = to_clip(x0, y0);
        v0.local[0] = 0.0f;
        v0.local[1] = 0.0f;
        ShapeVertex v1 = to_clip(x1, y0);
        v1.local[0] = 1.0f;
        v1.local[1] = 0.0f;
        ShapeVertex v2 = to_clip(x1, y1);
        v2.local[0] = 1.0f;
        v2.local[1] = 1.0f;
        ShapeVertex v3 = to_clip(x0, y1);
        v3.local[0] = 0.0f;
        v3.local[1] = 1.0f;

        shape_vertices_.push_back(v0);
        shape_vertices_.push_back(v1);
        shape_vertices_.push_back(v2);
        shape_vertices_.push_back(v0);
        shape_vertices_.push_back(v2);
        shape_vertices_.push_back(v3);
    };

    for (const auto& command : shape_commands_) {
        append_shape_quad(command);
    }

    auto append_text_quad = [&](const TextCommand& command) {
        float x0 = command.position.x;
        float y0 = command.position.y;
        float x1 = command.position.x + command.size.x;
        float y1 = command.position.y + command.size.y;

        auto to_clip = [&](float x, float y) {
            TextVertex vertex{};
            vertex.position[0] = (x / static_cast<float>(framebuffer_width_)) * 2.0f - 1.0f;
            vertex.position[1] = 1.0f - (y / static_cast<float>(framebuffer_height_)) * 2.0f;
            return vertex;
        };

    auto color_vec = ColorToVec4(command.color);

    TextVertex v0 = to_clip(x0, y0);
    v0.uv[0] = command.uv0.x;
    v0.uv[1] = command.uv0.y;
    std::copy(color_vec.begin(), color_vec.end(), v0.color);

    TextVertex v1 = to_clip(x1, y0);
    v1.uv[0] = command.uv1.x;
    v1.uv[1] = command.uv0.y;
    std::copy(color_vec.begin(), color_vec.end(), v1.color);

    TextVertex v2 = to_clip(x1, y1);
    v2.uv[0] = command.uv1.x;
    v2.uv[1] = command.uv1.y;
    std::copy(color_vec.begin(), color_vec.end(), v2.color);

    TextVertex v3 = to_clip(x0, y1);
    v3.uv[0] = command.uv0.x;
    v3.uv[1] = command.uv1.y;
    std::copy(color_vec.begin(), color_vec.end(), v3.color);

        text_vertices_.push_back(v0);
        text_vertices_.push_back(v1);
        text_vertices_.push_back(v2);
        text_vertices_.push_back(v0);
        text_vertices_.push_back(v2);
        text_vertices_.push_back(v3);
    };

    for (const auto& command : text_commands_) {
        append_text_quad(command);
    }

    UploadShapeVertices(shape_vertices_);
    UploadTextVertices(text_vertices_);

    RecordCommandBuffer();
    SubmitAndWait();
    CopyImageToReadbackBuffer();
    readback_pending_ = true;
    frame_dirty_ = false;
    has_pending_clear_ = false;
}

bool VulkanRenderer::HasRenderableContent() const {
    return has_pending_clear_ || !shape_commands_.empty() || !text_commands_.empty();
}

void VulkanRenderer::RecordCommandBuffer() {
    if (color_image_layout_ != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        TransitionImageLayout(color_image_, color_image_layout_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        color_image_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    vkResetCommandBuffer(command_buffer_, 0);
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(command_buffer_, &begin_info));

    VkClearValue clear_value{};
    clear_value.color.float32[0] = clear_color_.r;
    clear_value.color.float32[1] = clear_color_.g;
    clear_value.color.float32[2] = clear_color_.b;
    clear_value.color.float32[3] = clear_color_.a;

    VkRenderPassBeginInfo render_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render_begin.renderPass = render_pass_;
    render_begin.framebuffer = framebuffer_;
    render_begin.renderArea.extent.width = static_cast<uint32_t>(framebuffer_width_);
    render_begin.renderArea.extent.height = static_cast<uint32_t>(framebuffer_height_);
    render_begin.clearValueCount = 1;
    render_begin.pClearValues = &clear_value;

    vkCmdBeginRenderPass(command_buffer_, &render_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(framebuffer_width_);
    viewport.height = static_cast<float>(framebuffer_height_);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffer_, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent.width = static_cast<uint32_t>(framebuffer_width_);
    scissor.extent.height = static_cast<uint32_t>(framebuffer_height_);
    vkCmdSetScissor(command_buffer_, 0, 1, &scissor);

    if (!shape_vertices_.empty()) {
        VkDeviceSize offset = 0;
        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, shape_pipeline_);
        vkCmdBindVertexBuffers(command_buffer_, 0, 1, &shape_vertex_buffer_.buffer, &offset);

        std::size_t vertex_index = 0;
        for (const auto& command : shape_commands_) {
            ShapePushConstants constants{};
            constants.color0 = ColorToVec4(command.color0);
            constants.color1 = ColorToVec4(command.color1);
            constants.color2 = ColorToVec4(command.color2);
            constants.params0 = {command.size.x, command.size.y, command.radius, command.border_width};
            constants.params1 = {static_cast<float>(static_cast<std::uint32_t>(command.type)), command.extra0, 0.0f, 0.0f};
            vkCmdPushConstants(command_buffer_, shape_pipeline_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(ShapePushConstants), &constants);
            vkCmdDraw(command_buffer_, 6, 1, static_cast<uint32_t>(vertex_index), 0);
            vertex_index += 6;
        }
    }

    if (font_uploaded_ && !text_vertices_.empty()) {
        VkDeviceSize offset = 0;
        vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, text_pipeline_);
        vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, text_pipeline_layout_,
                                0, 1, &text_descriptor_set_, 0, nullptr);
        vkCmdBindVertexBuffers(command_buffer_, 0, 1, &text_vertex_buffer_.buffer, &offset);
        vkCmdDraw(command_buffer_, static_cast<uint32_t>(text_vertices_.size()), 1, 0, 0);
    }

    vkCmdEndRenderPass(command_buffer_);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = color_image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    color_image_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VK_CHECK(vkEndCommandBuffer(command_buffer_));
}

void VulkanRenderer::SubmitAndWait() {
    VK_CHECK(vkResetFences(device_, 1, &render_fence_));

    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer_;
    VK_CHECK(vkQueueSubmit(graphics_queue_, 1, &submit_info, render_fence_));
    VK_CHECK(vkWaitForFences(device_, 1, &render_fence_, VK_TRUE, UINT64_MAX));
}

void VulkanRenderer::CopyImageToReadbackBuffer() {
    VkDeviceSize required_size = static_cast<VkDeviceSize>(framebuffer_width_) * framebuffer_height_ * 4u;
    EnsureBufferCapacity(readback_buffer_, required_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    vkResetCommandBuffer(command_buffer_, 0);
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(command_buffer_, &begin_info));

    VkBufferImageCopy copy_region{};
    copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.imageSubresource.layerCount = 1;
    copy_region.imageExtent = {static_cast<uint32_t>(framebuffer_width_), static_cast<uint32_t>(framebuffer_height_), 1};
    copy_region.bufferRowLength = static_cast<uint32_t>(framebuffer_width_);
    copy_region.bufferImageHeight = static_cast<uint32_t>(framebuffer_height_);

    vkCmdCopyImageToBuffer(command_buffer_, color_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_buffer_.buffer, 1, &copy_region);
    VK_CHECK(vkEndCommandBuffer(command_buffer_));

    SubmitAndWait();

    void* data = nullptr;
    VK_CHECK(vkMapMemory(device_, readback_buffer_.memory, 0, required_size, 0, &data));

    const auto row_size = static_cast<std::size_t>(framebuffer_width_) * 4u;
    const auto height = static_cast<std::size_t>(framebuffer_height_);
    const std::uint8_t* src_bytes = static_cast<std::uint8_t*>(data);
    std::uint8_t* dst_bytes = readback_cache_.data();

    for (std::size_t y = 0; y < height; ++y) {
        const std::size_t src_row = (height - 1 - y) * row_size;
        const std::size_t dst_row = y * row_size;
        std::memcpy(dst_bytes + dst_row, src_bytes + src_row, row_size);
    }

    vkUnmapMemory(device_, readback_buffer_.memory);
    readback_pending_ = false;
}

std::vector<std::uint8_t> VulkanRenderer::ReadFramebuffer(int width, int height) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (width != framebuffer_width_ || height != framebuffer_height_ || !offscreen_initialized_) {
        return {};
    }
    FlushIfNeeded();
    return readback_cache_;
}

std::vector<std::uint8_t> VulkanRenderer::ReadFramebufferPBO(int width, int height) {
    return ReadFramebuffer(width, height);
}

void VulkanRenderer::StartAsyncReadback(int width, int height) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (width != framebuffer_width_ || height != framebuffer_height_) {
        return;
    }
    FlushIfNeeded();
    readback_pending_ = true;
}

std::vector<std::uint8_t> VulkanRenderer::GetAsyncReadbackResult(int width, int height) {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (width != framebuffer_width_ || height != framebuffer_height_) {
        return {};
    }
    if (readback_pending_) {
        CopyImageToReadbackBuffer();
    }
    return readback_cache_;
}

void VulkanRenderer::RenderOffscreenTextureToScreen(int screen_width, int screen_height) {
    (void)screen_width;
    (void)screen_height;
}

void VulkanRenderer::RenderPreviewOverlay(int screen_width, int screen_height,
                                          const std::vector<std::string>& info_lines,
                                          float progress_ratio) {
    (void)screen_width;
    (void)screen_height;
    (void)info_lines;
    (void)progress_ratio;
}

Vec2 VulkanRenderer::ScreenToGL(const Vec2& screen_pos) const {
    return screen_pos;
}

Vec2 VulkanRenderer::GLToScreen(const Vec2& gl_pos) const {
    return gl_pos;
}

void VulkanRenderer::ResetDrawCallCount() {
    draw_call_count_ = 0;
}

unsigned int VulkanRenderer::GetDrawCallCount() const {
    return draw_call_count_;
}

void VulkanRenderer::EnsureBufferCapacity(VulkanBuffer& buffer, VkDeviceSize required_size, VkBufferUsageFlags usage) {
    if (buffer.buffer != VK_NULL_HANDLE && buffer.size >= required_size) {
        return;
    }

    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer.buffer, nullptr);
        vkFreeMemory(device_, buffer.memory, nullptr);
        buffer = {};
    }

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = required_size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device_, &buffer_info, nullptr, &buffer.buffer));

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device_, buffer.buffer, &mem_req);

    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = FindMemoryType(mem_req.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(device_, &alloc_info, nullptr, &buffer.memory));
    VK_CHECK(vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0));

    buffer.size = required_size;
}

void VulkanRenderer::UploadShapeVertices(const std::vector<ShapeVertex>& vertices) {
    if (vertices.empty()) {
        return;
    }
    VkDeviceSize size = static_cast<VkDeviceSize>(vertices.size() * sizeof(ShapeVertex));
    EnsureBufferCapacity(shape_vertex_buffer_, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device_, shape_vertex_buffer_.memory, 0, size, 0, &mapped));
    std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(ShapeVertex));
    vkUnmapMemory(device_, shape_vertex_buffer_.memory);
}

void VulkanRenderer::UploadTextVertices(const std::vector<TextVertex>& vertices) {
    if (vertices.empty()) {
        return;
    }
    VkDeviceSize size = static_cast<VkDeviceSize>(vertices.size() * sizeof(TextVertex));
    EnsureBufferCapacity(text_vertex_buffer_, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(device_, text_vertex_buffer_.memory, 0, size, 0, &mapped));
    std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(TextVertex));
    vkUnmapMemory(device_, text_vertex_buffer_.memory);
}

void VulkanRenderer::TransitionImageLayout(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout) {
    if (old_layout == new_layout) {
        return;
    }

    vkResetCommandBuffer(command_buffer_, 0);
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(command_buffer_, &begin_info));

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = 0;
    }

    vkCmdPipelineBarrier(command_buffer_, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VK_CHECK(vkEndCommandBuffer(command_buffer_));
    SubmitAndWait();
}

uint32_t VulkanRenderer::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable Vulkan memory type");
}
