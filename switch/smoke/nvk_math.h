/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * Minimal column-major mat4 math for the NVK validation apps.
 */
#ifndef NVK_MATH_H
#define NVK_MATH_H

#include <math.h>

typedef struct { float m[16]; } mat4;

static inline mat4 mat4_identity(void)
{
   mat4 r = { { 0 } };
   r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
   return r;
}

static inline mat4 mat4_mul(mat4 a, mat4 b)
{
   mat4 r = { { 0 } };
   for (int col = 0; col < 4; col++)
      for (int row = 0; row < 4; row++)
         for (int k = 0; k < 4; k++)
            r.m[col * 4 + row] += a.m[k * 4 + row] * b.m[col * 4 + k];
   return r;
}

static inline mat4 mat4_translate(float x, float y, float z)
{
   mat4 r = mat4_identity();
   r.m[12] = x; r.m[13] = y; r.m[14] = z;
   return r;
}

static inline mat4 mat4_rotate_y(float a)
{
   mat4 r = mat4_identity();
   float c = cosf(a), s = sinf(a);
   r.m[0] = c;  r.m[8] = s;
   r.m[2] = -s; r.m[10] = c;
   return r;
}

static inline mat4 mat4_rotate_x(float a)
{
   mat4 r = mat4_identity();
   float c = cosf(a), s = sinf(a);
   r.m[5] = c;  r.m[9] = -s;
   r.m[6] = s;  r.m[10] = c;
   return r;
}

static inline mat4 mat4_perspective(float fovy, float aspect, float znear, float zfar)
{
   mat4 r = { { 0 } };
   float f = 1.0f / tanf(fovy * 0.5f);
   r.m[0]  = f / aspect;
   r.m[5]  = -f;
   r.m[10] = zfar / (znear - zfar);
   r.m[11] = -1.0f;
   r.m[14] = (zfar * znear) / (znear - zfar);
   return r;
}

#endif /* NVK_MATH_H */
