/**
 * MollenOS
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
 * Standard C Support
 * - Standard Socket IO Implementation
 */

#include <internal/_io.h>
#include <inet/local.h>
#include <errno.h>
#include <string.h>

int getpeername(int iod, struct sockaddr* address_out, socklen_t* address_length_out)
{
    stdio_handle_t* handle = stdio_handle_get(iod);
    
    if (!handle) {
        _set_errno(EBADF);
        return -1;
    }
    
    if (!address_out || !address_length_out) {
        _set_errno(EINVAL);
        return -1;
    }
    
    if (handle->object.type != STDIO_HANDLE_SOCKET) {
        _set_errno(ENOTSOCK);
        return -1;
    }
    
    if (!(handle->object.data.socket.flags & SOCKET_CONNECTED)) {
        _set_errno(ENOTCONN);
        return -1;
    }
    
    if (*address_length_out != 0) {
        memcpy(address_out, &handle->object.data.socket.default_address,
            MIN(*address_length_out, handle->object.data.socket.default_address.__ss_len));
    }
    *address_length_out = handle->object.data.socket.default_address.__ss_len;
    return 0;
}
