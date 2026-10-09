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

using Microsoft::WRL::ComPtr;

namespace {

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

    D3D11_VIEWPORT current_viewport{};
    int width = 1920;
    int height = 1080;
    bool initialized = false;
};

static dx11_context g_dx11;

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
    cb_desc.ByteWidth = (sizeof(float) * 4 * 192 + 64 + 15) & ~15; // 192 constants + viewport
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dx11.device->CreateBuffer(&cb_desc, nullptr, g_dx11.vs_constants.GetAddressOf());

    cb_desc.ByteWidth = 512; // Pixel shader uniforms
    g_dx11.device->CreateBuffer(&cb_desc, nullptr, g_dx11.ps_constants.GetAddressOf());

    // Dynamic VB / IB
    D3D11_BUFFER_DESC vb_desc{};
    vb_desc.ByteWidth = 16 * 1024 * 1024; // 16 MB ring buffer
    vb_desc.Usage = D3D11_USAGE_DYNAMIC;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dx11.device->CreateBuffer(&vb_desc, nullptr, g_dx11.dynamic_vb.GetAddressOf());

    D3D11_BUFFER_DESC ib_desc{};
    ib_desc.ByteWidth = 2 * 1024 * 1024; // 2 MB index buffer
    ib_desc.Usage = D3D11_USAGE_DYNAMIC;
    ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ib_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dx11.device->CreateBuffer(&ib_desc, nullptr, g_dx11.dynamic_ib.GetAddressOf());

    // Default Rasterizer State
    D3D11_RASTERIZER_DESC rast_desc{};
    rast_desc.FillMode = D3D11_FILL_SOLID;
    rast_desc.CullMode = D3D11_CULL_NONE;
    rast_desc.DepthClipEnable = TRUE;
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

    // Default Depth Stencil State
    D3D11_DEPTH_STENCIL_DESC ds_state_desc{};
    ds_state_desc.DepthEnable = TRUE;
    ds_state_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds_state_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
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

    // Set initial viewport
    d3d8_dx11_set_viewport(0, 0, g_dx11.width, g_dx11.height, 0.0f, 1.0f);

    g_dx11.initialized = true;
    return true;
}

void d3d8_dx11_shutdown(void)
{
    g_dx11 = dx11_context{};
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

void d3d8_dx11_clear(DWORD flags, DWORD color, float z, DWORD stencil)
{
    if (!g_dx11.context) return;

    if (flags & 0x00000001) { // D3DCLEAR_TARGET
        float clear_color[4] = {
            ((color >> 16) & 0xff) / 255.0f,
            ((color >> 8) & 0xff) / 255.0f,
            (color & 0xff) / 255.0f,
            ((color >> 24) & 0xff) / 255.0f
        };
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

void d3d8_dx11_draw_vertices(DWORD primitive_type, DWORD start_vertex, DWORD vertex_count)
{
    if (!g_dx11.context) return;

    D3D_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    switch (primitive_type) {
    case 1: topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;     // D3DPT_POINTLIST
    case 2: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;      // D3DPT_LINELIST
    case 3: topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;     // D3DPT_LINESTRIP
    case 4: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;  // D3DPT_TRIANGLELIST
    case 5: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break; // D3DPT_TRIANGLESTRIP
    case 6: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break; // D3DPT_TRIANGLEFAN (approximate)
    default: break;
    }

    g_dx11.context->IASetPrimitiveTopology(topology);
    g_dx11.context->Draw(vertex_count, start_vertex);
}

void d3d8_dx11_draw_indexed_vertices(DWORD primitive_type, DWORD vertex_count, const uint16_t *indices)
{
    if (!g_dx11.context || !indices) return;

    D3D_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    switch (primitive_type) {
    case 4: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case 5: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    default: break;
    }

    g_dx11.context->IASetPrimitiveTopology(topology);
    g_dx11.context->DrawIndexed(vertex_count, 0, 0);
}
