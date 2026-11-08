#version 420 core

// full-resolution volumetric clouds : a 3D noise texture baked by a compute
// shader, marched with a short sun shadow march and composited as premultiplied
// alpha. the texture keeps the per-step cost low enough for a clean full-res pass

struct VS_OUT {
	vec2 uv;
	vec3 view_position;
};

in VS_OUT vs_out;

layout (binding = 0) uniform sampler2D scene_depth;
layout (binding = 1) uniform sampler3D cloud_noise;

layout (location = 0) out vec4 pixel_color;

uniform mat4  inv_proj_view;
uniform vec3  view_pos;
uniform vec3  sun_direction;
uniform vec3  sun_color;
uniform vec3  ambient_up;
uniform float sun_power;
uniform float cloud_time;
uniform float cloud_weather;
uniform int   cloud_steps;

const float PI = 3.14159265359;

const float CLOUD_BASE = 140.0;
const float CLOUD_TOP  = 300.0;

float cloud_density(vec3 p)
{
	// the compute pass bakes shape/detail/mask into RGB and a warp scalar in A
	vec3 uv = p * 0.0026 + vec3(cloud_time * 0.0040, cloud_time * 0.0015, cloud_time * 0.0014);
	vec4 w = texture(cloud_noise, uv * 0.5 + vec3(0.13, 0.27, 0.41));
	vec3 q = uv + vec3(w.a - 0.5, 0.0, w.a - 0.5) * 0.55;

	vec4 n = texture(cloud_noise, q);
	float nval = n.r * 0.68 + n.g * 0.40 + 0.06;
	float cover = smoothstep(0.50, 0.64, nval)
	            * smoothstep(0.38, 0.54, n.b + 0.10);

	cover = mix(cover, smoothstep(0.44, 0.60, nval) * 0.95, cloud_weather * 0.70);

	float hn = clamp((p.y - CLOUD_BASE) / (CLOUD_TOP - CLOUD_BASE), 0.0, 1.0);
	float height = smoothstep(0.0, 0.06, hn) * (1.0 - smoothstep(0.72, 1.0, hn));
	return cover * height * 2.10;
}

void main()
{
	if (texture(scene_depth, vs_out.uv).r < 0.9999) discard; // terrain owns this pixel

	vec2 ndc = vs_out.uv * 2.0 - 1.0;
	vec4 far = inv_proj_view * vec4(ndc, 1.0, 1.0);
	vec3 dir = normalize(far.xyz / far.w - view_pos);
	if (dir.y < 0.020) discard;

	float t0 = (CLOUD_BASE - view_pos.y) / dir.y;
	float t1 = (CLOUD_TOP  - view_pos.y) / dir.y;
	if (t1 <= 0.0) discard;
	t0 = max(t0, 0.0);
	if (t1 <= t0) discard;
	t1 = min(t1, 6000.0);
	if (t1 <= t0 + 1.0) discard;

	const int MAX_STEPS = 128;
	int steps = clamp(cloud_steps, 8, MAX_STEPS);
	float dt = (t1 - t0) / float(steps);

	vec3 sun_dir = -sun_direction;
	float phase = 0.55 + 0.45 * pow(max(dot(dir, sun_dir), 0.0), 6.0);

	vec3 col = vec3(0.0);
	float trans = 1.0;

	for (int i = 0; i < MAX_STEPS; i++)
	{
		if (i >= steps) break;
		vec3 p = view_pos + dir * (t0 + (float(i) + 0.5) * dt);
		float d = cloud_density(p);
		if (d <= 0.005) continue;

		float shadow = 0.0;
		for (int j = 1; j <= 3; j++)
			shadow += cloud_density(p + sun_dir * (float(j) * 40.0));

		float light = exp(-shadow * 0.42);
		float powder = 1.0 - exp(-d * 3.0);
		vec3 lit = mix(ambient_up * 0.45 + vec3(0.06, 0.08, 0.12),
		               sun_color * 2.20, 0.12 + 0.88 * light);
		lit *= 0.60 + 0.55 * powder;

		// storm bases are grey and heavy
		lit = mix(lit, lit * vec3(0.55, 0.60, 0.68), cloud_weather * 0.70);

		float a = 1.0 - exp(-d * dt * 0.040);
		col += trans * a * lit * phase;
		trans *= 1.0 - a;
		if (trans < 0.015) break;
	}

	if (any(isnan(col))) col = vec3(0.0);
	col = clamp(col, vec3(0.0), vec3(8.0));
	pixel_color = vec4(col, 1.0 - trans); // premultiplied
}
