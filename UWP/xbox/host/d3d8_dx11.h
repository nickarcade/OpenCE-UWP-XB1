/*
D3D8_DX11.H

Direct3D 11 backend for Xbox Direct3D 8 (NV2A) on Windows and Xbox One UWP.
Replaces d3d8_gl.c and Mesa D3D12 with a native, zero-dependency Direct3D 11 renderer.
*/

#ifndef __HALO_WINDOWS_D3D8_DX11_H
#define __HALO_WINDOWS_D3D8_DX11_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#ifndef DWORD
typedef uint32_t DWORD;
#endif
#ifndef HRESULT
typedef long HRESULT;
#endif
#ifndef BOOL
typedef int BOOL;
#endif
#endif

/* Initialization and Host Lifecycle */
bool d3d8_dx11_initialize_uwp(void *core_window, int width, int height);
void d3d8_dx11_shutdown(void);
void d3d8_dx11_present(int interval);
void *d3d8_dx11_get_device(void);
void *d3d8_dx11_get_context(void);
void *d3d8_dx11_get_swap_chain(void);
void *d3d8_dx11_get_render_target_view(void);

/* Viewport, Render Targets and Clear */
void d3d8_dx11_set_viewport(DWORD x, DWORD y, DWORD width, DWORD height, float min_z, float max_z);
void d3d8_dx11_set_clear_color(float r, float g, float b, float a);
void d3d8_dx11_bind_framebuffer(uint32_t target, uint32_t fbo);
void d3d8_dx11_clear(DWORD flags, DWORD color, float z, DWORD stencil);

/* Vertex & Index Buffer Stream Management */
void d3d8_dx11_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data);
void d3d8_dx11_buffer_data(uint32_t target, uint32_t size, const void *data, uint32_t usage);
void d3d8_dx11_enable_vertex_attrib(uint32_t index, int enable);
void d3d8_dx11_vertex_attrib_pointer(uint32_t index, int size, uint32_t type, int normalized, int stride, uint32_t offset);
void d3d8_dx11_vertex_attrib_format(uint32_t attribindex, int size, uint32_t type, int normalized, uint32_t relativeoffset);
void d3d8_dx11_vertex_attrib_binding(uint32_t attribindex, uint32_t bindingindex);
void d3d8_dx11_bind_vertex_buffer(uint32_t bindingindex, uint32_t buffer, uint32_t offset, int stride);

/* Textures */
void d3d8_dx11_bind_texture(uint32_t target, uint32_t texture);
void d3d8_dx11_active_texture(uint32_t texture);
void d3d8_dx11_bind_textures(uint32_t first, int count, const uint32_t *textures);
void d3d8_dx11_tex_image_2d(uint32_t target, int level, int internalformat, int width, int height, int border, uint32_t format, uint32_t type, const void *pixels);
void d3d8_dx11_tex_sub_image_2d(uint32_t target, int level, int xoffset, int yoffset, int width, int height, uint32_t format, uint32_t type, const void *pixels);
void d3d8_dx11_delete_textures(int n, const uint32_t *textures);
void d3d8_dx11_tex_parameter_i(uint32_t target, uint32_t pname, int param);

/* State & Blending */
void d3d8_dx11_enable(uint32_t cap, int enable);
void d3d8_dx11_blend_func(uint32_t sfactor, uint32_t dfactor);
void d3d8_dx11_blend_func_separate(uint32_t srcRGB, uint32_t dstRGB, uint32_t srcAlpha, uint32_t dstAlpha);

/* Framebuffer & Blit */
void d3d8_dx11_framebuffer_texture_2d(uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int level);
void d3d8_dx11_blit_framebuffer(int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0, int dstX1, int dstY1, uint32_t mask, uint32_t filter);

/* Draw operations */
void d3d8_dx11_draw_vertices(DWORD primitive_type, DWORD start_vertex, DWORD vertex_count);
void d3d8_dx11_draw_indexed_vertices(DWORD primitive_type, DWORD vertex_count, const uint16_t *indices);
void d3d8_dx11_draw_arrays(uint32_t mode, uint32_t first, uint32_t count);
void d3d8_dx11_draw_elements(uint32_t mode, uint32_t count, uint32_t type, uint32_t offset);
void d3d8_dx11_draw_elements_base_vertex(uint32_t mode, uint32_t count, uint32_t type, uint32_t offset, int32_t base_vertex);

#ifdef __cplusplus
}
#endif

#endif /* __HALO_WINDOWS_D3D8_DX11_H */
