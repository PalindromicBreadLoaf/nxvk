#version 450
/* Interpolated vertex colour modulated by a push-constant tint. */
layout(location = 0) in  vec3 v_color;
layout(location = 0) out vec4 o_color;

layout(push_constant) uniform PC {
   mat4 mvp;
   vec4 tint;
} pc;

void main()
{
   o_color = vec4(v_color * pc.tint.rgb, pc.tint.a);
}
