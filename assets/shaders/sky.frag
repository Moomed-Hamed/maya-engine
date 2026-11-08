#version 420 core

// sky disks : a linear additive pass over the deferred frame, before water. the
// opaque depth mask keeps them off terrain; forward water then covers them

struct VS_OUT {
    vec2 uv;
    vec3 view_position;
};

in VS_OUT vs_out;

layout (binding = 0) uniform sampler2D scene_depth;

layout (location = 0) out vec4 pixel_color;

uniform vec2 sun_screen;
uniform vec2 moon_screen;
uniform vec3 sun_color;
uniform vec3 moon_color;
uniform float sun_strength;
uniform float moon_strength;
uniform float aspect;

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

// the sun is a bright core with limb darkening, a tight aureole and a wide
// atmospheric halo. the core is HDR so bloom picks it up
vec3 sun_disk(vec2 uv, vec2 center, vec3 color, float strength)
{
    vec2 d = (uv - center) * vec2(aspect, 1.0);
    float r = length(d);
    float core = smoothstep(0.0115, 0.0092, r);
    float limb = 1.0 - 0.35 * smoothstep(0.0, 0.0115, r);
    float aureole = pow(max(0.0, 1.0 - r / 0.060), 4.0);
    float halo = pow(max(0.0, 1.0 - r / 0.240), 2.0);
    return color * (core * limb * 36.0 + aureole * 1.4 + halo * 0.10) * strength;
}

// a cooler, slightly mottled moon with a soft halo instead of a flat white dot
vec3 moon_disk(vec2 uv, vec2 center, vec3 color, float strength)
{
    vec2 d = (uv - center) * vec2(aspect, 1.0);
    float r = length(d);
    float core = smoothstep(0.0080, 0.0062, r);
    float limb = 1.0 - 0.30 * smoothstep(0.0, 0.0080, r);
    float maria = 0.80 + 0.20 * value_noise(d * 1100.0 + vec2(3.0, 7.0));
    float halo = pow(max(0.0, 1.0 - r / 0.11), 3.0);
    return color * (core * limb * maria * 26.0 + halo * 0.55) * strength;
}

void main()
{
    if (texture(scene_depth, vs_out.uv).r < 0.99999)
        discard;

    vec3 col = sun_disk(vs_out.uv, sun_screen, sun_color, sun_strength);
    col += moon_disk(vs_out.uv, moon_screen, moon_color, moon_strength);
    pixel_color = vec4(col, 1.0);
}
