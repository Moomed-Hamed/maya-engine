#version 420 core

// bloom bright pass : the HDR frame minus its darker half, at half resolution.
// the soft knee keeps a gradient from popping as pixels cross the threshold

struct VS_OUT {
	vec2 uv;
	vec3 view_position;
};

in VS_OUT vs_out;

layout (binding = 0) uniform sampler2D lit_color;

layout (location = 0) out vec4 pixel_color;

uniform float threshold;

void main()
{
	vec3 c = texture(lit_color, vs_out.uv).rgb;
	float l = dot(c, vec3(0.2126, 0.7152, 0.0722));

	float knee = max(threshold * 0.55, 1e-3);
	float w = smoothstep(threshold - knee, threshold + knee, l);

	// keep one blown-out sun from flooding the whole frame
	c = min(c, vec3(24.0));
	pixel_color = vec4(c * w, 1.0);
}
