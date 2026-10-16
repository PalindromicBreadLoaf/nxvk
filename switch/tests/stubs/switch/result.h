/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TEST_SWITCH_RESULT_H
#define TEST_SWITCH_RESULT_H
#define R_VALUE(res) ((res) & 0x3FFFFFu)
#define R_SUCCEEDED(res) (R_VALUE(res) == 0)
#define R_FAILED(res) (!R_SUCCEEDED(res))
#endif
