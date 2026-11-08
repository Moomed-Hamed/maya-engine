#version 420 core

// waterline pass : tints the frame when the eye is under the sea, and only the
// below-water part of the screen when the eye is half submerged. the seam is
// the ray / water-plane intersection, so the above-water half keeps its normal
// exposure and colour.

struct VS_OUT {
	vec2 uv;
	vec3 view_position;
};

in VS_OUT vs_out;

layout (binding = 0) uniform sampler2D scene_color;
layout (binding = 1) uniform sampler2D scene_depth;

layout (location = 0) out vec4 pixel_color;

uniform mat4  inv_proj_view;
uniform vec3  view_pos;
uniform float water_on;
uniform float water_level;
uniform vec3  underwater_color;
uniform float underwater_density;
uniform float near_plane;
uniform float far_plane;
uniform float water_time;

float linear_depth(float d)
{
	float z = d * 2.0 - 1.0;
	return (2.0 * near_plane * far_plane) / (far_plane + near_plane - z * (far_plane - near_plane));
}

void main()
{
	vec2 uv = vs_out.uv;
	vec3 color = texture(scene_color, uv).rgb;

	if (water_on < 0.5)
	{
		pixel_color = vec4(color, 1.0);
		return;
	}

	vec2 ndc = uv * 2.0 - 1.0;
	vec4 far = inv_proj_view * vec4(ndc, 1.0, 1.0);
	vec3 dir = normalize(far.xyz / far.w - view_pos);

	float scene_z = linear_depth(texture(scene_depth, uv).r);
	float plane_t = 1e9;
	if (abs(dir.y) > 1e-4) plane_t = (water_level - view_pos.y) / dir.y;

	// the eye crossing the surface is not a hard switch : the underwater fog
	// fades in over the last centimetres, so walking into water does not look
	// like a different shader turning on
	float under_eye = 1.0 - smoothstep(water_level - 0.30, water_level + 0.18, view_pos.y);
	bool near_surface = abs(view_pos.y - water_level) < 0.75;
	float below_plane = (near_surface && plane_t > 0.0 && plane_t < scene_z) ? 1.0 : 0.0;
	float wet = max(below_plane, under_eye);

	if (wet < 0.001)
	{
		pixel_color = vec4(color, 1.0);
		return;
	}

	// wobble only once the eye is really under, faded in
	if (under_eye > 0.001)
	{
		vec2 wobble = vec2(sin(uv.y * 44.0 + water_time * 2.2),
		                   cos(uv.x * 51.0 - water_time * 1.8)) * 0.0012;
		vec3 wobbled = texture(scene_color, clamp(uv + wobble, vec2(0.001), vec2(0.999))).rgb;
		color = mix(color, wobbled, under_eye);
	}

	float depth_below = max(scene_z - plane_t, 0.0);
	float depth = mix(depth_below, scene_z, under_eye);
	float fog = 1.0 - exp(-depth * underwater_density);
	fog *= wet;
	color = mix(color, underwater_color, fog);

	// a touch more blue-green in the last metres, so the eye reads the medium
	float tint = 1.0 - exp(-depth * underwater_density * 0.45);
	color = mix(color, underwater_color, tint * 0.35 * wet);

	pixel_color = vec4(color, 1.0);
}
