#version 450
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

void main()
{
   o_color = texture(u_tex, v_uv);
}
