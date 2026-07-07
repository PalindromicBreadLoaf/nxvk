#version 450
layout(location = 0) in vec3 a_pos;

layout(location = 0) out vec3 v_dir;

layout(push_constant) uniform PC {
   mat4 mvp;
   vec4 tint;
} pc;

void main()
{
   gl_Position = pc.mvp * vec4(a_pos, 1.0);
   v_dir = a_pos;
}
