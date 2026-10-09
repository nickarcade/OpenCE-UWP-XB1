/*
NV2A_HLSL.H

Translation of NV2A vertex programs and pixel shader register combiners into HLSL (Shader Model 5.0).
Direct3D 11 translation layer for OpenCE on Windows / Xbox One UWP.
*/

#ifndef __HALO_WINDOWS_NV2A_HLSL_H
#define __HALO_WINDOWS_NV2A_HLSL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DWORD
typedef uint32_t DWORD;
#endif

#ifndef BOOL
typedef int BOOL;
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define NV2A_HLSL_VERTEX_ATTRIBUTE_COUNT 16
#define NV2A_HLSL_VERTEX_CONSTANT_COUNT 192
#define NV2A_HLSL_VERTEX_CONSTANT_BIAS 96

struct nv2a_hlsl_vertex_lighting
{
	int lights; /* 1: distant + ambient, 2: point lights too */
	unsigned long normal_instruction, normal_register;
	unsigned long position_instruction, position_register;
};

BOOL nv2a_vertex_shader_lighting_hlsl(const DWORD *instructions, unsigned long instruction_count,
	struct nv2a_hlsl_vertex_lighting *lighting);

char *nv2a_vertex_shader_to_hlsl(const DWORD *instructions, unsigned long instruction_count,
	unsigned long packed_attribute_mask, const struct nv2a_hlsl_vertex_lighting *lighting);

/* Pixel shader combiners and stages */
#define D3DRS_PS_COMBINER_COUNT 8

struct nv2a_hlsl_pixel_shader_key
{
	DWORD combiner_state[57]; /* D3DRS_PS_MAX */
	DWORD texture_modes;      /* D3DRS_PSTEXTUREMODES */
	unsigned char sampler_type[4];
	unsigned char alpha_kill[4];
	unsigned char color_sign[4];
	unsigned long alpha_test_function;
	unsigned char fog_enable;
	unsigned char fog_table_mode;
	unsigned char count_samples;
	unsigned char coverage_alpha;
	unsigned char per_pixel_lighting;
	unsigned char alpha_test_samples;
};

char *nv2a_pixel_shader_to_hlsl(const struct nv2a_hlsl_pixel_shader_key *key);

#ifdef __cplusplus
}
#endif

#endif /* __HALO_WINDOWS_NV2A_HLSL_H */
