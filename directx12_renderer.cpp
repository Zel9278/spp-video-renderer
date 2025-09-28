#include "directx12_renderer.h"

#ifdef _WIN32

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

inline void ThrowIfFailed(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        throw std::runtime_error(message);
    }
}

inline D3D12_RESOURCE_BARRIER TransitionBarrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

constexpr int kGlyphsPerRow = 16;

const char kShapeShaderSource[] = R"(cbuffer ShapeConstants : register(b0)
{
    float4 rect;      // x, y, width, height
    float4 color0;    // primary color
    float4 color1;    // secondary / border color
    float4 params;    // radius, border width, type, extra
    float4 extra0;    // viewport width, viewport height, u0, v0
    float4 extra1;    // u1, v1, glyph width, glyph height (unused for shapes)
}

Texture2D fontTexture : register(t0);
SamplerState fontSampler : register(s0);

struct VSOutput {
    float4 position : SV_Position;
    float2 localPos : TEXCOORD0;
    float2 corner   : TEXCOORD1;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    float2 corner = float2((vertexId & 1), (vertexId >> 1));
    float2 pixelPos = rect.xy + corner * rect.zw;
    float2 viewport = extra0.xy;
    float2 clip;
    clip.x = (viewport.x > 0.0f) ? ((pixelPos.x / viewport.x) * 2.0f - 1.0f) : -1.0f;
    clip.y = (viewport.y > 0.0f) ? (1.0f - (pixelPos.y / viewport.y) * 2.0f) : 1.0f;
    output.position = float4(clip, 0.0f, 1.0f);
    output.localPos = corner * rect.zw;
    output.corner = corner;
    return output;
}

float sdRoundRect(float2 p, float2 halfSize, float radius)
{
    float2 q = abs(p) - max(halfSize - radius, float2(0.0f, 0.0f));
    return length(max(q, float2(0.0f, 0.0f))) + min(max(q.x, q.y), 0.0f) - radius;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float type = params.z;
    float2 size = rect.zw;
    float2 local = input.localPos;
    float4 result = color0;

    if (type == 0.0f) {
        result = color0;
    } else if (type == 1.0f) {
        float t = (size.y > 0.0f) ? saturate(local.y / size.y) : 0.0f;
        result = lerp(color0, color1, t);
    } else if (type == 2.0f) {
        float radius = params.x;
        float2 p = local - size * 0.5f;
        float dist = sdRoundRect(p, size * 0.5f, radius);
        if (dist > 0.0f) discard;
        float t = (size.y > 0.0f) ? saturate(local.y / size.y) : 0.0f;
        result = lerp(color0, color1, t);
    } else if (type == 3.0f) {
        float radius = params.x;
        float borderWidth = params.y;
        float2 p = local - size * 0.5f;
        float distOuter = sdRoundRect(p, size * 0.5f, radius);
        if (distOuter > 0.0f) discard;
        float2 innerHalf = max(size * 0.5f - borderWidth, float2(0.0f, 0.0f));
        float innerRadius = max(radius - borderWidth, 0.0f);
        float distInner = sdRoundRect(p, innerHalf, innerRadius);
        if (distInner < 0.0f) {
            discard;
        } else {
            result = color1;
        }
    } else if (type == 4.0f) {
        float2 center = size * 0.5f;
        float dist = length(local - center);
        float maxDist = max(length(center), length(size - center));
        float t = (maxDist > 0.0f) ? saturate(dist / maxDist) : 0.0f;
        result = lerp(color0, color1, t);
    } else if (type == 5.0f) {
        float2 uv0 = extra0.zw;
        float2 uv1 = extra1.xy;
        float2 uv = lerp(uv0, uv1, input.corner);
        float alpha = fontTexture.Sample(fontSampler, uv).r * color0.a;
        if (alpha <= 0.001f) discard;
        result = float4(color0.rgb * alpha, alpha);
    } else if (type == 6.0f) {
        float borderWidth = params.y;
        bool inside = (local.x >= borderWidth) && (local.x <= size.x - borderWidth) &&
                      (local.y >= borderWidth) && (local.y <= size.y - borderWidth);
        if (inside) {
            discard;
        } else {
            result = color1;
        }
    }

    return result;
}
)";

std::array<float, 4> ToFloat4(const Color& color) {
    return {color.r, color.g, color.b, color.a};
}

} // namespace

DirectX12Renderer::DirectX12Renderer() {
    clear_color_ = {0.0f, 0.0f, 0.0f, 1.0f};
}

DirectX12Renderer::~DirectX12Renderer() {
    DestroyDeviceResources();
    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
}

void DirectX12Renderer::InitializeDevice() {
    UINT factory_flags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ThrowIfFailed(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_)),
                  "Failed to create DXGI factory");

    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
                  "Failed to create D3D12 device");

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue_)),
                  "Failed to create D3D12 command queue");

    ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
                  "Failed to create D3D12 fence");
    fence_value_ = 0;

    fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        throw std::runtime_error("Failed to create fence event");
    }

    rtv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

void DirectX12Renderer::CreateDeviceResources() {
    if (!device_) {
        return;
    }

    ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&command_allocator_)),
                  "Failed to create command allocator");

    ThrowIfFailed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             command_allocator_.Get(), nullptr,
                                             IID_PPV_ARGS(&command_list_)),
                  "Failed to create command list");
    command_list_->Close();

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.NumDescriptors = 1;
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_)),
                  "Failed to create RTV descriptor heap");
    rtv_handle_ = rtv_heap_->GetCPUDescriptorHandleForHeapStart();

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.NumDescriptors = 1;
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device_->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap_)),
                  "Failed to create SRV descriptor heap");
    srv_gpu_handle_ = srv_heap_->GetGPUDescriptorHandleForHeapStart();

    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1;
    srv_range.BaseShaderRegister = 0;
    srv_range.RegisterSpace = 0;
    srv_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER root_parameters[2] = {};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameters[0].Constants.ShaderRegister = 0;
    root_parameters[0].Constants.RegisterSpace = 0;
    root_parameters[0].Constants.Num32BitValues = DirectX12Renderer::kRootConstantCount;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &srv_range;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = _countof(root_parameters);
    root_desc.pParameters = root_parameters;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error_blob;
    HRESULT hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &signature, &error_blob);
    if (FAILED(hr)) {
        std::string message = "Failed to serialize root signature";
        if (error_blob) {
            message += ": ";
            message.append(static_cast<const char*>(error_blob->GetBufferPointer()),
                           error_blob->GetBufferSize());
        }
        throw std::runtime_error(message);
    }

    ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&root_signature_)),
                  "Failed to create root signature");

    UINT compile_flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    hr = D3DCompile(kShapeShaderSource, std::strlen(kShapeShaderSource), nullptr,
                    nullptr, nullptr, "VSMain", "vs_5_0", compile_flags, 0,
                    &vs_blob, &error_blob);
    if (FAILED(hr)) {
        std::string message = "Failed to compile vertex shader";
        if (error_blob) {
            message += ": ";
            message.append(static_cast<const char*>(error_blob->GetBufferPointer()),
                           error_blob->GetBufferSize());
        }
        throw std::runtime_error(message);
    }

    hr = D3DCompile(kShapeShaderSource, std::strlen(kShapeShaderSource), nullptr,
                    nullptr, nullptr, "PSMain", "ps_5_0", compile_flags, 0,
                    &ps_blob, &error_blob);
    if (FAILED(hr)) {
        std::string message = "Failed to compile pixel shader";
        if (error_blob) {
            message += ": ";
            message.append(static_cast<const char*>(error_blob->GetBufferPointer()),
                           error_blob->GetBufferSize());
        }
        throw std::runtime_error(message);
    }

    D3D12_BLEND_DESC blend_desc{};
    blend_desc.AlphaToCoverageEnable = FALSE;
    blend_desc.IndependentBlendEnable = FALSE;
    auto& rt_blend = blend_desc.RenderTarget[0];
    rt_blend.BlendEnable = TRUE;
    rt_blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt_blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt_blend.BlendOp = D3D12_BLEND_OP_ADD;
    rt_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt_blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer_desc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer_desc.FrontCounterClockwise = FALSE;
    rasterizer_desc.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth_desc.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_signature_.Get();
    pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
    pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
    pso_desc.BlendState = blend_desc;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState = rasterizer_desc;
    pso_desc.DepthStencilState = depth_desc;
    pso_desc.InputLayout = {nullptr, 0};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso_desc,
                                                       IID_PPV_ARGS(&pipeline_state_)),
                  "Failed to create pipeline state");

    CreateFontTexture();
}

void DirectX12Renderer::DestroyDeviceResources() {
    if (command_queue_) {
        WaitForGpu();
    }

    ReleaseRenderTarget();
    commands_.clear();
    cpu_buffer_.clear();
    async_buffer_.clear();

    font_texture_.Reset();
    pipeline_state_.Reset();
    root_signature_.Reset();
    command_list_.Reset();
    command_allocator_.Reset();
    rtv_heap_.Reset();
    srv_heap_.Reset();
    fence_.Reset();
    command_queue_.Reset();
    device_.Reset();
    factory_.Reset();

    offscreen_initialized_ = false;
    font_loaded_ = false;
}

void DirectX12Renderer::Initialize(int window_width, int window_height) {
    window_width_ = window_width;
    window_height_ = window_height;

    try {
        InitializeDevice();
        CreateDeviceResources();
    } catch (const std::exception& ex) {
        std::cerr << "DirectX12Renderer initialization warning: " << ex.what() << std::endl;
        DestroyDeviceResources();
        return;
    }

    CreateOffscreenFramebuffer(window_width, window_height);
    ResetForNewFrame();
}

void DirectX12Renderer::SetViewport(int width, int height) {
    window_width_ = width;
    window_height_ = height;
    EnsureFramebufferSize(width, height);
}

void DirectX12Renderer::EnsureFramebufferSize(int width, int height) {
    int clamped_width = std::max(1, width);
    int clamped_height = std::max(1, height);
    if (framebuffer_width_ == clamped_width && framebuffer_height_ == clamped_height && offscreen_initialized_) {
        return;
    }

    framebuffer_width_ = clamped_width;
    framebuffer_height_ = clamped_height;
    CreateRenderTarget(framebuffer_width_, framebuffer_height_);
}

DirectX12Renderer::GPUConstants DirectX12Renderer::MakeBaseConstants(const Vec2& position, const Vec2& size) const {
    GPUConstants constants{};
    constants.rect = {position.x, position.y, size.x, size.y};
    constants.color0 = {0.0f, 0.0f, 0.0f, 0.0f};
    constants.color1 = {0.0f, 0.0f, 0.0f, 0.0f};
    constants.params = {0.0f, 0.0f, 0.0f, 0.0f};
    constants.extra0 = {static_cast<float>(framebuffer_width_), static_cast<float>(framebuffer_height_), 0.0f, 0.0f};
    constants.extra1 = {0.0f, 0.0f, 0.0f, 0.0f};
    return constants;
}

void DirectX12Renderer::PopulateShapeCommand(CommandType type, const GPUConstants& constants) {
    if (!offscreen_initialized_) {
        return;
    }

    DrawCommand command{};
    command.type = type;
    command.constants = constants;
    command.constants.params[2] = static_cast<float>(type);
    command.constants.extra0[0] = static_cast<float>(framebuffer_width_);
    command.constants.extra0[1] = static_cast<float>(framebuffer_height_);

    std::lock_guard<std::mutex> lock(command_mutex_);
    commands_.push_back(command);
    IncrementDrawCall();
}

void DirectX12Renderer::PopulateTextCommand(const GPUConstants& constants) {
    PopulateShapeCommand(CommandType::Text, constants);
}

void DirectX12Renderer::ResetForNewFrame() {
    std::lock_guard<std::mutex> lock(command_mutex_);
    commands_.clear();
    clear_requested_ = false;
}

void DirectX12Renderer::Clear(const Color& clear_color) {
    clear_color_ = ToFloat4(clear_color);
    clear_requested_ = true;
}

void DirectX12Renderer::ClearWithRadialGradient(const Color& center_color, const Color& edge_color) {
    clear_color_ = ToFloat4(edge_color);
    clear_requested_ = true;

    GPUConstants constants = MakeBaseConstants(Vec2(0.0f, 0.0f),
                                               Vec2(static_cast<float>(framebuffer_width_),
                                                    static_cast<float>(framebuffer_height_)));
    constants.color0 = ToFloat4(center_color);
    constants.color1 = ToFloat4(edge_color);
    PopulateShapeCommand(CommandType::RadialGradient, constants);
}

void DirectX12Renderer::ClearWithImage(const std::string& image_path, float opacity, int scale_mode) {
    (void)image_path;
    (void)opacity;
    (void)scale_mode;
    Clear(Color(0.0f, 0.0f, 0.0f, 1.0f));
}

bool DirectX12Renderer::LoadFont(float font_size) {
    font_scale_ = std::max(0.1f, font_size / 16.0f);
    return font_loaded_;
}

void DirectX12Renderer::DrawText(const std::string& text, const Vec2& position, const Color& color, float scale) {
    if (!font_loaded_ || !offscreen_initialized_) {
        return;
    }

    const float effective_scale = scale * font_scale_;
    const float pixel_size = std::max(1.0f, effective_scale);
    const float glyph_width = pixel_size * simple_font::kGlyphWidth;
    const float glyph_height = pixel_size * simple_font::kGlyphHeight;
    const float char_advance = (simple_font::kGlyphWidth + 1) * pixel_size;
    const float line_spacing = 2.0f * pixel_size;

    float current_x = position.x;
    float current_y = position.y;
    auto color_vec = ToFloat4(color);

    for (char c : text) {
        if (c == '\n') {
            current_x = position.x;
            current_y += glyph_height + line_spacing;
            continue;
        }

        const unsigned char* glyph = simple_font::GlyphData(c);
        if (!glyph) {
            current_x += char_advance;
            continue;
        }

        int glyph_index = c - simple_font::kFirstChar;
        if (glyph_index < 0 || glyph_index >= simple_font::kCharCount) {
            current_x += char_advance;
            continue;
        }

        int glyph_row = glyph_index / kGlyphsPerRow;
        int glyph_col = glyph_index % kGlyphsPerRow;

        float u0 = (glyph_col * simple_font::kGlyphWidth) / static_cast<float>(font_texture_width_);
        float v0 = (glyph_row * simple_font::kGlyphHeight) / static_cast<float>(font_texture_height_);
        float u1 = ((glyph_col + 1) * simple_font::kGlyphWidth) / static_cast<float>(font_texture_width_);
        float v1 = ((glyph_row + 1) * simple_font::kGlyphHeight) / static_cast<float>(font_texture_height_);

        GPUConstants constants = MakeBaseConstants(Vec2(current_x, current_y),
                                                   Vec2(glyph_width, glyph_height));
        constants.color0 = color_vec;
        constants.extra0[2] = u0;
        constants.extra0[3] = v0;
        constants.extra1[0] = u1;
        constants.extra1[1] = v1;
        PopulateTextCommand(constants);

        current_x += char_advance;
    }
}

Vec2 DirectX12Renderer::GetTextSize(const std::string& text, float scale) {
    float effective_scale = scale * font_scale_;
    float char_width = (simple_font::kGlyphWidth + 1) * effective_scale;
    float char_height = simple_font::kGlyphHeight * effective_scale;
    float line_spacing = 2.0f * effective_scale;

    size_t max_line_length = 0;
    size_t current_length = 0;
    int line_count = 1;

    for (char c : text) {
        if (c == '\n') {
            max_line_length = std::max(max_line_length, current_length);
            current_length = 0;
            line_count++;
        } else {
            current_length++;
        }
    }

    max_line_length = std::max(max_line_length, current_length);
    float width = max_line_length * char_width;
    float height = line_count * char_height + (line_count - 1) * line_spacing;
    return Vec2(width, height);
}

void DirectX12Renderer::DrawRect(const Vec2& position, const Vec2& size, const Color& color) {
    GPUConstants constants = MakeBaseConstants(position, size);
    constants.color0 = ToFloat4(color);
    PopulateShapeCommand(CommandType::SolidRect, constants);
}

void DirectX12Renderer::DrawRectGradient(const Vec2& position, const Vec2& size,
                                         const Color& top_color, const Color& bottom_color) {
    GPUConstants constants = MakeBaseConstants(position, size);
    constants.color0 = ToFloat4(top_color);
    constants.color1 = ToFloat4(bottom_color);
    PopulateShapeCommand(CommandType::VerticalGradient, constants);
}

void DirectX12Renderer::DrawRectGradientRounded(const Vec2& position, const Vec2& size,
                                                const Color& top_color, const Color& bottom_color,
                                                float corner_radius) {
    GPUConstants constants = MakeBaseConstants(position, size);
    constants.color0 = ToFloat4(top_color);
    constants.color1 = ToFloat4(bottom_color);
    constants.params[0] = corner_radius;
    PopulateShapeCommand(CommandType::RoundedGradient, constants);
}

void DirectX12Renderer::DrawRectWithBorder(const Vec2& position, const Vec2& size,
                                           const Color& fill_color, const Color& border_color,
                                           float border_width) {
    if (fill_color.a > 0.0f) {
        DrawRect(position, size, fill_color);
    }

    if (border_width <= 0.0f) {
        return;
    }

    GPUConstants constants = MakeBaseConstants(position, size);
    constants.color1 = ToFloat4(border_color);
    constants.params[1] = border_width;
    PopulateShapeCommand(CommandType::Border, constants);
}

void DirectX12Renderer::DrawRectWithRoundedBorder(const Vec2& position, const Vec2& size,
                                                  const Color& fill_color, const Color& border_color,
                                                  float border_width, float corner_radius) {
    if (fill_color.a > 0.0f) {
        DrawRectGradientRounded(position, size, fill_color, fill_color, corner_radius);
    }

    if (border_width <= 0.0f) {
        return;
    }

    GPUConstants constants = MakeBaseConstants(position, size);
    constants.color1 = ToFloat4(border_color);
    constants.params[0] = corner_radius;
    constants.params[1] = border_width;
    PopulateShapeCommand(CommandType::RoundedBorder, constants);
}

void DirectX12Renderer::BeginBatch() {}
void DirectX12Renderer::EndBatch() {}

void DirectX12Renderer::BeginFrame() {
    ResetDrawCallCount();
    ResetForNewFrame();
}

void DirectX12Renderer::EndFrame() {
    FlushCommandList();
}

bool DirectX12Renderer::CreateOffscreenFramebuffer(int width, int height) {
    EnsureFramebufferSize(width, height);
    return offscreen_initialized_;
}

void DirectX12Renderer::BindOffscreenFramebuffer() {
    frame_bound_ = true;
    ResetForNewFrame();
}

void DirectX12Renderer::UnbindOffscreenFramebuffer() {
    FlushCommandList();
    frame_bound_ = false;
}

bool DirectX12Renderer::InitializePBO(int width, int height) {
    (void)width;
    (void)height;
    return false;
}

void DirectX12Renderer::CleanupPBO() {
    ReleaseRenderTarget();
}

std::vector<std::uint8_t> DirectX12Renderer::ReadFramebuffer(int width, int height) {
    if (width != framebuffer_width_ || height != framebuffer_height_) {
        return {};
    }

    return cpu_buffer_;
}

std::vector<std::uint8_t> DirectX12Renderer::ReadFramebufferPBO(int width, int height) {
    return ReadFramebuffer(width, height);
}

void DirectX12Renderer::StartAsyncReadback(int width, int height) {
    if (width != framebuffer_width_ || height != framebuffer_height_) {
        async_buffer_.clear();
    } else {
        async_buffer_ = cpu_buffer_;
    }
}

std::vector<std::uint8_t> DirectX12Renderer::GetAsyncReadbackResult(int width, int height) {
    if (width != framebuffer_width_ || height != framebuffer_height_) {
        return {};
    }
    return async_buffer_;
}

void DirectX12Renderer::RenderOffscreenTextureToScreen(int screen_width, int screen_height) {
    (void)screen_width;
    (void)screen_height;
    // Preview not supported in DirectX 12 renderer.
}

void DirectX12Renderer::RenderPreviewOverlay(int screen_width, int screen_height,
                                             const std::vector<std::string>& info_lines,
                                             float progress_ratio) {
    (void)screen_width;
    (void)screen_height;
    (void)info_lines;
    (void)progress_ratio;
    // Preview overlay not supported.
}

Vec2 DirectX12Renderer::ScreenToGL(const Vec2& screen_pos) const {
    return screen_pos;
}

Vec2 DirectX12Renderer::GLToScreen(const Vec2& gl_pos) const {
    return gl_pos;
}

void DirectX12Renderer::ResetDrawCallCount() {
    draw_call_count_ = 0;
}

unsigned int DirectX12Renderer::GetDrawCallCount() const {
    return draw_call_count_;
}

void DirectX12Renderer::FlushCommandList() {
    if (!offscreen_initialized_ || !command_queue_ || !command_allocator_ || !command_list_) {
        return;
    }

    std::vector<DrawCommand> frame_commands;
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        frame_commands = commands_;
        commands_.clear();
    }

    command_allocator_->Reset();
    command_list_->Reset(command_allocator_.Get(), pipeline_state_.Get());

    command_list_->ResourceBarrier(1, &TransitionBarrier(render_target_.Get(),
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET));

    command_list_->RSSetViewports(1, &viewport_);
    command_list_->RSSetScissorRects(1, &scissor_rect_);
    command_list_->OMSetRenderTargets(1, &rtv_handle_, FALSE, nullptr);

    if (clear_requested_) {
        command_list_->ClearRenderTargetView(rtv_handle_, clear_color_.data(), 0, nullptr);
    }

    if (!frame_commands.empty()) {
        ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
        command_list_->SetDescriptorHeaps(1, heaps);
        command_list_->SetGraphicsRootSignature(root_signature_.Get());
        command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        for (const auto& cmd : frame_commands) {
            const float* data = reinterpret_cast<const float*>(&cmd.constants);
            command_list_->SetGraphicsRoot32BitConstants(0, DirectX12Renderer::kRootConstantCount, data, 0);
            if (cmd.type == CommandType::Text) {
                command_list_->SetGraphicsRootDescriptorTable(1, srv_gpu_handle_);
            }
            command_list_->DrawInstanced(4, 1, 0, 0);
        }
    }

    command_list_->ResourceBarrier(1, &TransitionBarrier(render_target_.Get(),
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE));

    D3D12_TEXTURE_COPY_LOCATION dest{};
    dest.pResource = readback_buffer_.Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dest.PlacedFootprint = readback_footprint_;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = render_target_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    command_list_->CopyTextureRegion(&dest, 0, 0, 0, &src, nullptr);

    command_list_->ResourceBarrier(1, &TransitionBarrier(render_target_.Get(),
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET));

    command_list_->Close();

    ID3D12CommandList* lists[] = {command_list_.Get()};
    command_queue_->ExecuteCommandLists(1, lists);

    WaitForGpu();
    CopyRenderTargetToCpu();

    clear_requested_ = false;
}

void DirectX12Renderer::WaitForGpu() {
    if (!command_queue_ || !fence_) {
        return;
    }

    fence_value_++;
    ThrowIfFailed(command_queue_->Signal(fence_.Get(), fence_value_),
                  "Failed to signal fence");

    if (fence_->GetCompletedValue() < fence_value_) {
        ThrowIfFailed(fence_->SetEventOnCompletion(fence_value_, fence_event_),
                      "Failed to set fence event");
        WaitForSingleObject(fence_event_, INFINITE);
    }
}

void DirectX12Renderer::CopyRenderTargetToCpu() {
    if (!readback_buffer_) {
        return;
    }

    D3D12_RANGE range{0, static_cast<SIZE_T>(readback_buffer_size_)};
    uint8_t* mapped = nullptr;
    HRESULT hr = readback_buffer_->Map(0, &range, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr) || !mapped) {
        return;
    }

    cpu_buffer_.resize(static_cast<size_t>(framebuffer_width_) * framebuffer_height_ * 4);
    for (int y = 0; y < framebuffer_height_; ++y) {
        const uint8_t* src_row = mapped + readback_footprint_.Footprint.RowPitch * y;
        uint8_t* dst_row = cpu_buffer_.data() + static_cast<size_t>(y) * framebuffer_width_ * 4;
        std::memcpy(dst_row, src_row, static_cast<size_t>(framebuffer_width_) * 4);
    }
    readback_buffer_->Unmap(0, nullptr);

    async_buffer_ = cpu_buffer_;
}

bool DirectX12Renderer::CreateRenderTarget(int width, int height) {
    ReleaseRenderTarget();

    if (!device_) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Alignment = 0;
    rt_desc.Width = static_cast<UINT64>(width);
    rt_desc.Height = static_cast<UINT>(height);
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1;
    rt_desc.SampleDesc.Quality = 0;
    rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = rt_desc.Format;
    clear_value.Color[0] = clear_color_[0];
    clear_value.Color[1] = clear_color_[1];
    clear_value.Color[2] = clear_color_[2];
    clear_value.Color[3] = clear_color_[3];

    ThrowIfFailed(device_->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE,
                                                   &rt_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   &clear_value, IID_PPV_ARGS(&render_target_)),
                  "Failed to create render target");

    device_->CreateRenderTargetView(render_target_.Get(), nullptr, rtv_handle_);

    UINT64 total_bytes = 0;
    UINT num_rows = 0;
    UINT64 row_size = 0;
    device_->GetCopyableFootprints(&rt_desc, 0, 1, 0, &readback_footprint_, &num_rows, &row_size, &total_bytes);
    readback_buffer_size_ = total_bytes;

    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = total_bytes;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.Format = DXGI_FORMAT_UNKNOWN;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    readback_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ThrowIfFailed(device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                                   &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&readback_buffer_)),
                  "Failed to create readback buffer");

    viewport_.TopLeftX = 0.0f;
    viewport_.TopLeftY = 0.0f;
    viewport_.Width = static_cast<float>(width);
    viewport_.Height = static_cast<float>(height);
    viewport_.MinDepth = 0.0f;
    viewport_.MaxDepth = 1.0f;

    scissor_rect_.left = 0;
    scissor_rect_.top = 0;
    scissor_rect_.right = width;
    scissor_rect_.bottom = height;

    cpu_buffer_.resize(static_cast<size_t>(width) * height * 4);
    async_buffer_.resize(cpu_buffer_.size());
    offscreen_initialized_ = true;
    return true;
}

void DirectX12Renderer::ReleaseRenderTarget() {
    render_target_.Reset();
    readback_buffer_.Reset();
    offscreen_initialized_ = false;
}

void DirectX12Renderer::CreateFontTexture() {
    font_texture_width_ = kGlyphsPerRow * simple_font::kGlyphWidth;
    int glyph_rows = (simple_font::kCharCount + kGlyphsPerRow - 1) / kGlyphsPerRow;
    font_texture_height_ = glyph_rows * simple_font::kGlyphHeight;

    std::vector<uint8_t> font_data(static_cast<size_t>(font_texture_width_ * font_texture_height_), 0);

    for (int index = 0; index < simple_font::kCharCount; ++index) {
        int row = index / kGlyphsPerRow;
        int col = index % kGlyphsPerRow;
        int x_base = col * simple_font::kGlyphWidth;
        int y_base = row * simple_font::kGlyphHeight;

        const unsigned char* glyph = simple_font::kFont5x8[index];
        for (int y = 0; y < simple_font::kGlyphHeight; ++y) {
            for (int x = 0; x < simple_font::kGlyphWidth; ++x) {
                if (glyph[y] & (1 << (simple_font::kGlyphWidth - 1 - x))) {
                    int px = x_base + x;
                    int py = y_base + y;
                    font_data[py * font_texture_width_ + px] = 255;
                }
            }
        }
    }

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = static_cast<UINT>(font_texture_width_);
    texture_desc.Height = static_cast<UINT>(font_texture_height_);
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ThrowIfFailed(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
                                                   &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&font_texture_)),
                  "Failed to create font texture");

    UINT64 upload_size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    device_->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, &num_rows, &row_size, &upload_size);

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = upload_size;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.Format = DXGI_FORMAT_UNKNOWN;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload_buffer;
    ThrowIfFailed(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
                                                   &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&upload_buffer)),
                  "Failed to create font upload buffer");

    uint8_t* mapped = nullptr;
    D3D12_RANGE range{0, 0};
    upload_buffer->Map(0, &range, reinterpret_cast<void**>(&mapped));
    for (UINT row = 0; row < num_rows; ++row) {
        std::memcpy(mapped + footprint.Offset + footprint.Footprint.RowPitch * row,
                    font_data.data() + row * font_texture_width_, font_texture_width_);
    }
    upload_buffer->Unmap(0, nullptr);

    command_allocator_->Reset();
    command_list_->Reset(command_allocator_.Get(), nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = font_texture_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload_buffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    command_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    command_list_->ResourceBarrier(1, &TransitionBarrier(font_texture_.Get(),
                                                         D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    command_list_->Close();

    ID3D12CommandList* lists[] = {command_list_.Get()};
    command_queue_->ExecuteCommandLists(1, lists);
    WaitForGpu();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(font_texture_.Get(), &srv_desc,
                                      srv_heap_->GetCPUDescriptorHandleForHeapStart());

    font_loaded_ = true;
}

#endif // _WIN32
