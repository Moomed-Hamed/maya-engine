#version 420 core

// the last pass : exposure, ACES and the display encode. everything before this
// point is linear HDR, so a local light and the sun add on the same scale

struct VS_OUT {
    vec2 uv;
    vec3 view_position;
};

in VS_OUT vs_out;

layout (binding = 0) uniform sampler2D lit_color;
layout (binding = 1) uniform sampler2D bloom;
layout (binding = 2) uniform sampler2D scene_depth;
layout (binding = 3) uniform sampler2D cloud_mask;

layout (location = 0) out vec4 pixel_color;

uniform float exposure;
uniform float contrast;
uniform float saturation;
uniform float bloom_strength;
uniform vec2  sun_screen;
uniform float sun_on;
uniform vec3  sun_color;
uniform float godray_strength;
uniform float godray_on;
uniform int   godray_steps;
uniform float weather;   // 0 clear .. 1 storm
uniform float rain_time;

vec3 aces(vec3 x) // Narkowicz fit
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

float hash21(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float value_noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// interleaved gradient noise, +/- half an 8-bit step after encode : kills banding
float screen_dither()
{
    // white-noise TPDF dither, one 8-bit step : the interleaved pattern could
    // read as diagonal lines on a large flat floor
    float a = hash21(gl_FragCoord.xy + 0.5);
    float b = hash21(gl_FragCoord.xy + 17.3);
    return (a - b) / 255.0;
}

// screen-space god rays : march from this pixel toward the sun and accumulate
// sky radiance. terrain depth blocks the shaft, so trees and hills carve it
vec3 godrays(vec2 uv)
{
    if (sun_on < 0.5 || godray_on < 0.5) return vec3(0.0);
    int steps = max(godray_steps, 1);
    vec2 delta = (sun_screen - uv) / float(steps);
    vec2 p = uv;
    float illum = 0.0;
    float weight = 1.0;
    for (int i = 0; i < 24; i++)
    {
        if (i >= steps) break;
        p += delta;
        if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) break;
        if (texture(scene_depth, p).r > 0.9999)
        {
            float l = dot(texture(lit_color, p).rgb, vec3(0.2126, 0.7152, 0.0722));
            float clear = 1.0 - clamp(texture(cloud_mask, p).a, 0.0, 1.0);
            illum += min(l, 2.0) * weight * clear;
        }
        weight *= 0.965;
    }
    return sun_color * (illum / float(steps)) * godray_strength;
}

void main()
{
    vec3 color = texture(lit_color, vs_out.uv).rgb;
    color += texture(bloom, vs_out.uv).rgb * bloom_strength;
    color += godrays(vs_out.uv);

    // storm veil : soft vertical streaks when the cloud deck is heavy
    if (weather > 0.01)
    {
        vec2 rp = vec2((vs_out.uv.x + vs_out.uv.y * 0.12) * 300.0,
                       vs_out.uv.y * 4.0 - rain_time * 30.0);
        float n = value_noise(rp) * 0.65
                + value_noise(vec2(rp.x * 1.5, rp.y * 0.70) + 7.0) * 0.35;
        float streak = smoothstep(0.78, 0.92, n);
        color += vec3(0.55, 0.65, 0.78) * streak * weather * 0.05;
    }

    color = aces(exposure * color);

    // a small grade : ACES rolls colour off in the highlights, and the tropical
    // presets read better with the turquoise kept alive
    color = (color - 0.5) * contrast + 0.5;
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, saturation);

    // gentle lens falloff : keeps the eye on the centre without a heavy vignette
    vec2 q = vs_out.uv - 0.5;
    color *= 1.0 - dot(q, q) * 0.16;

    color = clamp(color, 0.0, 1.0);
    color = pow(color, vec3(1.0 / 2.2));
    pixel_color = vec4(color + screen_dither(), 1.0);
}
