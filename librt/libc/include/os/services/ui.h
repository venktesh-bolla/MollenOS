/* MollenOS
 *
 * Copyright 2019, Philip Meulengracht
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
 * Ui Service Definitions & Structures
 * - This header describes the base ui-structure, prototypes
 *   and functionality, refer to the individual things for descriptions
 */

#ifndef __SERVICES_UI_H__
#define __SERVICES_UI_H__

#include <os/osdefs.h>

typedef enum {
    SurfaceRGBA,
} UISurfaceFormat_t;

typedef struct {
    int x, y;
    int w, h;
} Rect_t;

typedef struct {
    Rect_t            Dimensions;
    UISurfaceFormat_t Format;
} UISurfaceDescriptor_t;

typedef struct {
    UISurfaceDescriptor_t Surface;
    unsigned              Flags;
    UUId_t                InputPipeHandle;
    UUId_t                WmEventPipeHandle;
} UIWindowParameters_t;

_CODE_BEGIN
/* UiParametersSetDefault
 * Set(s) default window parameters for the given window param structure. */
CRTDECL(void,
UiParametersSetDefault(
    _In_  UIWindowParameters_t* Descriptor));

/* UiRegisterWindow
 * Registers a new window with the window manage with the given 
 * configuration. If the configuration is invalid, OsError is returned. */
CRTDECL(OsStatus_t,
UiRegisterWindow(
    _In_  UIWindowParameters_t* Descriptor,
    _Out_ void**                WindowBuffer));

/* UiSwapBackbuffer
 * Presents the current backbuffer and rendering all changes made to the window. */
CRTDECL(OsStatus_t,
UiSwapBackbuffer(void));
_CODE_END

#endif // !__SERVICES_UI_H__