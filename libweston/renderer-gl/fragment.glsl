/*
 * Copyright 2012 Intel Corporation
 * Copyright 2015,2019,2021 Collabora, Ltd.
 * Copyright 2016 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* GLSL version 1.00 ES, defined in gl-shaders.c */

/* For annotating shader compile-time constant arguments */
#define compile_const const

/*
 * Enumeration of shader variants, must match enum gl_shader_texture_variant.
 */
#define SHADER_VARIANT_RGBX     1
#define SHADER_VARIANT_RGBA     2
#define SHADER_VARIANT_Y_U_V    3
#define SHADER_VARIANT_Y_UV     4
#define SHADER_VARIANT_Y_XUXV   5
#define SHADER_VARIANT_XYUV     6
#define SHADER_VARIANT_SOLID    7
#define SHADER_VARIANT_EXTERNAL 8

/* enum gl_shader_color_curve */
#define SHADER_COLOR_CURVE_IDENTITY 0
#define SHADER_COLOR_CURVE_LUT_3x1D 1

/* enum gl_shader_degamma_variant */
#define SHADER_DEGAMMA_NONE 0
#define SHADER_DEGAMMA_SRGB 1
#define SHADER_DEGAMMA_PQ   2
#define SHADER_DEGAMMA_HLG  3

/* enum gl_shader_gamma_variant */
#define SHADER_GAMMA_NONE 0
#define SHADER_GAMMA_SRGB 1
#define SHADER_GAMMA_PQ   2
#define SHADER_GAMMA_HLG  3

/* enum gl_shader_gamma_variant */
#define SHADER_TONE_MAP_NONE       0
#define SHADER_TONE_MAP_HDR_TO_SDR 1
#define SHADER_TONE_MAP_SDR_TO_HDR 2
#define SHADER_TONE_MAP_HDR_TO_HDR 3

#if DEF_VARIANT == SHADER_VARIANT_EXTERNAL
#extension GL_OES_EGL_image_external : require
#endif

#ifdef GL_FRAGMENT_PRECISION_HIGH
#define HIGHPRECISION highp
#else
#define HIGHPRECISION mediump
#endif

precision HIGHPRECISION float;

/*
 * These undeclared identifiers will be #defined by a runtime generated code
 * snippet.
 */
compile_const int c_variant = DEF_VARIANT;
compile_const bool c_input_is_premult = DEF_INPUT_IS_PREMULT;
compile_const bool c_green_tint = DEF_GREEN_TINT;
compile_const int c_color_pre_curve = DEF_COLOR_PRE_CURVE;
compile_const int c_degamma = DEF_DEGAMMA;
compile_const int c_gamma = DEF_GAMMA;
compile_const bool c_csc_matrix = DEF_CSC_MATRIX;
compile_const int c_tone_mapping = DEF_TONE_MAP;
compile_const int c_nl_variant = DEF_GAMMA;

vec4
yuva2rgba(vec4 yuva)
{
	vec4 color_out;
	float Y, su, sv;

	/* ITU-R BT.601 & BT.709 quantization (limited range) */

	/* Y = 255/219 * (x - 16/256) */
	Y = 1.16438356 * (yuva.x - 0.0625);

	/* Remove offset 128/256, but the 255/224 multiplier comes later */
	su = yuva.y - 0.5;
	sv = yuva.z - 0.5;

	/*
	 * ITU-R BT.601 encoding coefficients (inverse), with the
	 * 255/224 limited range multiplier already included in the
	 * factors for su (Cb) and sv (Cr).
	 */
	color_out.r = Y                   + 1.59602678 * sv;
	color_out.g = Y - 0.39176229 * su - 0.81296764 * sv;
	color_out.b = Y + 2.01723214 * su;

	color_out.a = yuva.w;

	return color_out;
}

#if DEF_VARIANT == SHADER_VARIANT_EXTERNAL
uniform samplerExternalOES tex;
#else
uniform sampler2D tex;
#endif

varying vec2 v_texcoord;
uniform sampler2D tex1;
uniform sampler2D tex2;
uniform float alpha;
uniform vec4 unicolor;
uniform HIGHPRECISION sampler2D color_pre_curve_lut_2d;
uniform HIGHPRECISION vec2 color_pre_curve_lut_scale_offset;
uniform mat3 csc;

uniform float display_max_luminance;
uniform float content_max_luminance;
uniform float content_min_luminance;

/* EOTFS */
#if DEF_DEGAMMA == SHADER_DEGAMMA_SRGB
float eotf_srgb_single(float c) {
    return c < 0.04045 ? c / 12.92 : pow(((c + 0.055) / 1.055), 2.4);
}

vec3 eotf_srgb(vec3 color) {
    float r = eotf_srgb_single(color.r);
    float g = eotf_srgb_single(color.g);
    float b = eotf_srgb_single(color.b);
    return vec3(r, g, b);
}

vec3 eotf(vec3 color) {
    return sign(color) * eotf_srgb(abs(color.rgb));
}

vec3 ScaleLuminance(vec3 color) {
    return color * display_max_luminance;
}
#elif DEF_DEGAMMA == SHADER_DEGAMMA_PQ
vec3 eotf(vec3 v) {
    float m1 = 0.25 * 2610.0 / 4096.0;
    float m2 = 128.0 * 2523.0 / 4096.0;
    float c3 = 32.0 * 2392.0 / 4096.0;
    float c2 = 32.0 * 2413.0 / 4096.0;
    float c1 = c3 - c2 + 1.0;
    vec3 n = pow(v, vec3(1.0 / m2));
    return pow(max(n - c1, 0.0) / (c2 - c3 * n), vec3(1.0 / m1));
}

vec3 ScaleLuminance(vec3 color) {
    return color * 10000.0;
}
#elif DEF_DEGAMMA == SHADER_DEGAMMA_HLG
vec3 eotf(vec3 l) {
    float a = 0.17883277;
    float b = 1.0 - 4.0 * a;
    float c = 0.5 - a * log(4.0 * a);
    float x = step(1.0 / 2.0, l);
    vec3 v0 = pow(l, 2.0) / 3.0;
    vec3 v1 = (exp((l - c) / a) + b) / 12.0;
    return mix(v0, v1, x);
}

vec3 ScaleLuminance(vec3 color) {
    /* These are ITU 2100 recommendations */
    float kr = 0.2627;
    float kb = 0.0593;
    float kg = 1.0 - kr - kb;
    float luma = dot(color, vec3(kr, kg, kb));
    return color * 1000.0 * pow(luma, 0.2);
}
#else
vec3 eotf(vec3 color) {
    return color;
}

vec3 ScaleLuminance(vec3 color) {
    return color;
}
#endif

/* OETFS */
#if DEF_GAMMA == SHADER_GAMMA_SRGB
float oetf_srgb_single(float c) {
    float ret = 0.0;
    if (c < 0.0031308) {
        ret = 12.92 * c;
    } else {
        ret = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    }
    return ret;
}

vec3 oetf_srgb(vec3 color) {
    float r = oetf_srgb_single(color.r);
    float g = oetf_srgb_single(color.g);
    float b = oetf_srgb_single(color.b);
    return vec3(r, g, b);
}

vec3 oetf(vec3 linear) {
    return sign(linear) * oetf_srgb(abs(linear.rgb));
}

vec3 NormalizeLuminance(vec3 color) {
    return color / display_max_luminance;
}

#elif DEF_GAMMA == SHADER_GAMMA_PQ
vec3 oetf(vec3 l) {
    float m1 = 0.25 * 2610.0 / 4096.0;
    float m2 = 128.0 * 2523.0 / 4096.0;
    float c3 = 32.0 * 2392.0 / 4096.0;
    float c2 = 32.0 * 2413.0 / 4096.0;
    float c1 = c3 - c2 + 1.0;
    vec3 n = pow(l, vec3(m1));
    return pow((c1 + c2 * n) / (1.0 + c3 * n), vec3(m2));
}

vec3 NormalizeLuminance(vec3 color) {
    return color / 10000.0;
}

#elif DEF_GAMMA == SHADER_GAMMA_HLG
vec3 oetf(vec3 l) {
    float a = 0.17883277;
    float b = 1.0 - 4.0 * a;
    float c = 0.5 - a * log(4.0 * a);
    float x = step(1.0 / 12.0, l);
    vec3 v0 = a * log(12.0 * l - b) + c;
    vec3 v1 = sqrt(3.0 * l);
    return mix(v0, v1, x);
}

vec3 NormalizeLuminance(vec3 color) {
    /* These are ITU 2100 recommendations */
    float kr = 0.2627;
    float kb = 0.0593;
    float kg = 1.0 - kr - kb;
    float luma = dot(color, vec3(kr, kg, kb));
    return (color / 1000.0) * pow(luma, -0.2);
}

#else
vec3 oetf(vec3 color) {
    return color;
}

vec3 NormalizeLuminance(vec3 color) {
    return color;
}

#endif

#if DEF_TONE_MAP == SHADER_TONE_MAP_NONE
vec3 tone_mapping(vec3 color) {
    return color;
}

#elif DEF_TONE_MAP == SHADER_TONE_MAP_HDR_TO_SDR
vec3 hable_curve(vec3 c) {
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    vec3 numerator = (c * (A * c + C * B) + D * E);
    vec3 denominator = (c * (A * c + B) + D * F);
    c = (numerator / denominator) - E / F;
    return c;
}

vec3 tone_mapping(vec3 color) {
    float W = 11.2;
    float exposure = 100.0;
    color *= exposure;
    color = hable_curve(color);
    float white = hable_curve(vec3(W, 0, 0)).x;
    color /= white;
    return color;
}

#elif DEF_TONE_MAP == SHADER_TONE_MAP_SDR_TO_HDR
vec3 tone_mapping(vec3 color) {
    /* These are ITU 2100 recommendations */
    float kr = 0.2627;
    float kb = 0.0593;
    float kg = 1.0 - kr - kb;
    float luma = dot(color, vec3(kr, kg, kb));
    highp float tone_mapped_luma = 0.0;

    if (luma > 5.0) {
        tone_mapped_luma = luma / display_max_luminance;
        tone_mapped_luma = pow(tone_mapped_luma, 1.5);
        tone_mapped_luma *= display_max_luminance;
        color *= tone_mapped_luma / luma;
    }
    return color;
}

#elif DEF_TONE_MAP == SHADER_TONE_MAP_HDR_TO_HDR
vec3 tone_mapping(vec3 color) {
    float range = content_max_luminance - content_min_luminance;
    /* These are ITU 2100 recommendations */
    float kr = 0.2627;
    float kb = 0.0593;
    float kg = 1.0 - kr - kb;
    float luma = dot(color, vec3(kr, kg, kb));
    float tone_mapped_luma = luma - content_min_luminance;
    tone_mapped_luma /= range;
    tone_mapped_luma *= display_max_luminance;
    color *= tone_mapped_luma / luma;
    return color;
}
#endif

vec4
sample_input_texture()
{
	vec4 yuva = vec4(0.0, 0.0, 0.0, 1.0);

	/* Producing RGBA directly */

	if (c_variant == SHADER_VARIANT_SOLID)
		return unicolor;

	if (c_variant == SHADER_VARIANT_RGBA ||
	    c_variant == SHADER_VARIANT_EXTERNAL) {
		return texture2D(tex, v_texcoord);
	}

	if (c_variant == SHADER_VARIANT_RGBX)
		return vec4(texture2D(tex, v_texcoord).rgb, 1.0);

	/* Requires conversion to RGBA */

	if (c_variant == SHADER_VARIANT_Y_U_V) {
		yuva.x = texture2D(tex, v_texcoord).x;
		yuva.y = texture2D(tex1, v_texcoord).x;
		yuva.z = texture2D(tex2, v_texcoord).x;

	} else if (c_variant == SHADER_VARIANT_Y_UV) {
		yuva.x = texture2D(tex, v_texcoord).x;
		yuva.yz = texture2D(tex1, v_texcoord).rg;

	} else if (c_variant == SHADER_VARIANT_Y_XUXV) {
		yuva.x = texture2D(tex, v_texcoord).x;
		yuva.yz = texture2D(tex1, v_texcoord).ga;

	} else if (c_variant == SHADER_VARIANT_XYUV) {
		yuva.xyz = texture2D(tex, v_texcoord).bgr;

	} else {
		/* Never reached, bad variant value. */
		return vec4(1.0, 0.3, 1.0, 1.0);
	}

	return yuva2rgba(yuva);
}

/*
 * Texture coordinates go from 0.0 to 1.0 corresponding to texture edges.
 * When we do LUT look-ups with linear filtering, the correct range to sample
 * from is not from edge to edge, but center of first texel to center of last
 * texel. This follows because with LUTs, you have the exact end points given,
 * you never extrapolate but only interpolate.
 * The scale and offset are precomputed to achieve this mapping.
 */
float
lut_texcoord(float x, vec2 scale_offset)
{
	return x * scale_offset.s + scale_offset.t;
}

/*
 * Sample a 1D LUT which is a single row of a 2D texture. The 2D texture has
 * four rows so that the centers of texels have precise y-coordinates.
 */
float
sample_color_pre_curve_lut_2d(float x, compile_const int row)
{
	float tx = lut_texcoord(x, color_pre_curve_lut_scale_offset);

	return texture2D(color_pre_curve_lut_2d,
			 vec2(tx, (float(row) + 0.5) / 4.0)).x;
}

vec3
color_pre_curve(vec3 color)
{
	vec3 ret;

	if (c_color_pre_curve == SHADER_COLOR_CURVE_IDENTITY) {
		return color;
	} else if (c_color_pre_curve == SHADER_COLOR_CURVE_LUT_3x1D) {
		ret.r = sample_color_pre_curve_lut_2d(color.r, 0);
		ret.g = sample_color_pre_curve_lut_2d(color.g, 1);
		ret.b = sample_color_pre_curve_lut_2d(color.b, 2);
		return ret;
	} else {
		/* Never reached, bad c_color_pre_curve. */
		return vec3(1.0, 0.3, 1.0);
	}
}

vec4
color_pipeline(vec4 color)
{
	/* View alpha (opacity) */
	color.a *= alpha;

	color.rgb = color_pre_curve(color.rgb);

	return color;
}

void
main()
{
	vec4 color;

	/* Electrical (non-linear) RGBA values, may be premult or not */
	color = sample_input_texture();

	/* Ensure straight alpha */
	if (c_input_is_premult) {
		if (color.a == 0.0)
			color.rgb = vec3(0, 0, 0);
		else
			color.rgb *= 1.0 / color.a;
	}

	color = color_pipeline(color);

	/* pre-multiply for blending */
	color.rgb *= color.a;

	if (c_green_tint)
		color = vec4(0.0, 0.3, 0.0, 0.2) + color * 0.8;

	gl_FragColor = color;

	if (c_csc_matrix ||
	    (c_tone_mapping != 0) && (c_degamma != 0))
	/* eotf_shader */
		gl_FragColor.rgb = eotf(gl_FragColor.rgb);

	if (c_csc_matrix)
	/* csc_shader */
		gl_FragColor.rgb = clamp((csc * gl_FragColor.rgb), 0.0, 1.0);

	if ((c_degamma != 0) &&
	    (c_tone_mapping == SHADER_TONE_MAP_HDR_TO_HDR) ||
	    (c_tone_mapping == SHADER_TONE_MAP_SDR_TO_HDR))
	/* sl_shader */
		gl_FragColor.rgb = ScaleLuminance(gl_FragColor.rgb);

	if (c_tone_mapping != 0)
	/* hdr_shader */
		gl_FragColor.rgb = tone_mapping(gl_FragColor.rgb);

	if (c_csc_matrix ||
	    (c_tone_mapping != 0) &&
	    (c_nl_variant != 0))
	/* nl_shader */
		gl_FragColor.rgb = NormalizeLuminance(gl_FragColor.rgb);

	if (c_csc_matrix ||
	    (c_tone_mapping != 0) &&
	    (c_gamma != 0))
	/* oetf_shader */
		gl_FragColor.rgb = oetf(gl_FragColor.rgb);
}
