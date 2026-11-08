#version 420 core

// forward water : the opaque frame is copied to scene_color, then the surface
// refracts it, absorbs it with depth, reflects the sky and adds foam. drawn after
// the deferred lights, so the water is lit by the same sun and environment

struct VS_OUT
{
	vec3 normal;
	vec3 world_position;
	vec2 uv;
};

in VS_OUT vs_out;
flat in vec4 mat_a; // water colour rgb + mean surface height
flat in vec4 mat_b; // roughness, ao, open-sea flag, 1

layout (binding = 0) uniform sampler2D foam_map;
layout (binding = 1) uniform sampler2D scene_color;
layout (binding = 2) uniform sampler2D scene_depth;
layout (binding = 3) uniform sampler2D water_normal_map;
layout (binding = 5) uniform sampler2D scene_normal;

layout (location = 0) out vec4 pixel_color;

uniform vec3  view_pos;
uniform mat4  proj_view;
uniform mat4  inv_proj_view;
uniform vec2  screen_size;
uniform float near_plane;
uniform float far_plane;

uniform vec3  sun_direction;
uniform vec3  sun_color;
uniform float sun_power;

uniform vec3  moon_direction;
uniform vec3  moon_color;
uniform float moon_power;
uniform float moon_disk;

uniform vec3 zenith;
uniform vec3 haze;
uniform vec3 glow;
uniform vec3 ground;
uniform vec3 ambient_up;
uniform vec3 ambient_side;
uniform vec3 ambient_down;

uniform float fog_density;

uniform vec3  foam_color;
uniform float foam_roughness;
uniform float shore_foam;
uniform float sun_water;
uniform vec3  water_absorb; // per-scene absorption, rgb == red/green/blue depth
uniform float water_detail;
uniform float water_chop; // small procedural ripples, independent of the detail map
uniform float water_sss;
uniform float water_time;
uniform float ssr_on;
uniform int   ssr_steps;

const float PI = 3.14159265359;

// ---- procedural sky detail -------------------------------------------------

float hash21(vec2 p)
{
	vec2 q = fract(p * vec2(123.34, 456.21));
	q += dot(q, q + 45.32);
	return fract(q.x * q.y);
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

float cloud_fbm(vec2 p)
{
	float v = 0.0;
	float a = 0.5;
	for (int i = 0; i < 4; i++)
	{
		v += a * value_noise(p);
		p = p * 2.03 + 17.1;
		a *= 0.5;
	}
	return v;
}

// single-scattering atmosphere, matching the deferred sky
vec3 atmosphere(vec3 dir)
{
    vec3 L_sun = -sun_direction;
    float mu = max(dot(dir, L_sun), 0.0);
    float h = max(dir.y, 0.0);
    float sun_height = clamp(-sun_direction.y, 0.0, 1.0);
    float airmass = 1.0 / (h + 0.16);

    float phase_r = 0.75 * (1.0 + mu * mu);
    vec3 rayleigh = vec3(0.055, 0.200, 0.620) * phase_r * airmass;

    const float g = 0.76;
    float denom = max(1.0 + g*g - 2.0*g*mu, 1e-4);
    float phase_m = (1.0 - g*g) / (4.0 * PI * pow(denom, 1.5));
    vec3 mie_tint = mix(vec3(1.0, 0.62, 0.32), vec3(1.0, 0.92, 0.82), sun_height);
    vec3 mie = mie_tint * phase_m * airmass;

    return (rayleigh * 0.010 + mie * 0.006) * (sun_color * sun_power);
}

// ---- environment : the same sky the opaque pass uses ----------------------
vec3 environment(vec3 dir)
{
	dir = normalize(dir);
	float h = dir.y;
	vec3 L_sun = -sun_direction;

	vec2 flat_dir = normalize(vec2(dir.x, dir.z) + vec2(1e-4, 0.0));
	vec2 flat_sun = normalize(vec2(L_sun.x, L_sun.z) + vec2(1e-4, 0.0));
	float azimuth = max(dot(flat_dir, flat_sun), 0.0);

	vec3 horizon = mix(haze, glow, pow(azimuth, 3.0) * 0.35);
	vec3 col = (h > 0.0) ? mix(horizon, zenith, pow(h, 0.55))
	                     : mix(horizon, ground, pow(-h, 0.45));

	// no sun or moon disk here. a rough surface never mirrors a hard disk : the
	// reflection is broad sky scatter, and the tight glint is the GGX lobe below.
	// a disk in this function reads as a second sun sitting under the water.
	float above = smoothstep(0.0, 0.12, h);
	float d = max(dot(dir, L_sun), 0.0);
	vec3 sky = col + atmosphere(dir) * above;
	float high = smoothstep(0.02, 0.40, h);
	sky = mix(sky, sky * vec3(0.55, 0.78, 1.35), 0.60 * high);
	sky += sun_color * (pow(d, 10.0) * 0.16 + pow(d, 3.0) * 0.03) * sun_water;

	vec3 L_moon = -moon_direction;
	float dm = max(dot(dir, L_moon), 0.0);
	sky += moon_color * (pow(dm, 10.0) * 0.10) * moon_disk;
	return sky;
}

vec3 ambient_irradiance(vec3 N)
{
	return (N.y > 0.0) ? mix(ambient_side, ambient_up,   smoothstep(0.0, 1.0,  N.y))
	                   : mix(ambient_side, ambient_down, smoothstep(0.0, 1.0, -N.y));
}

// ---- screen-space reflection ----------------------------------------------
// march the reflected ray through the opaque depth buffer. the hit is weighted by
// a screen-edge and thickness confidence, so a marginal hit fades into the sky
// rather than popping a stretched colour patch across the surface
vec4 screen_reflection(vec3 P, vec3 R)
{
	if (ssr_steps <= 0) return vec4(0.0);
	float t = 0.15;
	float step_len = 0.30;
	vec3 start = P + R * 0.04;
	for (int i = 0; i < 24; i++)
	{
		if (i >= ssr_steps) break;
		vec3 Q = start + R * t;
		vec4 clip = proj_view * vec4(Q, 1.0);
		if (clip.w <= 0.0) break;
		vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
		if (uv.x <= 0.002 || uv.x >= 0.998 || uv.y <= 0.002 || uv.y >= 0.998) break;

		float d = texture(scene_depth, uv).r;
		if (d < 0.9999)
		{
			vec4 ndc = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
			vec4 wp = inv_proj_view * ndc;
			vec3 SP = wp.xyz / wp.w;
			float ray_d = length(Q - view_pos);
			float scene_d = length(SP - view_pos);
			float diff = ray_d - scene_d;
			float thick = max(0.10, t * 0.08);
			if (scene_d < ray_d && diff < thick
			    && SP.y > mat_a.a - 0.02) // never reflect the submerged floor
			{
				float edge = smoothstep(0.0, 0.10, uv.x) * smoothstep(0.0, 0.10, 1.0 - uv.x)
				           * smoothstep(0.0, 0.10, uv.y) * smoothstep(0.0, 0.10, 1.0 - uv.y);
				float conf = edge * (1.0 - smoothstep(thick * 0.5, thick, diff));
				return vec4(texture(scene_color, uv).rgb, conf);
			}
		}

		t += step_len;
		step_len *= 1.12;
	}
	return vec4(0.0);
}

// ---- brdf -----------------------------------------------------------------
float distribution_ggx(float NdotH, float roughness)
{
	float a2 = roughness * roughness;
	a2 *= a2;
	float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
	return a2 / max(PI * d * d, 1e-6);
}

float geometry_smith(float NdotV, float NdotL, float roughness)
{
	float k = pow(roughness + 1.0, 2.0) / 8.0;
	float gv = NdotV / (NdotV * (1.0 - k) + k);
	float gl = NdotL / (NdotL * (1.0 - k) + k);
	return gv * gl;
}

vec3 fresnel_schlick(float cosTheta, vec3 F0)
{
	float c = clamp(cosTheta, 0.0, 1.0);
	return F0 + (1.0 - F0) * pow(1.0 - c, 5.0);
}

// ---- screen-space helpers -------------------------------------------------
float linear_depth(float d)
{
	float z = d * 2.0 - 1.0;
	return (2.0 * near_plane * far_plane) / (far_plane + near_plane - z * (far_plane - near_plane));
}

void main()
{
	vec2 screen_uv = gl_FragCoord.xy / screen_size;

	vec3 V = normalize(view_pos - vs_out.world_position);
	float water_level = mat_a.a;
	float dist = length(vs_out.world_position - view_pos);

	float water_z = linear_depth(gl_FragCoord.z);
	float scene_z = linear_depth(texture(scene_depth, screen_uv).r);
	float thickness = clamp(scene_z - water_z, 0.0, 80.0);

	// short chop : three world-space scales of the 16 m cascade. the fine layers
	// fade with distance so the horizon does not shimmer
	vec2 detail_uv = vs_out.world_position.xz * (1.0 / 16.0);
	vec2 detail_uv2 = vs_out.world_position.xz * (1.0 / 5.7)
	                + vec2(water_time * .004, -water_time * .003);
	vec2 d1 = texture(water_normal_map, detail_uv).rg * 2.0 - 1.0;
	vec2 d2 = texture(water_normal_map, detail_uv.yx * 1.63 + vec2(.37, .11)).rg * 2.0 - 1.0;
	vec2 d3 = texture(water_normal_map, detail_uv2.yx * 1.37 + vec2(.19, .53)).rg * 2.0 - 1.0;

	float detail_fade = 1.0 - smoothstep(80.0, 250.0, dist);
	vec3 detail = vec3(d1.x + d2.x + d3.x * .55, 0.0,
	                   d1.y + d2.y + d3.y * .55) * (water_detail * 0.68 * detail_fade);
	vec3 N = vs_out.normal + detail;

	// small procedural ripples : the shallow patch has no second normal cascade,
	// and even the ocean benefits from a fine chop that does not tile
	vec2 mp = vs_out.world_position.xz;
	float mt = water_time;
	// calm the fine ripples with depth : a deep sea is smoother than a puddle
	float depth_ripple = 1.0 - 0.55 * smoothstep(1.0, 10.0, thickness);
	vec2 micro = vec2(
		sin(mp.x * 2.7 + mt * 1.3 + sin(mp.y * 1.9 - mt * 0.7)),
		cos(mp.y * 2.3 - mt * 1.1 + sin(mp.x * 1.5 + mt * 0.9)));
	N += vec3(micro.x, 0.0, micro.y) * (0.70 * water_chop * detail_fade * depth_ripple);

	// a finer layer breaks the surface into small facets, so the sky reflection
	// sparkles instead of reading as one smooth sheet
	float fine_fade = 1.0 - smoothstep(12.0, 80.0, dist);
	vec2 micro2 = vec2(
		sin(mp.x * 9.7 - mt * 1.7 + sin(mp.y * 7.3 + mt * 1.1)),
		cos(mp.y * 8.1 + mt * 1.4 + sin(mp.x * 6.7 - mt * 1.3)));
	N += vec3(micro2.x, 0.0, micro2.y) * (0.90 * water_chop * fine_fade * depth_ripple);

	// refraction uses the full ripple field : the bottom must visibly wobble
	vec3 N_refr = normalize(N);

	N.y = max(N.y, 0.55);
	N = normalize(N);
	vec3 Nup = N; // always the sunlit side, even when the face is seen from below

	// seen from under the surface the same normal belongs to the underside, so it
	// must face the camera or fresnel picks the dark reflection every time
	if (!gl_FrontFacing) { N = -N; N_refr = -N_refr; }

	// glitter : a tiny high-frequency normal just for the sun/moon highlight. the
	// lace mask is already in memory, so it makes a cheap sparkle field
	float glitter_fade = 1.0 - smoothstep(18.0, 110.0, dist);
	vec2 g_uv = vs_out.world_position.xz / 2.7 + vec2(water_time * .020, water_time * .011);
	float gx = value_noise(g_uv * 9.0) - 0.5;
	float gz = value_noise(g_uv.yx * 9.0 + vec2(.31, .17)) - 0.5;
	vec3 Ng = normalize(N + vec3(gx, 0.0, gz) * (0.04 * water_detail * glitter_fade));
	Ng.y = max(Ng.y, 0.40);
	Ng = normalize(Ng);

	// refraction : bend the screen lookup by the surface normal, then fall back to
	// the unshifted pixel if the shifted one belongs to a nearer surface
	vec2 refr_uv = screen_uv + N_refr.xz * (0.032 / max(water_z, 0.8));
	refr_uv = clamp(refr_uv, vec2(0.001), vec2(0.999));
	float refr_z = linear_depth(texture(scene_depth, refr_uv).r);
	float refr_ok = smoothstep(0.0, 0.25, refr_z - water_z);
	refr_uv = mix(screen_uv, refr_uv, refr_ok);

	vec3 background = texture(scene_color, refr_uv).rgb;

	// absorption : red dies first, then green. the water itself is clear until
	// the path gets long, so a shallow pool shows its sand almost unchanged
	vec3 transmittance = exp(-thickness * water_absorb);

	// in-scattering : the water volume slowly adds its own colour as the path
	// through it grows. this is what turns a clear shallows into a blue sea
	float scatter_amount = 1.0 - exp(-thickness * 0.35);
	// neutral light level for the volume : the authored water colour should not
	// pick up a strong green cast from a cyan ambient
	float water_light = dot(ambient_up, vec3(0.3333))
	                  + dot(sun_color, vec3(0.3333)) * sun_power * 0.05;

	// depth ramp : shallow water scatters turquoise, deep water the authored blue.
	// the change with thickness is the main cue for how deep the water is
	float depth_t = 1.0 - exp(-thickness * 0.150);
	vec3 shallow_col = vec3(0.030, 0.140, 0.155);
	vec3 deep_col = mat_a.rgb * 1.9;
	vec3 body_col = mix(shallow_col, deep_col, depth_t);
	vec3 scatter = body_col * (1.35 * water_light);
	vec3 beneath = background * transmittance + scatter * scatter_amount;

	// fresnel : glassy toward the horizon, nearly transparent when looked into
	// from above. the reflected ray is only slightly biased toward the normal,
	// so the ripples still appear in the reflection
	float NdotV = max(dot(N, V), 1e-3);
	float cosv = mix(clamp(V.y, 0.0, 1.0), NdotV, 0.30);
	float fresnel = 0.02 + 0.98 * pow(1.0 - cosv, 5.0);
	vec3 R = reflect(-V, N);
	R = normalize(mix(R, N, 0.12));
	R.y = max(R.y, 0.02);
	vec3 env_refl = environment(normalize(R));
	vec4 ssr = vec4(0.0);
	if (ssr_on > 0.5) ssr = screen_reflection(vs_out.world_position, normalize(R));
	vec3 reflection = mix(env_refl, ssr.rgb, ssr.a * 0.85);
	reflection = mix(reflection, mat_a.rgb * 1.6, 0.12); // a hint of the water body
	vec3 color = mix(beneath, reflection, fresnel);

	// sun highlight : a tight GGX lobe, widened with distance so the far water
	// does not alias into a field of hard white pixels
	vec3 L = -sun_direction;
	float NdotL = max(dot(N, L), 0.0);
	if (NdotL > 0.0)
	{
		float sun_rough = mix(max(mat_b.r, 0.09), 0.20, smoothstep(25.0, 200.0, dist));
		vec3 hv = V + L;
		if (dot(hv, hv) < 1e-8) hv = N; // backlit : a zero half-vector would be NaN
		vec3 H = normalize(hv);
		float NDF = distribution_ggx(max(dot(Ng, H), 0.0), sun_rough);
		float G = geometry_smith(NdotV, NdotL, sun_rough);
		vec3 F = fresnel_schlick(max(dot(H, V), 0.0), vec3(0.02));
		vec3 specular = NDF * G * F / (4.0 * NdotV * NdotL + 1e-6);
		color += specular * sun_color * sun_power * NdotL * sun_water;
	}

	// moonlight : the same lobe, cool and much weaker
	vec3 Lm = -moon_direction;
	float NdotLm = max(dot(N, Lm), 0.0);
	if (NdotLm > 0.0 && moon_power > 0.0)
	{
		vec3 Hm = normalize(V + Lm);
		if (dot(V + Lm, V + Lm) < 1e-8) Hm = N; // same guard as the sun lobe
		float NDFm = distribution_ggx(max(dot(Ng, Hm), 0.0), max(mat_b.r, 0.09));
		float Gm = geometry_smith(NdotV, NdotLm, max(mat_b.r, 0.09));
		vec3 Fm = fresnel_schlick(max(dot(Hm, V), 0.0), vec3(0.02));
		vec3 specularm = NDFm * Gm * Fm / (4.0 * NdotV * NdotLm + 1e-6);
		color += specularm * moon_color * moon_power * NdotLm * sun_water;
	}

	// subsurface scattering : light entering a crest scatters forward through the
	// thin water. the peak mask follows the crest top and its steep sides, and the
	// phase term peaks when the sun is behind the wave from this view
	float crest_height = vs_out.world_position.y - water_level;
	float crest = smoothstep(0.02, 0.40, crest_height);
	float steep = smoothstep(0.02, 0.45, 1.0 - Nup.y);
	float peak = crest * (0.35 + 0.65 * steep);
	// wrapped phase : strongest when the sun is behind the crest, but the volume
	// still carries a little light from the side and above
	float backlight = pow(clamp(dot(V, sun_direction) * 0.5 + 0.5, 0.0, 1.0), 2.0);
	float sss = peak * backlight * (1.0 - fresnel) * water_sss;
	vec3 sss_tint = mat_a.rgb * 1.2 + vec3(0.005, 0.02, 0.03);
	color += sss_tint * (sun_color * sun_power * 0.55 + ambient_up * 0.8) * sss;

	// foam : simulated crests, a breaking band where the water is shallow, and a
	// public-domain lace mask so the whitewater has structure instead of a flat
	// blob. two scales drift at different speeds to break up the tiling
	vec2 foam_uv = vs_out.world_position.xz / 7.0;
	float lace_a = cloud_fbm(foam_uv * 2.3 + vec2(water_time * 0.008, water_time * 0.003));
	float lace_b = cloud_fbm(foam_uv * 4.1 + vec2(-water_time * 0.012, water_time * 0.005));
	float lace = smoothstep(0.35, 0.75, lace_a * 0.65 + lace_b * 0.35);

	float foam = 0.0;
	if (mat_b.a > 0.5)
		foam = smoothstep(0.15, 0.85, texture(foam_map, vs_out.uv).r);

	// breaking : the band widens with the crest, a slow run-up pulse washes it up
	// and back, and a steep bottom (rock, wall, reef) pulls more whitewater
	vec3 bottom_n = texture(scene_normal, screen_uv).rgb;
	float bottom_slope = 1.0 - clamp(normalize(bottom_n + vec3(0.0, 1e-4, 0.0)).y, 0.0, 1.0);
	float shore_width = 0.18 + 1.4 * crest;
	float shore = 1.0 - smoothstep(0.0, shore_width, thickness);
	float runup = 0.72 + 0.28 * sin(water_time * 1.15 + vs_out.world_position.x * 0.17
	                              + vs_out.world_position.z * 0.11);
	shore *= runup;
	float breakers = shore * (0.25 + 0.75 * crest) * (0.55 + 0.90 * bottom_slope) * shore_foam;
	foam = max(foam, breakers);

	// a thin bright contact line where the water meets the shore : the depth
	// buffer is almost exactly at the surface there, so this rings the pool and
	// every beach with a crisp edge
	float edge = 1.0 - smoothstep(0.0, 0.05, thickness);
	foam = max(foam, edge * 0.60 * shore_foam);

	foam *= mix(0.35, 1.15, lace);
	foam = clamp(foam, 0.0, 1.0);

	vec3 foam_linear = pow(foam_color, vec3(2.2));
	vec3 foam_lit = foam_linear * (ambient_irradiance(vec3(0.0, 1.0, 0.0)) * 1.5
	                             + sun_color * sun_power * 0.28);
	foam_lit *= 1.0 + (1.0 - foam_roughness) * 0.6; // gloss : brighter foam
	color = mix(color, foam_lit, clamp(foam, 0.0, 1.0));

	// a little glisten on the foam itself : the lace mask doubles as a bump
	float foam_spec = pow(max(dot(reflect(-V, Ng), -sun_direction), 0.0), 24.0) * foam;
	color += foam_spec * sun_color * sun_power * 0.22;

	// fog : the same distance fade as the opaque pass. the fog colour comes from
	// the sky above the horizon, never the dark ground hemisphere under the sea
	vec3 view_dir = normalize(vs_out.world_position - view_pos);
	view_dir.y = max(view_dir.y, 0.02);
	color = mix(color, environment(view_dir), 1.0 - exp(-dist * fog_density));

	if (any(isnan(color))) color = reflection; // a bad normal must not punch a hole
	color = clamp(color, vec3(0.0), vec3(64.0));
	pixel_color = vec4(color, 1.0);
}
