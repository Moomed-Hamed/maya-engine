#version 420 core

// deferred lighting pass : one fullscreen quad shades the frame from the g-buffer
// (world pos + metallic, normal + roughness, albedo + ao).
//
// the rig is a key sun and a cool rim, uploaded per scene, over a procedural hazy
// sky that serves as the background, the ambient and the reflections at once, so
// what a mirror shows is what the camera sees. occlusion is screen space and
// applies to the ambient only - there are no shadow maps yet, so the key light
// does not cast. the lighting pass binds the samplers and uploads view_pos,
// proj_view and the scene's lighting.

struct VS_OUT {
    vec2 uv;
    vec3 view_position;
};

in VS_OUT vs_out;

layout (binding = 0) uniform sampler2D positions; // rgb world pos, a metallic
layout (binding = 1) uniform sampler2D normals;   // rgb world normal, a roughness
layout (binding = 2) uniform sampler2D albedo;    // rgb base color, a = ao
layout (binding = 3) uniform sampler2DShadow shadow_map;

layout (location = 0) out vec4 pixel_color;

uniform mat4 proj_view; // the occlusion taps land with it
uniform mat4 inv_proj_view; // sky pixels need the real per-pixel view ray

uniform mat4  light_proj_view;
uniform vec3  shadow_direction;
uniform float shadow_texel;
uniform float shadow_world_texel;
uniform float shadow_on;
uniform float shadow_moon;
uniform float shadow_bias;

// ---- per-scene lighting ---------------------------------------------------
// uploaded from GameWindow::draw_gbuf. directions are the direction the light
// travels; every use below flips them to surface -> light.

uniform vec3  sun_direction;
uniform vec3  sun_color;
uniform float sun_power;
uniform float sun_disk;      // the bright spot in the procedural sky

uniform vec3  rim_direction;
uniform vec3  rim_color;
uniform float rim_power;

uniform vec3  moon_direction;
uniform vec3  moon_color;
uniform float moon_power;
uniform float moon_disk;

uniform float fog_density;
uniform float multiscatter;
uniform float ao_strength;
uniform float ssao_on;
uniform float night; // 0 = day, 1 = night : drives the emissive markers
uniform float sky_time;

uniform vec3 zenith;
uniform vec3 haze;
uniform vec3 glow;
uniform vec3 ground;

uniform vec3 ambient_up;
uniform vec3 ambient_side;
uniform vec3 ambient_down;

const float PI = 3.14159265359;

// ---- clouds ----------------------------------------------------------------
// one softly-lit cloud plane with a domain-warped 2D fbm. no raymarch steps, no
// dither, no noise : the layer stays clean at every resolution and costs a
// handful of fbm samples per sky pixel
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

vec4 cloud_layer(vec3 dir)
{
    if (dir.y < 0.020) return vec4(0.0);

    // sky-dome projection : the cloud layer never collapses into a hard band at
    // the horizon and the cells keep a natural angular size
    vec2 uv = dir.xz / (dir.y + 0.14) * 0.95
            + vec2(sky_time * 0.0040, sky_time * 0.0018);

    vec2 warp = vec2(cloud_fbm(uv * 0.70 + vec2(3.1, 1.7)),
                     cloud_fbm(uv * 0.70 + vec2(8.3, 4.2))) * 2.0 - 1.0;
    vec2 q = uv + warp * 0.55;

    float shape  = cloud_fbm(q * 1.15);
    float detail = cloud_fbm(q * 3.30 + vec2(sky_time * 0.0012, -sky_time * 0.0008));
    float n = shape * 0.70 + detail * 0.30;

    float mask = cloud_fbm(uv * 0.32 + vec2(7.0, 2.0));
    float cover = smoothstep(0.42, 0.58, n)
                * smoothstep(0.28, 0.50, mask + 0.14)
                * smoothstep(0.02, 0.10, dir.y);
    if (cover <= 0.001) return vec4(0.0);

    // the part of the cloud toward the sun shades the part behind it
    vec2 sun_uv = q - sun_direction.xz * 0.045;
    float shadow_n = cloud_fbm(sun_uv * 1.15) * 0.70
                   + cloud_fbm(sun_uv * 3.30) * 0.30;
    float light = 1.0 - smoothstep(0.42, 0.64, shadow_n);

    // detail also stands in for vertical billow : bright tops, grey bases
    float billow = smoothstep(0.38, 0.72, detail);
    vec3 lit = mix(ambient_up * 0.35 + vec3(0.05, 0.06, 0.09),
                   sun_color * 2.20,
                   0.12 + 0.88 * light);
    lit *= 0.70 + 0.40 * billow;

    float alpha = clamp(cover * (0.75 + 0.35 * billow), 0.0, 1.0);
    return vec4(lit, alpha);
}

// ---- the environment ------------------------------------------------------

// single-scattering atmosphere : Rayleigh blue plus a Mie forward halo. the
// airmass term makes the horizon thicker and warmer than the zenith, which is
// the part that reads as atmosphere rather than a two-colour gradient
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

    vec3 sun_rad = sun_color * sun_power;
    return (rayleigh * 0.010 + mie * 0.006) * sun_rad;
}

// the sky, by direction : dark overhead, hazy at the horizon and warm toward the
// sun, dark underfoot. the background branch, the ambient and every reflection call
// this, so the world and what it reflects are the same place
vec3 environment(vec3 dir)
{
    dir = normalize(dir);
    float h = dir.y;
    vec3 L_sun = -sun_direction;

    // how much of the sun's compass direction this ray carries : the horizon warms
    // around it whether the sun is high or low
    vec2 flat_dir = normalize(vec2(dir.x, dir.z) + vec2(1e-4, 0.0));
    vec2 flat_sun = normalize(vec2(L_sun.x, L_sun.z) + vec2(1e-4, 0.0));
    float azimuth = max(dot(flat_dir, flat_sun), 0.0);

    vec3 horizon = mix(haze, glow, pow(azimuth, 3.0) * 0.85);
    vec3 col = (h > 0.0) ? mix(horizon, zenith, pow(h, 0.55))
                         : mix(horizon, ground, pow(-h, 0.45));

    // atmosphere above the horizon, warm dusk light near the horizon line
    float above = smoothstep(0.0, 0.12, h);
    col += atmosphere(dir) * above;

    // deepen the high sky : a haze-only gradient reads almost white at this
    // exposure, and the clouds need some blue to sit against
    float high = smoothstep(0.02, 0.40, h);
    col = mix(col, col * vec3(0.55, 0.78, 1.35), 0.60 * high);
    float sun_height = clamp(-sun_direction.y, 0.0, 1.0);
    float dusk = 1.0 - smoothstep(0.0, 0.55, sun_height);
    col = mix(col, col * vec3(1.28, 0.82, 0.60), dusk * 0.30 * above);

    // the sun : an antialiased disk plus a bloom wide enough to smear across a rough
    // sphere instead of sitting on it as a dot
    float d = max(dot(dir, L_sun), 0.0);
    float sun_patch = smoothstep(0.99930, 0.99975, d);
    vec3 sky = col + sun_color * (sun_patch * 26.0 + pow(d, 24.0) * 0.35
                                + pow(d, 4.0) * 0.05) * sun_disk;

    // the moon is a smaller, cooler disk with a faint halo
    vec3 L_moon = -moon_direction;
    float dm = max(dot(dir, L_moon), 0.0);
    float moon_patch = smoothstep(0.99955, 0.99982, dm);
    sky += moon_color * (moon_patch * 22.0 + pow(dm, 96.0) * 0.30) * moon_disk;

    return sky;
}

// hemisphere irradiance : three zones is enough for the eye to read the sky's shape
vec3 ambient_irradiance(vec3 N)
{
    return (N.y > 0.0) ? mix(ambient_side, ambient_up,   smoothstep(0.0, 1.0,  N.y))
                       : mix(ambient_side, ambient_down, smoothstep(0.0, 1.0, -N.y));
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
    float k = pow(roughness + 1.0, 2.0) / 8.0; // direct-lighting form
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

vec3 fresnel_schlick(float cosTheta, vec3 F0)
{
    float c = clamp(cosTheta, 0.0, 1.0); // pow() is undefined for a negative base
    return F0 + (1.0 - F0) * pow(1.0 - c, 5.0);
}

// fresnel-weighted environment response (Karis analytic split-sum fit) :
// returns (scale, bias), ambient_spec = F0 * ab.x + ab.y
vec2 env_brdf(float roughness, float NdotV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// one analytic light : cook-torrance, and nothing occludes it. the occlusion below
// is an ambient term, so a lit fold stays lit until shadows exist
vec3 direct_light(vec3 N, vec3 V, vec3 L, vec3 radiance,
                  vec3 base, vec3 F0, float metallic, float spec_rough, float NdotV)
{
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);

    vec3  H  = normalize(V + L);
    float NDF = distribution_ggx(max(dot(N, H), 0.0), spec_rough);
    float G   = geometry_smith(NdotV, NdotL, spec_rough);
    vec3  F   = fresnel_schlick(max(dot(H, V), 0.0), F0);

    vec3 specular = NDF * G * F / (4.0 * NdotV * NdotL + 1e-6)
                  * mix(0.35, 1.0, 1.0 - spec_rough); // rough blocks stay matte
    vec3 kD = (1.0 - F) * (1.0 - metallic); // metals keep no diffuse

    return (kD * base / PI + specular) * radiance * NdotL;
}

// ---- occlusion ------------------------------------------------------------

// interleaved gradient noise : a stable pseudo-random per pixel, used to rotate the
// taps so the sampling pattern does not read as rings
float ssao(vec3 P, vec3 N)
{
    const int   taps   = 12;
    const float radius = 0.40; // metres : reaches the floor under a resting object

    // a fixed basis around the normal
    vec3  t = normalize(abs(N.z) < 0.9 ? cross(N, vec3(0.0, 0.0, 1.0))
                                       : cross(N, vec3(1.0, 0.0, 0.0)));
    vec3  x = t;
    vec3  y = cross(N, x);

    float occ = 0.0;
    for (int i = 0; i < taps; i++)
    {
        // spiral : even angular coverage, growing radius
        float fi  = float(i);
        float ang = fi * 2.399963; // golden angle
        float rad = radius * sqrt((fi + 0.5) / float(taps));
        vec3  tap = P + (x * cos(ang) + y * sin(ang)) * rad;

        vec4 clip = proj_view * vec4(tap, 1.0);
        if (clip.w <= 0.0) continue;
        vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue; // off screen

        vec4 s_norm = texture(normals, suv);
        if (dot(s_norm.rgb, s_norm.rgb) < 1e-8) continue; // sky : nothing to hit there

        // the same surface, one pixel over : on a convex edge the taps land back on
        // the object and it occludes itself, ringing every sphere with its own band
        if (dot(s_norm.rgb, N) > 0.9) continue;

        vec3 sp = texture(positions, suv).rgb;

        // and it has to sit properly between the eye and the tap, not a hair in front
        if (dot(sp - tap, normalize(tap - vs_out.view_position)) > -0.02) continue;

        occ += 1.0 - clamp(length(sp - P) / radius, 0.0, 1.0);
    }

    // keep AO low-contrast : a strong screen-space term reads as tap-pattern
    // banding on large flat faces and still leaves silhouettes bright
    float occl = occ / float(taps);
    return clamp(1.0 - occl * ao_strength * 0.35, 0.55, 1.0);
}

// 3x3 PCF sun visibility : zero outside the shadow frustum or when the feature
// is off, so the pass can be toggled without another shader. the normal offset
// and hardware PCF are what keep the map from banding across a flat surface
float sun_shadow(vec3 P, vec3 N)
{
    if (shadow_on < 0.5) return 1.0;

    // move the receiver a little along its normal before projecting : the cheapest
    // cure for self-shadow acne at grazing angles
    vec3 Pn = P + N * (shadow_world_texel * 1.5);
    vec4 lp = light_proj_view * vec4(Pn, 1.0);
    vec3 proj = lp.xyz / lp.w * 0.5 + 0.5;
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0 || proj.z > 1.0)
        return 1.0;

    float bias = max(shadow_bias * (1.0 - dot(N, -shadow_direction)), shadow_bias * 0.50);

    // twelve rotated taps over a hardware 2x2 comparison : a soft edge with no
    // visible grid, and enough samples that the dither does not read as holes
    // Vogel spiral : an even disc with no repeated linear pattern, so a flat
    // receiver cannot pick up stripes from the tap layout
    const int TAPS = 16;
    float lit = 0.0;
    for (int i = 0; i < TAPS; i++)
    {
        float fi  = float(i);
        float ang = fi * 2.39996323;
        float rad = sqrt((fi + 0.5) / float(TAPS)) * shadow_texel * 7.0;
        vec2 offset = vec2(cos(ang), sin(ang)) * rad;
        lit += texture(shadow_map, vec3(proj.xy + offset, proj.z - bias));
    }

    return lit / float(TAPS);
}

// ---- main -----------------------------------------------------------------

void main()
{
    vec4 pos_tex  = texture(positions, vs_out.uv);
    vec4 norm_tex = texture(normals,   vs_out.uv);
    vec4 alb_tex  = texture(albedo,    vs_out.uv);

    // per-pixel normal change, taken before the background branch : derivatives are
    // only defined in uniform flow. it becomes a roughness floor below, so edges and
    // grazing silhouettes go soft instead of sparkling
    vec3 dn = fwidth(norm_tex.rgb);

    vec3 view_dir = normalize(pos_tex.rgb - vs_out.view_position); // eye -> surface

    // nothing rasterized here : the open sky. the position texture is zero for
    // these pixels, so reconstruct the real eye -> sky ray from the inverse
    // projection instead of using the direction to the world origin
    if (dot(norm_tex.rgb, norm_tex.rgb) < 1e-8)
    {
        vec2 ndc = vs_out.uv * 2.0 - 1.0;
        vec4 far = inv_proj_view * vec4(ndc, 1.0, 1.0);
        vec3 sky_dir = normalize(far.xyz / far.w - vs_out.view_position);
        vec3 sky = environment(sky_dir);
        vec4 cloud = cloud_layer(sky_dir);
        sky = sky * (1.0 - cloud.a) + cloud.rgb * cloud.a;
        pixel_color = vec4(sky, 1.0);
        return;
    }

    // material : albedo is stored display-referred, and lighting runs linear
    vec3  base      = pow(alb_tex.rgb, vec3(2.2));
    float metallic  = clamp(pos_tex.a,  0.0, 1.0);
    float roughness = clamp(norm_tex.a, 0.045, 1.0);
    float ao        = clamp(alb_tex.a,  0.0, 1.0);

    // an ao value above 1 marks an emitter : the excess becomes the emissive
    // strength, and the rest of the pass still treats ao as a plain occlusion
    float emissive = max(alb_tex.a - 1.0, 0.0);

    vec3 N = normalize(norm_tex.rgb);
    vec3 P = pos_tex.rgb;
    vec3 V = -view_dir; // surface -> eye



    float NdotV = max(dot(N, V), 1e-4);

    // specular anti-aliasing : fold a little of the surface's own normal variance
    // into the lobe's roughness. the clamp is the important half - at a silhouette
    // the derivative jumps to the background's normal, and an unbounded fold makes
    // every edge fully rough, which on a metal reads as a black rim around it
    float variance   = min(dot(dn, dn), 0.09);
    float spec_rough = min(sqrt(roughness * roughness + variance), 1.0);

    vec3 F0 = mix(vec3(0.04), base, metallic);

    // occlusion : screen space, darkening the diffuse ambient only. a mirror is left
    // alone - the sky it reflects is not blocked by what stands in front of it, and
    // dimming the reflections is most of what made this read as flat
    float vis        = ssao_on > 0.5 ? ssao(P, N) : 1.0;
    float diffuse_ao = ao * vis;

    // --- the lights ----------------------------------------------------------
    vec3 L_sun = -sun_direction;
    vec3 L_rim = -rim_direction;

    float shadow_vis = sun_shadow(P, N);
    float sun_vis  = shadow_moon > 0.5 ? 1.0 : shadow_vis;
    float moon_vis = shadow_moon > 0.5 ? shadow_vis : 1.0;

    vec3 direct = direct_light(N, V, L_sun, sun_color * sun_power * sun_vis,
                               base, F0, metallic, spec_rough, NdotV)
                + direct_light(N, V, -moon_direction, moon_color * moon_power * moon_vis,
                               base, F0, metallic, spec_rough, NdotV)
                + direct_light(N, V, L_rim, rim_color * rim_power * max(1.0 - roughness, 0.0) * 0.8,
                               base, F0, metallic, spec_rough, NdotV);

    // --- ambient : the environment, split-sum --------------------------------
    // desaturate the sky term : the preset ambient is very blue, and using it
    // raw tints every shadowed block blue
    vec3 sky_amb = mix(ambient_irradiance(N), vec3(0.50), 0.45);
    vec3 diffuse_amb = sky_amb * base * (1.0 - metallic) / PI * diffuse_ao * 1.8;

    // reflection : a rough surface sees a blur of its surroundings, which one sample
    // cannot give. bending the reflection toward the normal at least makes it read
    // the right part of the sky, and that is most of what roughness looks like
    vec3 R       = reflect(-V, N);
    vec3 env_dir = normalize(mix(R, N, spec_rough * spec_rough));
    vec3 env_spec = environment(env_dir);

    vec2 ab     = env_brdf(spec_rough, max(NdotV, 0.15)); // tame the grazing rim on rough blocks
    vec3 FssEss = F0 * ab.x + ab.y;

    // energy : a rough metal loses light between its own microfacets and never gets
    // it back. this puts a share of the multiple scattering back (Fdez-Aguera's fit)
    vec3  F_avg  = F0 + (1.0 - F0) / 21.0;
    float Ems    = 1.0 - (ab.x + ab.y);
    vec3  FmsEms = min(Ems * F_avg / max(1.0 - F_avg * Ems, 1e-3), vec3(1.0));

    float spec_ao = ao * pow(vis, 1.4); // reflections are occluded in a fold too
    vec3 specular_amb = (FssEss + FmsEms * multiscatter) * env_spec * spec_ao;
    specular_amb *= mix(0.20, 1.0, 1.0 - spec_rough);
    float edge_spec = clamp(length(dn) * 3.0, 0.0, 1.0);
    specular_amb *= 1.0 - 0.65 * edge_spec; // no bright Fresnel outline on block silhouettes

    vec3 color = direct + diffuse_amb + specular_amb;

    // sky bounce into the shadow side : a fully shadowed block should read as a
    // cool ambient surface, not a black hole. shadow_moon keeps this for the sun
    float shadow_fill = shadow_moon > 0.5 ? 0.0 : (1.0 - sun_vis);
    color += base * vec3(0.14, 0.14, 0.15) * shadow_fill * diffuse_ao;

    // a neutral bounce and a tiny floor : shade stays readable without the blue
    // cast of using the sky colour as a constant fill
    color += base * vec3(0.12) * diffuse_ao;
    color += vec3(0.025) * diffuse_ao;

    // emitters : lit windows, lamp heads, interior panels. only at night, so
    // the same materials read as plain glass or metal in daylight
    color += base * emissive * night * 3.0;

    // --- distance : the world fades into the sky it stands in ----------------
    float dist = length(P - vs_out.view_position);
    color = mix(color, environment(view_dir), 1.0 - exp(-dist * fog_density));

    // linear HDR : local lights add here, water draws over it, then post encodes
    pixel_color = vec4(color, 1.0);
}
