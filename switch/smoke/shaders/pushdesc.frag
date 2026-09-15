#version 450
/* Reads every descriptor of a 24-element push descriptor array and reports the
 * first one not holding what the host put there.
 */
#define MAGIC 0x5A17C0DEu

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0, std430) readonly buffer Slot {
   uint word[];
} slot[24];

layout(push_constant) uniform PC {
   uint base;
   uint out_index;
} pc;

#define CHK(i) \
   if (bad == 0u && slot[i].word[0] != (MAGIC ^ (pc.base + uint(i)))) \
      bad = uint(i) + 1u;

void main()
{
   uint bad = 0u;
   CHK(0)  CHK(1)  CHK(2)  CHK(3)  CHK(4)  CHK(5)  CHK(6)  CHK(7)
   CHK(8)  CHK(9)  CHK(10) CHK(11) CHK(12) CHK(13) CHK(14) CHK(15)
   CHK(16) CHK(17) CHK(18) CHK(19) CHK(20) CHK(21) CHK(22) CHK(23)

   o_color = vec4(float(bad) / 255.0, bad == 0u ? 1.0 : 0.0, 0.0, 1.0);
}
