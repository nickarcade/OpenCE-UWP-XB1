/*
NV2A_HLSL.C

Translation of NV2A vertex programs (Xbox vertex shader microcode) and pixel shader
register combiners into HLSL (Shader Model 5.0) for Direct3D 11.
*/

#include "nv2a_hlsl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

struct hlsl_text
{
	char *buffer;
	unsigned long length;
	unsigned long capacity;
};

static void hlsl_text_append(struct hlsl_text *text, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	int needed = vsnprintf(NULL, 0, format, args);
	va_end(args);

	if (needed < 0) return;

	if (text->length + needed + 1 > text->capacity)
	{
		unsigned long new_capacity = (text->capacity == 0) ? 4096 : text->capacity * 2;
		while (new_capacity < text->length + needed + 1)
			new_capacity *= 2;
		char *new_buf = (char *)realloc(text->buffer, new_capacity);
		if (!new_buf) return;
		text->buffer = new_buf;
		text->capacity = new_capacity;
	}

	va_start(args, format);
	vsnprintf(text->buffer + text->length, needed + 1, format, args);
	va_end(args);
	text->length += needed;
}

/* ---------- vertex shader instruction fields */

static unsigned long field(const DWORD *instruction, int word, int low_bit, int bit_count)
{
	return (instruction[word] >> low_bit) & ((1UL << bit_count) - 1);
}

enum
{
	_mac_nop, _mac_mov, _mac_mul, _mac_add, _mac_mad, _mac_dp3, _mac_dph, _mac_dp4,
	_mac_dst, _mac_min, _mac_max, _mac_slt, _mac_sge, _mac_arl,
};

enum
{
	_ilu_nop, _ilu_mov, _ilu_rcp, _ilu_rcc, _ilu_rsq, _ilu_exp, _ilu_log, _ilu_lit,
};

enum
{
	_mux_unknown, _mux_temporary, _mux_input, _mux_constant,
};

static const char *output_name(unsigned long address)
{
	switch (address)
	{
	case 0: return "oPos";
	case 3: return "oD0";
	case 4: return "oD1";
	case 5: return "oFog";
	case 6: return "oPts";
	case 7: return "oB0";
	case 8: return "oB1";
	case 9: return "oT0";
	case 10: return "oT1";
	case 11: return "oT2";
	case 12: return "oT3";
	default: return "oUnused";
	}
}

static void swizzle_to_mask(unsigned long swizzle, char *out)
{
	static const char comps[] = "xyzw";
	out[0] = comps[(swizzle >> 6) & 3];
	out[1] = comps[(swizzle >> 4) & 3];
	out[2] = comps[(swizzle >> 2) & 3];
	out[3] = comps[swizzle & 3];
	out[4] = '\0';
}

static void write_mask(unsigned long mask_val, char *out)
{
	int p = 0;
	if (mask_val & 8) out[p++] = 'x';
	if (mask_val & 4) out[p++] = 'y';
	if (mask_val & 2) out[p++] = 'z';
	if (mask_val & 1) out[p++] = 'w';
	out[p] = '\0';
}

static const char vs_hlsl_prologue[] =
	"cbuffer VertexConstants : register(b0)\n"
	"{\n"
	"    float4 c[192];\n"
	"    float4 viewport_scale;\n"
	"    float4 viewport_offset;\n"
	"    float point_size;\n"
	"    float screen_offset;\n"
	"    float2 _pad_vs;\n"
	"};\n"
	"\n"
	"struct VS_INPUT\n"
	"{\n"
	"    float4 v[16] : TEXCOORD0;\n"
	"};\n"
	"\n"
	"struct VS_OUTPUT\n"
	"{\n"
	"    float4 oPos : SV_Position;\n"
	"    float4 oD0 : COLOR0;\n"
	"    float4 oD1 : COLOR1;\n"
	"    float4 oB0 : COLOR2;\n"
	"    float4 oB1 : COLOR3;\n"
	"    float4 oT0 : TEXCOORD0;\n"
	"    float4 oT1 : TEXCOORD1;\n"
	"    float4 oT2 : TEXCOORD2;\n"
	"    float4 oT3 : TEXCOORD3;\n"
	"    float oFog : FOG;\n"
	"    float4 xWorldNormal : NORMAL;\n"
	"    float3 xWorldPosition : POSITION1;\n"
	"};\n"
	"\n"
	"float4 unpack_normpacked3(uint p)\n"
	"{\n"
	"    int x = int(p << 21) >> 21;\n"
	"    int y = int(p << 10) >> 21;\n"
	"    int z = int(p) >> 22;\n"
	"    return float4(float(x) / 1023.0f, float(y) / 1023.0f, float(z) / 511.0f, 1.0f);\n"
	"}\n"
	"\n"
	"float4 nv2a_rcc(float x)\n"
	"{\n"
	"    float r = 1.0f / x;\n"
	"    if (r > 0.0f) r = clamp(r, 5.42101e-20f, 1.884467e+19f);\n"
	"    else r = clamp(r, -1.884467e+19f, -5.42101e-20f);\n"
	"    return float4(r, r, r, r);\n"
	"}\n"
	"\n"
	"float4 nv2a_exp(float x)\n"
	"{\n"
	"    return float4(exp2(floor(x)), frac(x), exp2(x), 1.0f);\n"
	"}\n"
	"\n"
	"float4 nv2a_log(float x)\n"
	"{\n"
	"    x = abs(x);\n"
	"    if (x == 0.0f) return float4(-1.0e30f, 1.0f, -1.0e30f, 1.0f);\n"
	"    float e = floor(log2(x));\n"
	"    return float4(e, x / exp2(e), log2(x), 1.0f);\n"
	"}\n"
	"\n"
	"float4 nv2a_lit(float4 s)\n"
	"{\n"
	"    float specular = s.x > 0.0f ? pow(max(s.y, 0.0f), clamp(s.w, -127.9961f, 127.9961f)) : 0.0f;\n"
	"    return float4(1.0f, max(s.x, 0.0f), specular, 1.0f);\n"
	"}\n";

char *nv2a_vertex_shader_to_hlsl(const DWORD *instructions, unsigned long instruction_count,
	unsigned long packed_attribute_mask, const struct nv2a_hlsl_vertex_lighting *lighting)
{
	struct hlsl_text text = { 0 };
	unsigned long index;

	hlsl_text_append(&text, "%s\n", vs_hlsl_prologue);
	hlsl_text_append(&text, "VS_OUTPUT main(VS_INPUT input)\n{\n");
	hlsl_text_append(&text, "    VS_OUTPUT output = (VS_OUTPUT)0;\n");

	for (index = 0; index < NV2A_HLSL_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (packed_attribute_mask & (1UL << index))
			hlsl_text_append(&text, "    float4 v%lu = unpack_normpacked3(asuint(input.v[%lu].x));\n", index, index);
		else
			hlsl_text_append(&text, "    float4 v%lu = input.v[%lu];\n", index, index);
	}

	hlsl_text_append(&text,
		"    float4 r0 = float4(0,0,0,0), r1 = float4(0,0,0,0), r2 = float4(0,0,0,0), r3 = float4(0,0,0,0);\n"
		"    float4 r4 = float4(0,0,0,0), r5 = float4(0,0,0,0), r6 = float4(0,0,0,0), r7 = float4(0,0,0,0);\n"
		"    float4 r8 = float4(0,0,0,0), r9 = float4(0,0,0,0), r10 = float4(0,0,0,0), r11 = float4(0,0,0,0);\n"
		"    float4 oPos = float4(0,0,0,1);\n"
		"    float4 oD0 = float4(0,0,0,1), oD1 = float4(0,0,0,1);\n"
		"    float4 oB0 = float4(0,0,0,1), oB1 = float4(0,0,0,1);\n"
		"    float4 oT0 = float4(0,0,0,1), oT1 = float4(0,0,0,1);\n"
		"    float4 oT2 = float4(0,0,0,1), oT3 = float4(0,0,0,1);\n"
		"    float4 oFog = float4(1,1,1,1), oPts = float4(point_size,point_size,point_size,point_size), oUnused = float4(0,0,0,0);\n"
		"    int a0 = 0;\n"
		"    float4 A = float4(0,0,0,0), B = float4(0,0,0,0), C = float4(0,0,0,0), mac = float4(0,0,0,0), ilu = float4(0,0,0,0);\n"
		"    float4 clip_position = float4(0,0,0,0);\n"
		"    bool clip_captured = false;\n\n");

	for (index = 0; index < instruction_count; index++)
	{
		const DWORD *inst = instructions + index * 4;
		unsigned long mac = field(inst, 1, 21, 4);
		unsigned long ilu = field(inst, 1, 25, 3);
		unsigned long mac_mask = field(inst, 3, 24, 4);
		unsigned long temp_reg = field(inst, 3, 20, 4);
		unsigned long ilu_mask = field(inst, 3, 16, 4);
		unsigned long out_mask = field(inst, 3, 12, 4);
		unsigned long out_is_reg = field(inst, 3, 11, 1);
		unsigned long out_addr = field(inst, 3, 3, 8);
		unsigned long out_from_ilu = field(inst, 3, 2, 1);
		int relative = (int)field(inst, 3, 1, 1);
		char mask_str[5];

		hlsl_text_append(&text, "    // Instruction %lu\n", index);

		if (lighting && index == lighting->normal_instruction)
			hlsl_text_append(&text, "    output.xWorldNormal = float4(r%lu.xyz, length(r%lu.xyz));\n",
				lighting->normal_register, lighting->normal_register);
		if (lighting && lighting->lights == 2 && index == lighting->position_instruction)
			hlsl_text_append(&text, "    output.xWorldPosition = r%lu.xyz;\n", lighting->position_register);

		/* Decode Operands A, B, C */
		int op;
		for (op = 0; op < 3; op++)
		{
			unsigned long mux = field(inst, 0, (2 - op) * 2, 2);
			unsigned long reg_idx = field(inst, 1, (2 - op) * 6, 6);
			unsigned long swiz = field(inst, 2, (2 - op) * 8 + 8, 8);
			unsigned long neg = field(inst, 1, (2 - op) + 18, 1);
			char swiz_str[5];
			swizzle_to_mask(swiz, swiz_str);

			const char op_char = (char)('A' + op);
			hlsl_text_append(&text, "    %c = %s", op_char, neg ? "-" : "");

			switch (mux)
			{
			case _mux_temporary:
				hlsl_text_append(&text, "r%lu.%s;\n", reg_idx, swiz_str);
				break;
			case _mux_input:
				hlsl_text_append(&text, "v%lu.%s;\n", reg_idx, swiz_str);
				break;
			case _mux_constant:
				if (relative)
					hlsl_text_append(&text, "c[clamp(int(%lu) + a0, 0, 191)].%s;\n", reg_idx, swiz_str);
				else
					hlsl_text_append(&text, "c[%lu].%s;\n", reg_idx, swiz_str);
				break;
			default:
				hlsl_text_append(&text, "float4(0,0,0,0);\n");
				break;
			}
		}

		/* Execute MAC */
		switch (mac)
		{
		case _mac_mov: hlsl_text_append(&text, "    mac = A;\n"); break;
		case _mac_mul: hlsl_text_append(&text, "    mac = A * B;\n"); break;
		case _mac_add: hlsl_text_append(&text, "    mac = A + C;\n"); break;
		case _mac_mad: hlsl_text_append(&text, "    mac = A * B + C;\n"); break;
		case _mac_dp3: hlsl_text_append(&text, "    mac = float4(dot(A.xyz, B.xyz).xxxx);\n"); break;
		case _mac_dph: hlsl_text_append(&text, "    mac = float4(dot(float4(A.xyz, 1.0f), B).xxxx);\n"); break;
		case _mac_dp4: hlsl_text_append(&text, "    mac = float4(dot(A, B).xxxx);\n"); break;
		case _mac_dst: hlsl_text_append(&text, "    mac = float4(1.0f, A.y * B.y, A.z, B.w);\n"); break;
		case _mac_min: hlsl_text_append(&text, "    mac = min(A, B);\n"); break;
		case _mac_max: hlsl_text_append(&text, "    mac = max(A, B);\n"); break;
		case _mac_slt: hlsl_text_append(&text, "    mac = float4(A.x < B.x, A.y < B.y, A.z < B.z, A.w < B.w);\n"); break;
		case _mac_sge: hlsl_text_append(&text, "    mac = float4(A.x >= B.x, A.y >= B.y, A.z >= B.z, A.w >= B.w);\n"); break;
		case _mac_arl: hlsl_text_append(&text, "    a0 = int(floor(A.x + 0.5f));\n"); break;
		default: break;
		}

		/* Execute ILU */
		switch (ilu)
		{
		case _ilu_mov: hlsl_text_append(&text, "    ilu = A;\n"); break;
		case _ilu_rcp: hlsl_text_append(&text, "    ilu = float4((1.0f / A.x).xxxx);\n"); break;
		case _ilu_rcc: hlsl_text_append(&text, "    ilu = nv2a_rcc(A.x);\n"); break;
		case _ilu_rsq: hlsl_text_append(&text, "    ilu = float4(rsqrt(max(abs(A.x), 1e-20f)).xxxx);\n"); break;
		case _ilu_exp: hlsl_text_append(&text, "    ilu = nv2a_exp(A.x);\n"); break;
		case _ilu_log: hlsl_text_append(&text, "    ilu = nv2a_log(A.x);\n"); break;
		case _ilu_lit: hlsl_text_append(&text, "    ilu = nv2a_lit(A);\n"); break;
		default: break;
		}

		/* Write Temporary Registers */
		if (mac != _mac_nop && mac_mask != 0)
		{
			write_mask(mac_mask, mask_str);
			hlsl_text_append(&text, "    r%lu.%s = mac.%s;\n", temp_reg, mask_str, mask_str);
		}
		if (ilu != _ilu_nop && ilu_mask != 0)
		{
			write_mask(ilu_mask, mask_str);
			hlsl_text_append(&text, "    r1.%s = ilu.%s;\n", mask_str, mask_str);
		}

		/* Write Outputs */
		if (out_mask != 0)
		{
			write_mask(out_mask, mask_str);
			const char *src = out_from_ilu ? "ilu" : "mac";
			if (out_is_reg)
			{
				hlsl_text_append(&text, "    r%lu.%s = %s.%s;\n", out_addr, mask_str, src, mask_str);
			}
			else
			{
				const char *dst = output_name(out_addr);
				hlsl_text_append(&text, "    %s.%s = %s.%s;\n", dst, mask_str, src, mask_str);
				if (out_addr == 0 && (out_mask & 8)) /* oPos.x written */
				{
					hlsl_text_append(&text, "    clip_position.%s = %s.%s;\n", mask_str, src, mask_str);
					hlsl_text_append(&text, "    clip_captured = true;\n");
				}
			}
		}
	}

	hlsl_text_append(&text,
		"\n"
		"    // Invert viewport transformation for Direct3D 11 clip space\n"
		"    if (clip_captured)\n"
		"        output.oPos = float4((oPos.xy - viewport_offset.xy) / viewport_scale.xy, (oPos.z - viewport_offset.z) / viewport_scale.z, oPos.w);\n"
		"    else\n"
		"        output.oPos = oPos;\n"
		"    output.oPos.x += screen_offset;\n"
		"    output.oD0 = oD0;\n"
		"    output.oD1 = oD1;\n"
		"    output.oB0 = oB0;\n"
		"    output.oB1 = oB1;\n"
		"    output.oT0 = oT0;\n"
		"    output.oT1 = oT1;\n"
		"    output.oT2 = oT2;\n"
		"    output.oT3 = oT3;\n"
		"    output.oFog = oFog.x;\n"
		"    return output;\n"
		"}\n");

	return text.buffer;
}

/* ---------- pixel shader translation ---------- */

static const char ps_hlsl_prologue[] =
	"cbuffer PixelConstants : register(b0)\n"
	"{\n"
	"    float4 ps_c0[8];\n"
	"    float4 ps_c1[8];\n"
	"    float4 ps_final_c0;\n"
	"    float4 ps_final_c1;\n"
	"    float4 fog_color;\n"
	"    float4 fog_parameters;\n"
	"    float alpha_reference;\n"
	"    float3 _pad_ps;\n"
	"    float4 bump_matrix[4];\n"
	"    float4 bump_luminance[4];\n"
	"    float4 texture_scale[4];\n"
	"};\n"
	"\n"
	"struct PS_INPUT\n"
	"{\n"
	"    float4 oPos : SV_Position;\n"
	"    float4 oD0 : COLOR0;\n"
	"    float4 oD1 : COLOR1;\n"
	"    float4 oB0 : COLOR2;\n"
	"    float4 oB1 : COLOR3;\n"
	"    float4 oT0 : TEXCOORD0;\n"
	"    float4 oT1 : TEXCOORD1;\n"
	"    float4 oT2 : TEXCOORD2;\n"
	"    float4 oT3 : TEXCOORD3;\n"
	"    float oFog : FOG;\n"
	"    float4 xWorldNormal : NORMAL;\n"
	"    float3 xWorldPosition : POSITION1;\n"
	"};\n"
	"\n"
	"Texture2D tex0 : register(t0); SamplerState samp0 : register(s0);\n"
	"Texture2D tex1 : register(t1); SamplerState samp1 : register(s1);\n"
	"Texture2D tex2 : register(t2); SamplerState samp2 : register(s2);\n"
	"Texture2D tex3 : register(t3); SamplerState samp3 : register(s3);\n";

char *nv2a_pixel_shader_to_hlsl(const struct nv2a_hlsl_pixel_shader_key *key)
{
	struct hlsl_text text = { 0 };

	hlsl_text_append(&text, "%s\n", ps_hlsl_prologue);
	hlsl_text_append(&text, "float4 main(PS_INPUT input) : SV_Target\n{\n");
	hlsl_text_append(&text, "    float4 v0 = input.oD0;\n");
	hlsl_text_append(&text, "    float4 v1 = input.oD1;\n");
	hlsl_text_append(&text, "    float4 t0 = tex0.Sample(samp0, input.oT0.xy * texture_scale[0].xy);\n");
	hlsl_text_append(&text, "    float4 t1 = tex1.Sample(samp1, input.oT1.xy * texture_scale[1].xy);\n");
	hlsl_text_append(&text, "    float4 t2 = tex2.Sample(samp2, input.oT2.xy * texture_scale[2].xy);\n");
	hlsl_text_append(&text, "    float4 t3 = tex3.Sample(samp3, input.oT3.xy * texture_scale[3].xy);\n");
	hlsl_text_append(&text, "    float4 r0 = t0;\n");
	hlsl_text_append(&text, "    float4 r1 = t1;\n");

	/* Final combiner standard formula: rgb = A*B + (1-A)*C + D, alpha = G */
	hlsl_text_append(&text,
		"\n"
		"    // Final combiner stage\n"
		"    float4 color;\n"
		"    color.rgb = r0.rgb * v0.rgb + (1.0f - r0.rgb) * ps_final_c0.rgb + v1.rgb;\n"
		"    color.a = r0.a * v0.a;\n");

	/* Alpha Test */
	if (key && key->alpha_test_function != 0)
	{
		hlsl_text_append(&text,
			"    if (color.a < alpha_reference)\n"
			"        discard;\n");
	}

	/* Fog */
	if (key && key->fog_enable)
	{
		hlsl_text_append(&text,
			"    color.rgb = lerp(fog_color.rgb, color.rgb, saturate(input.oFog));\n");
	}

	hlsl_text_append(&text, "    return color;\n}\n");

	return text.buffer;
}
