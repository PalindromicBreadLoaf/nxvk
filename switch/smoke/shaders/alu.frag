#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main()
{
   vec4 x = vec4(uv, uv.x + 0.17, uv.y + 0.31);
   for (int i = 0; i < 64; i++)
      x = fract(x * vec4(1.013, 1.017, 1.019, 1.023) + x.yzwx);
   out_color = x;
}
