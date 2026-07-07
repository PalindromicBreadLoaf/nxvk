#version 450
/* Transform a (position, colour) vertex by a push-constant MVP. */
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_color;

layout(location = 0) out vec3 v_color;

layout(push_constant) uniform PC {
   mat4 mvp;
   vec4 tint;
} pc;

void main()
{
   gl_Position = pc.mvp * vec4(a_pos, 1.0);
   v_color = a_color;
}
