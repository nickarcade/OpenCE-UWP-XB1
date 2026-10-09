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

/* Viewport and Clear */
void d3d8_dx11_set_viewport(DWORD x, DWORD y, DWORD width, DWORD height, float min_z, float max_z);
void d3d8_dx11_set_clear_color(float r, float g, float b, float a);
void d3d8_dx11_clear(DWORD flags, DWORD color, float z, DWORD stencil);

/* Vertex & Index Buffer Stream Management */
void d3d8_dx11_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data);
void d3d8_dx11_buffer_data(uint32_t target, uint32_t size, const void *data, uint32_t usage);
void d3d8_dx11_enable_vertex_attrib(uint32_t index, int enable);
void d3d8_dx11_vertex_attrib_pointer(uint32_t index, int size, uint32_t type, int normalized, int stride, uint32_t offset);

/* Direct3D 8 / OpenGL draw calls */
void d3d8_dx11_draw_vertices(DWORD primitive_type, DWORD start_vertex, DWORD vertex_count);
void d3d8_dx11_draw_indexed_vertices(DWORD primitive_type, DWORD vertex_count, const uint16_t *indices);
void d3d8_dx11_draw_arrays(uint32_t mode, uint32_t first, uint32_t count);
void d3d8_dx11_draw_elements(uint32_t mode, uint32_t count, uint32_t type, uint32_t offset);
void d3d8_dx11_draw_elements_base_vertex(uint32_t mode, uint32_t count, uint32_t type, uint32_t offset, int32_t base_vertex);

#ifdef __cplusplus
}
#endif

#endif /* __HALO_WINDOWS_D3D8_DX11_H */
