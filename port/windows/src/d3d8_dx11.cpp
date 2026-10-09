/*
D3D8_DX11.CPP

Native Direct3D 11 backend for OpenCE on Windows / Xbox One UWP.
Translates NV2A graphics commands and states directly to Direct3D 11,
bypassing OpenGL and Mesa entirely.
*/

#include "d3d8_dx11.h"
#include "nv2a_hlsl.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <cstdio>
#include <cstring>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

struct vertex_attribute {
    int size = 0;
    uint32_t type = 0;
    bool normalized = false;
    int stride = 0;
    uint32_t offset = 0;
    bool enabled = false;
};

static vertex_attribute g_attribs[16];

struct vertex_constants_cb {
    float c[192][4];
    float viewport_scale[4];
    float viewport_offset[4];
    float point_size;
    float screen_offset;
    float pad[2];
};

constexpr size_t VB_SIZE = 16 * 1024 * 1024; // 16 MB dynamic ring buffer
constexpr size_t IB_SIZE = 2 * 1024 * 1024;  // 2 MB index buffer
static std::vector<uint8_t> g_vb_shadow;
static std::vector<uint8_t> g_ib_shadow;
static uint32_t g_vb_dirty_max = 0;
static uint32_t g_ib_dirty_max = 0;
static bool g_vb_dirty = false;
static bool g_ib_dirty = false;

struct dx11_context {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap_chain;
    ComPtr<ID3D11RenderTargetView> back_buffer_rtv;
    ComPtr<ID3D11DepthStencilView> depth_stencil_dsv;
    ComPtr<ID3D11Texture2D> depth_stencil_texture;

    ComPtr<ID3D11Buffer> vs_constants;
    ComPtr<ID3D11Buffer> ps_constants;
    ComPtr<ID3D11Buffer> dynamic_vb;
    ComPtr<ID3D11Buffer> dynamic_ib;

    ComPtr<ID3D11RasterizerState> raster_state_default;
    ComPtr<ID3D11BlendState> blend_state_default;
    ComPtr<ID3D11DepthStencilState> depth_state_default;
    ComPtr<ID3D11SamplerState> sampler_linear_wrap;
    ComPtr<ID3D11SamplerState> sampler_point_clamp;

    ComPtr<ID3D11VertexShader> fallback_vs;
    ComPtr<ID3D11PixelShader> fallback_ps;
    ComPtr<ID3DBlob> vs_bytecode;
    std::unordered_map<uint64_t, ComPtr<ID3D11InputLayout>> input_layouts;

    D3D11_VIEWPORT current_viewport{};
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    int width = 1920;
    int height = 1080;
    bool initialized = false;
};

static dx11_context g_dx11;

static DXGI_FORMAT get_dxgi_format(int size, uint32_t type, bool normalized)
{
    // GL_FLOAT = 0x1406
    if (type == 0x1406) {
        if (size == 1) return DXGI_FORMAT_R32_FLOAT;
        if (size == 2) return DXGI_FORMAT_R32G32_FLOAT;
        if (size == 3) return DXGI_FORMAT_R32G32B32_FLOAT;
        if (size == 4) return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
    // GL_UNSIGNED_BYTE = 0x1401
    if (type == 0x1401) {
        if (size == 4) return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    // GL_SHORT = 0x1402
    if (type == 0x1402) {
        if (normalized) {
            if (size == 1) return DXGI_FORMAT_R16_SNORM;
            if (size == 2) return DXGI_FORMAT_R16G16_SNORM;
            if (size == 4) return DXGI_FORMAT_R16G16B16A16_SNORM;
        } else {
            if (size == 1) return DXGI_FORMAT_R16_SINT;
            if (size == 2) return DXGI_FORMAT_R16G16_SINT;
            if (size == 4) return DXGI_FORMAT_R16G16B16A16_SINT;
        }
    }
    // GL_UNSIGNED_INT = 0x1405
    if (type == 0x1405) {
        if (size == 1) return DXGI_FORMAT_R10G10B10A2_UNORM;
        if (size == 4) return DXGI_FORMAT_R32G32B32A32_UINT;
        return DXGI_FORMAT_R32_UINT;
    }
    // GL_INT_2_10_10_10_REV = 0x8D9F or GL_UNSIGNED_INT_2_10_10_10_REV = 0x8368
    if (type == 0x8D9F || type == 0x8368) {
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    return DXGI_FORMAT_R32G32B32A32_FLOAT;
}

static D3D_PRIMITIVE_TOPOLOGY gl_mode_to_d3d11_topology(uint32_t mode)
{
    switch (mode) {
    case 0x0000: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;     // GL_POINTS
    case 0x0001: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;      // GL_LINES
    case 0x0002: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;     // GL_LINE_LOOP
    case 0x0003: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;     // GL_LINE_STRIP
    case 0x0004: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;  // GL_TRIANGLES
    case 0x0005: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; // GL_TRIANGLE_STRIP
    case 0x0006: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; // GL_TRIANGLE_FAN
    default:     return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static void d3d8_dx11_prepare_draw(D3D_PRIMITIVE_TOPOLOGY topology)
{
    if (!g_dx11.context) return;

    // 1. Ensure Render Target is bound
    ID3D11RenderTargetView *rtvs[] = { g_dx11.back_buffer_rtv.Get() };
    g_dx11.context->OMSetRenderTargets(1, rtvs, g_dx11.depth_stencil_dsv.Get());

    // 2. Upload dirty vertex and index buffers
    if (g_vb_dirty && g_vb_dirty_max > 0) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = g_dx11.context->Map(g_dx11.dynamic_vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, g_vb_shadow.data(), g_vb_dirty_max);
            g_dx11.context->Unmap(g_dx11.dynamic_vb.Get(), 0);
            g_vb_dirty = false;
        }
    }
    if (g_ib_dirty && g_ib_dirty_max > 0) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = g_dx11.context->Map(g_dx11.dynamic_ib.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, g_ib_shadow.data(), g_ib_dirty_max);
            g_dx11.context->Unmap(g_dx11.dynamic_ib.Get(), 0);
            g_ib_dirty = false;
        }
    }

    // 3. Set Index Buffer
    g_dx11.context->IASetIndexBuffer(g_dx11.dynamic_ib.Get(), DXGI_FORMAT_R16_UINT, 0);

    // 4. Update Viewport / VS Constants
    if (g_dx11.vs_constants) {
        D3D11_MAPPED_SUBRESOURCE mapped_cb{};
        if (SUCCEEDED(g_dx11.context->Map(g_dx11.vs_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_cb))) {
            vertex_constants_cb cb{};
            cb.viewport_scale[0] = g_dx11.current_viewport.Width * 0.5f;
            cb.viewport_scale[1] = -g_dx11.current_viewport.Height * 0.5f;
            cb.viewport_scale[2] = 1.0f;
            cb.viewport_scale[3] = 0.0f;
            cb.viewport_offset[0] = g_dx11.current_viewport.TopLeftX + g_dx11.current_viewport.Width * 0.5f;
            cb.viewport_offset[1] = g_dx11.current_viewport.TopLeftY + g_dx11.current_viewport.Height * 0.5f;
            cb.viewport_offset[2] = 0.0f;
            cb.viewport_offset[3] = 0.0f;
            cb.point_size = 1.0f;
            cb.screen_offset = 0.0f;
            memcpy(mapped_cb.pData, &cb, sizeof(cb));
            g_dx11.context->Unmap(g_dx11.vs_constants.Get(), 0);
        }
        ID3D11Buffer *cbs[] = { g_dx11.vs_constants.Get() };
        g_dx11.context->VSSetConstantBuffers(0, 1, cbs);
    }

    // 5. Set Vertex Buffers & Build Complete 16-Stream Input Layout
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    elements.reserve(16);
    ID3D11Buffer *buffers[16] = {};
    UINT strides[16] = {};
    UINT offsets[16] = {};

    for (UINT i = 0; i < 16; ++i) {
        buffers[i] = g_dx11.dynamic_vb.Get();
        D3D11_INPUT_ELEMENT_DESC desc{};
        desc.SemanticName = "TEXCOORD";
        desc.SemanticIndex = i;
        desc.InputSlot = i;
        desc.AlignedByteOffset = 0;
        desc.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        desc.InstanceDataStepRate = 0;

        if (g_attribs[i].enabled && g_attribs[i].stride > 0) {
            strides[i] = static_cast<UINT>(g_attribs[i].stride);
            offsets[i] = static_cast<UINT>(g_attribs[i].offset < VB_SIZE ? g_attribs[i].offset : 0);
            desc.Format = get_dxgi_format(g_attribs[i].size, g_attribs[i].type, g_attribs[i].normalized);
        } else {
            strides[i] = 16;
            offsets[i] = 0;
            desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
        elements.push_back(desc);
    }

    g_dx11.context->IASetVertexBuffers(0, 16, buffers, strides, offsets);

    if (g_dx11.vs_bytecode) {
        uint64_t hash = 14695981039346656037ULL;
        const uint8_t *p = reinterpret_cast<const uint8_t *>(elements.data());
        size_t sz = elements.size() * sizeof(D3D11_INPUT_ELEMENT_DESC);
        for (size_t b = 0; b < sz; ++b) {
            hash ^= p[b];
            hash *= 1099511628211ULL;
        }

        auto it = g_dx11.input_layouts.find(hash);
        if (it == g_dx11.input_layouts.end()) {
            ComPtr<ID3D11InputLayout> layout;
            HRESULT hr = g_dx11.device->CreateInputLayout(
                elements.data(),
                static_cast<UINT>(elements.size()),
                g_dx11.vs_bytecode->GetBufferPointer(),
                g_dx11.vs_bytecode->GetBufferSize(),
                layout.GetAddressOf()
            );
            if (SUCCEEDED(hr)) {
                g_dx11.input_layouts[hash] = layout;
                g_dx11.context->IASetInputLayout(layout.Get());
            }
        } else {
            g_dx11.context->IASetInputLayout(it->second.Get());
        }
    }

    // 6. Set Shaders & Pipeline States
    g_dx11.context->VSSetShader(g_dx11.fallback_vs.Get(), nullptr, 0);
    g_dx11.context->PSSetShader(g_dx11.fallback_ps.Get(), nullptr, 0);
    g_dx11.context->IASetPrimitiveTopology(topology);
    g_dx11.context->RSSetState(g_dx11.raster_state_default.Get());
    g_dx11.context->OMSetBlendState(g_dx11.blend_state_default.Get(), nullptr, 0xFFFFFFFF);
    g_dx11.context->OMSetDepthStencilState(g_dx11.depth_state_default.Get(), 0);
}

} // namespace

bool d3d8_dx11_initialize_uwp(void *core_window, int width, int height)
{
    if (g_dx11.initialized) return true;

    g_dx11.width = width > 0 ? width : 1920;
    g_dx11.height = height > 0 ? height : 1080;

    UINT creation_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    creation_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL obtained_level;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        creation_flags,
        feature_levels,
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        g_dx11.device.GetAddressOf(),
        &obtained_level,
        g_dx11.context.GetAddressOf()
    );

    if (FAILED(hr)) {
        // Fallback without debug flag
        creation_flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            creation_flags,
            feature_levels,
            ARRAYSIZE(feature_levels),
            D3D11_SDK_VERSION,
            g_dx11.device.GetAddressOf(),
            &obtained_level,
            g_dx11.context.GetAddressOf()
        );
        if (FAILED(hr)) return false;
    }

    // DXGI SwapChain creation for UWP CoreWindow
    ComPtr<IDXGIDevice1> dxgi_device;
    hr = g_dx11.device.As(&dxgi_device);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = g_dx11.width;
    desc.Height = g_dx11.height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    if (core_window) {
        hr = factory->CreateSwapChainForCoreWindow(
            g_dx11.device.Get(),
            static_cast<IUnknown *>(core_window),
            &desc,
            nullptr,
            g_dx11.swap_chain.GetAddressOf()
        );
    }

    if (FAILED(hr) || !g_dx11.swap_chain) {
        return false;
    }

    // Create RTV
    ComPtr<ID3D11Texture2D> back_buffer;
    hr = g_dx11.swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr)) return false;

    hr = g_dx11.device->CreateRenderTargetView(back_buffer.Get(), nullptr, g_dx11.back_buffer_rtv.GetAddressOf());
    if (FAILED(hr)) return false;

    // Create Depth Stencil buffer
    D3D11_TEXTURE2D_DESC ds_desc{};
    ds_desc.Width = g_dx11.width;
    ds_desc.Height = g_dx11.height;
    ds_desc.MipLevels = 1;
    ds_desc.ArraySize = 1;
    ds_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    ds_desc.SampleDesc.Count = 1;
    ds_desc.Usage = D3D11_USAGE_DEFAULT;
    ds_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = g_dx11.device->CreateTexture2D(&ds_desc, nullptr, g_dx11.depth_stencil_texture.GetAddressOf());
    if (SUCCEEDED(hr)) {
        g_dx11.device->CreateDepthStencilView(g_dx11.depth_stencil_texture.Get(), nullptr, g_dx11.depth_stencil_dsv.GetAddressOf());
    }

    // Constant buffers
    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(vertex_constants_cb);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dx11.device->CreateBuffer(&cb_desc, nullptr, g_dx11.vs_constants.GetAddressOf());

    cb_desc.ByteWidth = 512; // Pixel shader uniforms
    g_dx11.device->CreateBuffer(&cb_desc, nullptr, g_dx11.ps_constants.GetAddressOf());

    // Dynamic VB / IB
    D3D11_BUFFER_DESC vb_desc{};
    vb_desc.ByteWidth = VB_SIZE;
    vb_desc.Usage = D3D11_USAGE_DYNAMIC;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dx11.device->CreateBuffer(&vb_desc, nullptr, g_dx11.dynamic_vb.GetAddressOf());

    D3D11_BUFFER_DESC ib_desc{};
    ib_desc.ByteWidth = IB_SIZE;
    ib_desc.Usage = D3D11_USAGE_DYNAMIC;
    ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ib_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dx11.device->CreateBuffer(&ib_desc, nullptr, g_dx11.dynamic_ib.GetAddressOf());

    // Default Rasterizer State
    D3D11_RASTERIZER_DESC rast_desc{};
    rast_desc.FillMode = D3D11_FILL_SOLID;
    rast_desc.CullMode = D3D11_CULL_NONE;
    rast_desc.DepthClipEnable = FALSE;
    g_dx11.device->CreateRasterizerState(&rast_desc, g_dx11.raster_state_default.GetAddressOf());

    // Default Blend State
    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dx11.device->CreateBlendState(&blend_desc, g_dx11.blend_state_default.GetAddressOf());

    // Default Depth Stencil State (disabled initially so UI & early geometry are visible)
    D3D11_DEPTH_STENCIL_DESC ds_state_desc{};
    ds_state_desc.DepthEnable = FALSE;
    ds_state_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds_state_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    g_dx11.device->CreateDepthStencilState(&ds_state_desc, g_dx11.depth_state_default.GetAddressOf());

    // Samplers
    D3D11_SAMPLER_DESC samp_desc{};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    g_dx11.device->CreateSamplerState(&samp_desc, g_dx11.sampler_linear_wrap.GetAddressOf());

    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_dx11.device->CreateSamplerState(&samp_desc, g_dx11.sampler_point_clamp.GetAddressOf());

    // Compile fallback vertex & pixel shaders
    static const char s_fallback_hlsl[] =
        "cbuffer VertexConstants : register(b0)\n"
        "{\n"
        "    float4 c[192];\n"
        "    float4 viewport_scale;\n"
        "    float4 viewport_offset;\n"
        "    float point_size;\n"
        "    float screen_offset;\n"
        "    float2 _pad_vs;\n"
        "};\n"
        "struct VS_INPUT\n"
        "{\n"
        "    float4 v0 : TEXCOORD0;\n"
        "    float4 v1 : TEXCOORD1;\n"
        "    float4 v2 : TEXCOORD2;\n"
        "    float4 v3 : TEXCOORD3;\n"
        "    float4 v4 : TEXCOORD4;\n"
        "    float4 v5 : TEXCOORD5;\n"
        "    float4 v6 : TEXCOORD6;\n"
        "    float4 v7 : TEXCOORD7;\n"
        "    float4 v8 : TEXCOORD8;\n"
        "    float4 v9 : TEXCOORD9;\n"
        "    float4 v10: TEXCOORD10;\n"
        "    float4 v11: TEXCOORD11;\n"
        "    float4 v12: TEXCOORD12;\n"
        "    float4 v13: TEXCOORD13;\n"
        "    float4 v14: TEXCOORD14;\n"
        "    float4 v15: TEXCOORD15;\n"
        "};\n"
        "struct PS_INPUT\n"
        "{\n"
        "    float4 pos : SV_Position;\n"
        "    float4 col : COLOR0;\n"
        "    float2 uv  : TEXCOORD0;\n"
        "};\n"
        "PS_INPUT VSMain(VS_INPUT input)\n"
        "{\n"
        "    PS_INPUT output;\n"
        "    float4 p = input.v0;\n"
        "    if (p.w == 0.0f) p.w = 1.0f;\n"
        "    if (abs(p.x) > 2.0f || abs(p.y) > 2.0f) {\n"
        "        float w = (viewport_scale.x > 1.0f) ? viewport_scale.x * 2.0f : 1920.0f;\n"
        "        float h = (viewport_scale.y != 0.0f) ? abs(viewport_scale.y) * 2.0f : 1080.0f;\n"
        "        if (p.x <= 640.0f && p.y <= 480.0f && w > 640.0f) {\n"
        "            w = 640.0f;\n"
        "            h = 480.0f;\n"
        "        }\n"
        "        output.pos = float4((p.x / w) * 2.0f - 1.0f, 1.0f - (p.y / h) * 2.0f, p.z, 1.0f);\n"
        "    } else {\n"
        "        output.pos = float4(p.x, -p.y, p.z, p.w);\n"
        "    }\n"
        "    if (input.v3.x > 0.0f || input.v3.y > 0.0f || input.v3.z > 0.0f || input.v3.w > 0.0f) {\n"
        "        output.col = input.v3;\n"
        "        if (output.col.a == 0.0f) output.col.a = 1.0f;\n"
        "    } else {\n"
        "        output.col = float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
        "    }\n"
        "    output.uv = input.v8.xy;\n"
        "    return output;\n"
        "}\n"
        "float4 PSMain(PS_INPUT input) : SV_Target\n"
        "{\n"
        "    return input.col;\n"
        "}\n";

    ComPtr<ID3DBlob> vs_blob, ps_blob, error_blob;
    hr = D3DCompile(s_fallback_hlsl, sizeof(s_fallback_hlsl) - 1, "fallback.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", 0, 0, vs_blob.GetAddressOf(), error_blob.GetAddressOf());
    if (SUCCEEDED(hr)) {
        g_dx11.vs_bytecode = vs_blob;
        g_dx11.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, g_dx11.fallback_vs.GetAddressOf());
    }
    hr = D3DCompile(s_fallback_hlsl, sizeof(s_fallback_hlsl) - 1, "fallback.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_0", 0, 0, ps_blob.GetAddressOf(), error_blob.GetAddressOf());
    if (SUCCEEDED(hr)) {
        g_dx11.device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, g_dx11.fallback_ps.GetAddressOf());
    }

    g_vb_shadow.assign(VB_SIZE, 0);
    g_ib_shadow.assign(IB_SIZE, 0);
    g_vb_dirty = false;
    g_ib_dirty = false;
    g_vb_dirty_max = 0;
    g_ib_dirty_max = 0;

    // Set initial viewport
    d3d8_dx11_set_viewport(0, 0, g_dx11.width, g_dx11.height, 0.0f, 1.0f);

    g_dx11.initialized = true;
    return true;
}

void d3d8_dx11_shutdown(void)
{
    g_dx11 = dx11_context{};
    g_vb_shadow.clear();
    g_ib_shadow.clear();
}

void d3d8_dx11_present(int interval)
{
    if (g_dx11.swap_chain) {
        g_dx11.swap_chain->Present(interval > 0 ? interval : 1, 0);
    }
}

void *d3d8_dx11_get_device(void)
{
    return g_dx11.device.Get();
}

void *d3d8_dx11_get_context(void)
{
    return g_dx11.context.Get();
}

void *d3d8_dx11_get_swap_chain(void)
{
    return g_dx11.swap_chain.Get();
}

void *d3d8_dx11_get_render_target_view(void)
{
    return g_dx11.back_buffer_rtv.Get();
}

void d3d8_dx11_set_viewport(DWORD x, DWORD y, DWORD width, DWORD height, float min_z, float max_z)
{
    if (!g_dx11.context) return;
    g_dx11.current_viewport.TopLeftX = static_cast<float>(x);
    g_dx11.current_viewport.TopLeftY = static_cast<float>(y);
    g_dx11.current_viewport.Width = static_cast<float>(width);
    g_dx11.current_viewport.Height = static_cast<float>(height);
    g_dx11.current_viewport.MinDepth = min_z;
    g_dx11.current_viewport.MaxDepth = max_z;
    g_dx11.context->RSSetViewports(1, &g_dx11.current_viewport);
}

void d3d8_dx11_set_clear_color(float r, float g, float b, float a)
{
    g_dx11.clear_color[0] = r;
    g_dx11.clear_color[1] = g;
    g_dx11.clear_color[2] = b;
    g_dx11.clear_color[3] = a;
}

void d3d8_dx11_clear(DWORD flags, DWORD color, float z, DWORD stencil)
{
    if (!g_dx11.context) return;

    if (flags & 0x00000001) { // D3DCLEAR_TARGET
        float clear_color[4];
        if (color != 0) {
            clear_color[0] = ((color >> 16) & 0xff) / 255.0f;
            clear_color[1] = ((color >> 8) & 0xff) / 255.0f;
            clear_color[2] = (color & 0xff) / 255.0f;
            clear_color[3] = ((color >> 24) & 0xff) / 255.0f;
        } else {
            memcpy(clear_color, g_dx11.clear_color, sizeof(clear_color));
        }
        if (g_dx11.back_buffer_rtv)
            g_dx11.context->ClearRenderTargetView(g_dx11.back_buffer_rtv.Get(), clear_color);
    }

    UINT ds_flags = 0;
    if (flags & 0x00000002) ds_flags |= D3D11_CLEAR_DEPTH;   // D3DCLEAR_ZBUFFER
    if (flags & 0x00000004) ds_flags |= D3D11_CLEAR_STENCIL; // D3DCLEAR_STENCIL

    if (ds_flags && g_dx11.depth_stencil_dsv) {
        g_dx11.context->ClearDepthStencilView(g_dx11.depth_stencil_dsv.Get(), ds_flags, z, static_cast<UINT8>(stencil));
    }
}

void d3d8_dx11_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
    if (!data || size == 0) return;
    if (target == 0x8892) { // GL_ARRAY_BUFFER
        if (offset + size <= VB_SIZE && !g_vb_shadow.empty()) {
            memcpy(g_vb_shadow.data() + offset, data, size);
            if (offset + size > g_vb_dirty_max) g_vb_dirty_max = offset + size;
            g_vb_dirty = true;
        }
    } else if (target == 0x8893) { // GL_ELEMENT_ARRAY_BUFFER
        if (offset + size <= IB_SIZE && !g_ib_shadow.empty()) {
            memcpy(g_ib_shadow.data() + offset, data, size);
            if (offset + size > g_ib_dirty_max) g_ib_dirty_max = offset + size;
            g_ib_dirty = true;
        }
    }
}

void d3d8_dx11_buffer_data(uint32_t target, uint32_t size, const void *data, uint32_t usage)
{
    (void)usage;
    if (data) {
        d3d8_dx11_buffer_write(target, 0, size, data);
    } else {
        if (target == 0x8892) {
            g_vb_dirty_max = 0;
            g_vb_dirty = true;
        } else if (target == 0x8893) {
            g_ib_dirty_max = 0;
            g_ib_dirty = true;
        }
    }
}

void d3d8_dx11_enable_vertex_attrib(uint32_t index, int enable)
{
    if (index < 16) {
        g_attribs[index].enabled = (enable != 0);
    }
}

void d3d8_dx11_vertex_attrib_pointer(uint32_t index, int size, uint32_t type, int normalized, int stride, uint32_t offset)
{
    if (index < 16) {
        if (stride == 0 && size > 0) {
            int elem_size = 4;
            if (type == 0x1401) elem_size = 1;
            else if (type == 0x1402) elem_size = 2;
            stride = size * elem_size;
        }
        g_attribs[index].size = size;
        g_attribs[index].type = type;
        g_attribs[index].normalized = (normalized != 0);
        g_attribs[index].stride = stride;
        g_attribs[index].offset = offset;
        g_attribs[index].enabled = true;
    }
}

void d3d8_dx11_draw_arrays(uint32_t mode, uint32_t first, uint32_t count)
{
    if (!g_dx11.context || count == 0) return;
    D3D_PRIMITIVE_TOPOLOGY topology = gl_mode_to_d3d11_topology(mode);
    d3d8_dx11_prepare_draw(topology);
    g_dx11.context->Draw(count, first);
}

void d3d8_dx11_draw_elements(uint32_t mode, uint32_t count, uint32_t type, uint32_t offset)
{
    d3d8_dx11_draw_elements_base_vertex(mode, count, type, offset, 0);
}

void d3d8_dx11_draw_elements_base_vertex(uint32_t mode, uint32_t count, uint32_t type, uint32_t offset, int32_t base_vertex)
{
    if (!g_dx11.context || count == 0) return;
    D3D_PRIMITIVE_TOPOLOGY topology = gl_mode_to_d3d11_topology(mode);
    d3d8_dx11_prepare_draw(topology);

    DXGI_FORMAT ib_format = (type == 0x1405 /* GL_UNSIGNED_INT */) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    g_dx11.context->IASetIndexBuffer(g_dx11.dynamic_ib.Get(), ib_format, 0);

    UINT index_size = (type == 0x1405 /* GL_UNSIGNED_INT */) ? 4 : 2;
    UINT start_index = offset / index_size;
    g_dx11.context->DrawIndexed(count, start_index, base_vertex);
}

void d3d8_dx11_draw_vertices(DWORD primitive_type, DWORD start_vertex, DWORD vertex_count)
{
    d3d8_dx11_draw_arrays(primitive_type, start_vertex, vertex_count);
}

void d3d8_dx11_draw_indexed_vertices(DWORD primitive_type, DWORD vertex_count, const uint16_t *indices)
{
    d3d8_dx11_draw_elements(primitive_type, vertex_count, 0x1403, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(indices)));
}
