#pragma once

#include "renderer.h"

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

class VulkanRenderer : public RendererBackend {
public:
    VulkanRenderer();
    ~VulkanRenderer() override;

    const char* GetName() const override { return "Vulkan"; }

    void Initialize(int window_width, int window_height) override;
    void SetViewport(int width, int height) override;

    void Clear(const Color& clear_color) override;
    void ClearWithRadialGradient(const Color& center_color, const Color& edge_color) override;
    void ClearWithImage(const std::string& image_path, float opacity, int scale_mode) override;

    bool LoadFont(float font_size = 16.0f) override;
    void DrawText(const std::string& text, const Vec2& position, const Color& color, float scale = 1.0f) override;
    Vec2 GetTextSize(const std::string& text, float scale = 1.0f) override;

    void DrawRect(const Vec2& position, const Vec2& size, const Color& color) override;
    void DrawRectGradient(const Vec2& position, const Vec2& size,
                          const Color& top_color, const Color& bottom_color) override;
    void DrawRectGradientRounded(const Vec2& position, const Vec2& size,
                                 const Color& top_color, const Color& bottom_color,
                                 float corner_radius = 5.0f) override;
    void DrawRectWithBorder(const Vec2& position, const Vec2& size,
                            const Color& fill_color, const Color& border_color,
                            float border_width = 1.0f) override;
    void DrawRectWithRoundedBorder(const Vec2& position, const Vec2& size,
                                   const Color& fill_color, const Color& border_color,
                                   float border_width = 1.0f, float corner_radius = 5.0f) override;

    void BeginBatch() override;
    void EndBatch() override;

    void BeginFrame() override;
    void EndFrame() override;

    bool CreateOffscreenFramebuffer(int width, int height) override;
    void BindOffscreenFramebuffer() override;
    void UnbindOffscreenFramebuffer() override;

    bool InitializePBO(int width, int height) override;
    void CleanupPBO() override;

    std::vector<std::uint8_t> ReadFramebuffer(int width, int height) override;
    std::vector<std::uint8_t> ReadFramebufferPBO(int width, int height) override;
    void StartAsyncReadback(int width, int height) override;
    std::vector<std::uint8_t> GetAsyncReadbackResult(int width, int height) override;

    void RenderOffscreenTextureToScreen(int screen_width, int screen_height) override;
    void RenderPreviewOverlay(int screen_width, int screen_height,
                              const std::vector<std::string>& info_lines,
                              float progress_ratio) override;

    Vec2 ScreenToGL(const Vec2& screen_pos) const override;
    Vec2 GLToScreen(const Vec2& gl_pos) const override;

    void ResetDrawCallCount() override;
    unsigned int GetDrawCallCount() const override;

    bool SupportsPreview() const override { return false; }
    bool SupportsAsyncReadback() const override { return false; }

private:
    enum class ShapeType : std::uint32_t {
        Solid = 0,
        VerticalGradient = 1,
        RoundedGradient = 2,
        Border = 3,
        RoundedBorder = 4,
        RadialGradient = 5
    };

    struct ShapeCommand {
        Vec2 position;
        Vec2 size;
        Color color0;
        Color color1;
        Color color2;
        float radius;
        float border_width;
        float extra0;
        ShapeType type;
    };

    struct TextCommand {
        Vec2 position;
        Vec2 size;
        Color color;
        Vec2 uv0;
        Vec2 uv1;
    };

    struct ShapeVertex {
        float position[2];
        float local[2];
    };

    struct TextVertex {
        float position[2];
        float uv[2];
        float color[4];
    };

    struct VulkanBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };

    struct VertexBufferSet {
        VulkanBuffer device;
        VulkanBuffer staging;
        VkDeviceSize pending_copy_size = 0;
    };

    struct FrameTimings {
        double vertex_upload_ms = 0.0;
        double command_record_ms = 0.0;
        double gpu_wait_ms = 0.0;
        double readback_ms = 0.0;
        double total_ms = 0.0;
    };

    struct GlyphInfo {
        float u0;
        float v0;
        float u1;
        float v1;
        float advance;
    };

    void InitializeVulkan();
    void CleanupVulkan();

    void EnsureFramebufferResources(int width, int height);
    void ReleaseFramebufferResources();

    void CreatePipelines();
    void DestroyPipelines();

    void EnsureFontResources(float font_size);
    void DestroyFontResources();

    void FlushIfNeeded();
    void Flush();
    void RecordCommandBuffer(VkDeviceSize readback_size);
    void SubmitAndWait();

    bool EnsureBufferCapacity(VulkanBuffer& buffer, VkDeviceSize required_size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkMemoryPropertyFlags* used_properties = nullptr);
    void EnsureReadbackBuffer(VkDeviceSize required_size);
    void EnsureVertexBufferCapacity(VertexBufferSet& buffers, VkDeviceSize required_size, VkBufferUsageFlags usage);
    void UploadShapeVertices(const std::vector<ShapeVertex>& vertices);
    void UploadTextVertices(const std::vector<TextVertex>& vertices);
    void ProcessReadbackData(VkDeviceSize required_size);

    void TransitionImageLayout(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout);
    uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties) const;

    void ResetBatches();
    void PushShapeCommand(const ShapeCommand& command);
    void PushTextQuad(const TextCommand& command);

    bool HasRenderableContent() const;

    // Vulkan handles and resources
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    uint32_t graphics_queue_family_index_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence render_fence_ = VK_NULL_HANDLE;

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout text_descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet text_descriptor_set_ = VK_NULL_HANDLE;

    VkPipelineLayout shape_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout text_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline shape_pipeline_ = VK_NULL_HANDLE;
    VkPipeline text_pipeline_ = VK_NULL_HANDLE;

    VkImage color_image_ = VK_NULL_HANDLE;
    VkDeviceMemory color_image_memory_ = VK_NULL_HANDLE;
    VkImageView color_image_view_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageLayout color_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    VertexBufferSet shape_vertex_buffer_;
    VertexBufferSet text_vertex_buffer_;
    VulkanBuffer readback_buffer_;
    void* readback_mapped_ptr_ = nullptr;
    VkDeviceSize readback_mapped_size_ = 0;
    VkMemoryPropertyFlags readback_memory_properties_ = 0;

    VkImage font_image_ = VK_NULL_HANDLE;
    VkDeviceMemory font_image_memory_ = VK_NULL_HANDLE;
    VkImageView font_image_view_ = VK_NULL_HANDLE;
    VkSampler font_sampler_ = VK_NULL_HANDLE;
    bool font_uploaded_ = false;

    // CPU-side state
    int window_width_ = 0;
    int window_height_ = 0;
    int framebuffer_width_ = 0;
    int framebuffer_height_ = 0;

    bool framebuffer_bound_ = false;
    bool offscreen_initialized_ = false;
    bool font_loaded_ = false;

    float requested_font_size_ = 16.0f;
    float font_pixel_scale_ = 1.0f;

    Color clear_color_{0.0f, 0.0f, 0.0f, 1.0f};
    bool has_pending_clear_ = false;

    std::vector<ShapeCommand> shape_commands_;
    std::vector<TextCommand> text_commands_;
    std::vector<ShapeVertex> shape_vertices_;
    std::vector<TextVertex> text_vertices_;
    std::vector<std::uint8_t> readback_cache_;
    std::vector<GlyphInfo> glyph_infos_;

    unsigned int draw_call_count_ = 0;

    bool frame_dirty_ = false;

    FrameTimings last_frame_timings_{};
    std::chrono::steady_clock::time_point last_slow_log_{};

    std::mutex render_mutex_;
};
