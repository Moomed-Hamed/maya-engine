#version 420 core

// separable Gaussian bloom blur. one pass per axis, ping-ponged through the
// half-resolution targets so the final add is a wide, soft halo

struct VS_OUT {
	vec2 uv;
	vec3 view_position;
};

in VS_OUT vs_out;

layout (binding = 0) uniform sampler2D bloom_in;

layout (location = 0) out vec4 pixel_color;

uniform vec2 texel; // 1 / target size
uniform vec2 dir;   // (1,0) or (0,1)

void main()
{
	vec2 o = texel * dir;
	vec3 c  = texture(bloom_in, vs_out.uv).rgb * 0.2270270270;
	c += (texture(bloom_in, vs_out.uv + o * 1.3846153846).rgb
	    + texture(bloom_in, vs_out.uv - o * 1.3846153846).rgb) * 0.3162162162;
	c += (texture(bloom_in, vs_out.uv + o * 3.2307692308).rgb
	    + texture(bloom_in, vs_out.uv - o * 3.2307692308).rgb) * 0.0702702703;
	pixel_color = vec4(c, 1.0);
}
