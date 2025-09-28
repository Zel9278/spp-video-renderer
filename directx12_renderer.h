#pragma once

#ifdef _WIN32

#include "renderer.h"
#include "simple_bitmap_font.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#ifdef _WIN32
#undef DrawText
#endif

#include <array>
#include <mutex>
#include <vector>

class DirectX12Renderer : public RendererBackend {
public:
    DirectX12Renderer();
    ~DirectX12Renderer() override;

    const char* GetName() const override { return "DirectX 12"; }

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
    using ComPtrDXGI = Microsoft::WRL::ComPtr<IDXGIFactory6>;
    using ComPtrDevice = Microsoft::WRL::ComPtr<ID3D12Device>;
    using ComPtrCommandQueue = Microsoft::WRL::ComPtr<ID3D12CommandQueue>;

    struct GPUConstants {
        std::array<float, 4> rect;       // x, y, width, height
        std::array<float, 4> color0;     // primary color
        std::array<float, 4> color1;     // secondary/border color
        std::array<float, 4> params;     // radius, border width, type, extra
        std::array<float, 4> extra0;     // viewport width, viewport height, u0, v0
        std::array<float, 4> extra1;     // u1, v1, glyph width, glyph height
    };

    enum class CommandType : std::uint32_t {
        SolidRect = 0,
        VerticalGradient = 1,
        RoundedGradient = 2,
        RoundedBorder = 3,
        RadialGradient = 4,
        Text = 5,
        Border = 6
    };

    struct DrawCommand {
        CommandType type;
        GPUConstants constants;
    };

    static constexpr UINT kRootConstantCount = static_cast<UINT>(sizeof(GPUConstants) / sizeof(float));

    void InitializeDevice();
    void CreateDeviceResources();
    void DestroyDeviceResources();
    bool CreateRenderTarget(int width, int height);
    void ReleaseRenderTarget();
    void EnsureFramebufferSize(int width, int height);
    void FlushCommandList();
    void WaitForGpu();
    void CopyRenderTargetToCpu();
    void CreateFontTexture();
    void PopulateShapeCommand(CommandType type, const GPUConstants& constants);
    void PopulateTextCommand(const GPUConstants& constants);
    GPUConstants MakeBaseConstants(const Vec2& position, const Vec2& size) const;
    void ResetForNewFrame();
    inline void IncrementDrawCall() { ++draw_call_count_; }

    ComPtrDXGI factory_;
    ComPtrDevice device_;
    ComPtrCommandQueue command_queue_;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> command_allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12Resource> render_target_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> font_texture_;

    int font_texture_width_ = 0;
    int font_texture_height_ = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle_{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_handle_{};
    UINT rtv_descriptor_size_ = 0;
    UINT64 fence_value_ = 0;
    HANDLE fence_event_ = nullptr;

    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_rect_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readback_footprint_{};
    UINT64 readback_buffer_size_ = 0;

    int window_width_ = 0;
    int window_height_ = 0;
    int framebuffer_width_ = 0;
    int framebuffer_height_ = 0;

    bool offscreen_initialized_ = false;
    bool frame_bound_ = false;
    bool font_loaded_ = false;
    bool clear_requested_ = false;
    float font_scale_ = 1.0f;

    std::array<float, 4> clear_color_{};
    std::vector<std::uint8_t> cpu_buffer_;
    std::vector<std::uint8_t> async_buffer_;
    std::vector<DrawCommand> commands_;

    unsigned int draw_call_count_ = 0;
    std::mutex command_mutex_;
};

#endif // _WIN32
