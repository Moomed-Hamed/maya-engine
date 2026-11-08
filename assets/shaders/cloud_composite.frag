#version 420 core

// composite the premultiplied cloud target over the lit frame. the caller sets
// GL_ONE / GL_ONE_MINUS_SRC_ALPHA

struct VS_OUT {
	vec2 uv;
	vec3 view_position;
};

in VS_OUT vs_out;

layout (binding = 0) uniform sampler2D cloud_tex;

layout (location = 0) out vec4 pixel_color;

void main()
{
	pixel_color = texture(cloud_tex, vs_out.uv);
}
