/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Shared unit-cube geometry for the 3D validation apps.
 * Eight coloured corners plus a 36-entry index list (12 triangles).
 */
#ifndef NVK_CUBE_H
#define NVK_CUBE_H

#include <stdint.h>

typedef struct { float pos[3]; float col[3]; } cube_vtx;

static const cube_vtx cube_corners[8] = {
   { { -1, -1, -1 }, { 0.1f, 0.1f, 0.1f } },
   { {  1, -1, -1 }, { 1.0f, 0.1f, 0.1f } },
   { {  1,  1, -1 }, { 1.0f, 1.0f, 0.1f } },
   { { -1,  1, -1 }, { 0.1f, 1.0f, 0.1f } },
   { { -1, -1,  1 }, { 0.1f, 0.1f, 1.0f } },
   { {  1, -1,  1 }, { 1.0f, 0.1f, 1.0f } },
   { {  1,  1,  1 }, { 1.0f, 1.0f, 1.0f } },
   { { -1,  1,  1 }, { 0.1f, 1.0f, 1.0f } },
};

static const uint16_t cube_indices[36] = {
   0, 1, 2,  2, 3, 0,   /* back   */
   4, 6, 5,  6, 4, 7,   /* front  */
   4, 5, 1,  1, 0, 4,   /* bottom */
   3, 2, 6,  6, 7, 3,   /* top    */
   4, 0, 3,  3, 7, 4,   /* left   */
   1, 5, 6,  6, 2, 1,   /* right  */
};

#endif /* NVK_CUBE_H */
