#version 450
layout(location = 0) in  vec3 v_dir;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform samplerCube u_cube;

void main()
{
   o_color = texture(u_cube, normalize(v_dir));
}
