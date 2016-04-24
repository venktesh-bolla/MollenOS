/* MollenOS
*
* Copyright 2011 - 2016, Philip Meulengracht
*
* This program is free software : you can redistribute it and / or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation ? , either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.If not, see <http://www.gnu.org/licenses/>.
*
*
* MollenOS C Library - SetJmp & LongJmp
*/

#ifndef __SETJMP_INC__
#define __SETJMP_INC__

/* Includes */
#include <crtdefs.h>
#include <stdint.h>

/* Definitions */
#define _JBLEN  24
#define _JBTYPE int
typedef _JBTYPE jmp_buf[_JBLEN];

/* C++ Guard */
#ifdef __cplusplus
extern "C" {
#endif

/* Prototypes */

/* The save in time -> jump */
_CRT_EXTERN int _setjmp(jmp_buf env);
_CRT_EXTERN int _setjmp3(jmp_buf env, int nb_args, ...);

/* Shorthand */
#define setjmp(env) _setjmp(env)

/* Restore time-state -> jmp */
_CRT_EXTERN void longjmp(jmp_buf env, int value);

#ifdef __cplusplus
}
#endif

#endif //!__SETJMP_INC__
