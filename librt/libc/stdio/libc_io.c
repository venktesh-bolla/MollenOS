/* MollenOS
 *
 * Copyright 2017, Philip Meulengracht
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
 * C Standard Library
 * - Standard IO Support functions
 */

#ifdef LIBC_KERNEL
#include <os/spinlock.h>
#include <threading.h>
#include <threads.h>
#include <stdio.h>

spinlock_t __GlbPrintLock = _SPN_INITIALIZER_NP(spinlock_plain);
FILE __GlbStdout = { 0 }, __GlbStdin = { 0 }, __GlbStderr = { 0 };

OsStatus_t
_lock_file(
    _In_ FILE *file)
{
    if (!(file->_flag & _IOSTRG)) {
        spinlock_acquire(&__GlbPrintLock);
    }
    return OsSuccess;
}

OsStatus_t
_unlock_file(
    _In_ FILE *file)
{
    if (!(file->_flag & _IOSTRG)) {
        spinlock_release(&__GlbPrintLock);
    }
    return OsSuccess;
}

FILE *
stdio_get_std(
    _In_ int n)
{
    switch (n) {
        case STDOUT_FILENO: {
            return &__GlbStdout;
        }
        case STDIN_FILENO: {
            return &__GlbStdin;
        }
        case STDERR_FILENO: {
            return &__GlbStderr;
        }
        default: {
            return NULL;
        }
    }
}

int wctomb(char *mbchar, wchar_t wchar)
{
    _CRT_UNUSED(mbchar);
    _CRT_UNUSED(wchar);
    return 0;
}

thrd_t thrd_current(void) {
    return (thrd_t)GetCurrentThreadId();
}

#else
//#define __TRACE
#include <assert.h>
#include <ddk/handle.h>
#include <ddk/utils.h>
#include <ds/collection.h>
#include <errno.h>
#include <internal/_syscalls.h>
#include <internal/_io.h>
#include <io.h>
#include <ctt_input_protocol.h>
#include <os/keycodes.h>
#include <os/mollenos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Collection_t stdio_objects = COLLECTION_INIT(KeyInteger); // TODO hashtable
static FILE         __GlbStdout = { 0 }, __GlbStdin = { 0 }, __GlbStderr = { 0 };

/* StdioIsHandleInheritable
 * Returns whether or not the handle should be inheritted by sub-processes based on the requested
 * startup information and the handle settings. */
static OsStatus_t
StdioIsHandleInheritable(
    _In_ ProcessConfiguration_t* Configuration,
    _In_ stdio_handle_t*         Object)
{
    OsStatus_t Status = OsSuccess;

    if (Object->wxflag & WX_DONTINHERIT) {
        Status = OsError;
    }

    // If we didn't request to inherit one of the handles, then we don't account it
    // for being the one requested.
    if (Object->fd == Configuration->StdOutHandle && 
        !(Configuration->InheritFlags & PROCESS_INHERIT_STDOUT)) {
        Status = OsError;
    }
    else if (Object->fd == Configuration->StdInHandle && 
        !(Configuration->InheritFlags & PROCESS_INHERIT_STDIN)) {
        Status = OsError;
    }
    else if (Object->fd == Configuration->StdErrHandle && 
        !(Configuration->InheritFlags & PROCESS_INHERIT_STDERR)) {
        Status = OsError;
    }
    else if (!(Configuration->InheritFlags & PROCESS_INHERIT_FILES)) {
        if (Object->fd != Configuration->StdOutHandle &&
            Object->fd != Configuration->StdInHandle &&
            Object->fd != Configuration->StdErrHandle) {
            Status = OsError;
        }
    }
    return Status;
}

static size_t
StdioGetNumberOfInheritableHandles(
    _In_ ProcessConfiguration_t* Configuration)
{
    size_t NumberOfFiles = 0;
    LOCK_FILES();
    foreach(Node, &stdio_objects) {
        stdio_handle_t* Object = (stdio_handle_t*)Node->Data;
        if (StdioIsHandleInheritable(Configuration, Object) == OsSuccess) {
            NumberOfFiles++;
        }
    }
    UNLOCK_FILES();
    return NumberOfFiles;
}

static OsStatus_t
StdioCreateInheritanceBlock(
    _In_  ProcessConfiguration_t* Configuration,
    _Out_ void**                  InheritationBlockOut,
    _Out_ size_t*                 InheritationBlockLengthOut)
{
    stdio_inheritation_block_t* InheritationBlock;
    size_t                      NumberOfObjects;
    int                         i = 0;

    assert(Configuration != NULL);

    if (Configuration->InheritFlags == PROCESS_INHERIT_NONE) {
        return OsSuccess;
    }
    
    NumberOfObjects = StdioGetNumberOfInheritableHandles(Configuration);
    if (NumberOfObjects != 0) {
        size_t InheritationBlockLength;
        
        InheritationBlockLength = sizeof(stdio_inheritation_block_t) + NumberOfObjects * sizeof(stdio_handle_t);
        InheritationBlock       = (stdio_inheritation_block_t*)malloc(InheritationBlockLength);
        if (!InheritationBlock) {
            return OsOutOfMemory;
        }
        
        InheritationBlock->handle_count = NumberOfObjects;
        
        LOCK_FILES();
        foreach(Node, &stdio_objects) {
            stdio_handle_t* Object = (stdio_handle_t*)Node->Data;
            if (StdioIsHandleInheritable(Configuration, Object) == OsSuccess) {
                memcpy(&InheritationBlock->handles[i], Object, sizeof(stdio_handle_t));
                
                // Check for this fd to be equal to one of the custom handles
                // if it is equal, we need to update the fd of the handle to our reserved
                if (Object->fd == Configuration->StdOutHandle) {
                    InheritationBlock->handles[i].fd = STDOUT_FILENO;
                }
                if (Object->fd == Configuration->StdInHandle) {
                    InheritationBlock->handles[i].fd = STDIN_FILENO;
                }
                if (Object->fd == Configuration->StdErrHandle) {
                    InheritationBlock->handles[i].fd = STDERR_FILENO;
                }
                i++;
            }
        }
        UNLOCK_FILES();
        
        *InheritationBlockOut       = (void*)InheritationBlock;
        *InheritationBlockLengthOut = InheritationBlockLength;
    }
    return OsSuccess;
}

static void
StdioInheritObject(
    _In_ stdio_handle_t* InheritHandle)
{
    stdio_handle_t* handle;
    int             status;
    
    status = stdio_handle_create(InheritHandle->fd, InheritHandle->wxflag | WX_INHERITTED, &handle);
    if (!status) {
        if (handle->fd == STDOUT_FILENO) {
            __GlbStdout._fd = handle->fd;
        }
        else if (handle->fd == STDIN_FILENO) {
            __GlbStdin._fd = handle->fd;
        }
        else if (handle->fd == STDERR_FILENO) {
            __GlbStderr._fd = handle->fd;
        }
        stdio_handle_set_handle(handle, InheritHandle->object.handle);
        stdio_handle_set_ops_type(handle, InheritHandle->object.type);
        if (handle->ops.inherit(handle) != OsSuccess) {
            WARNING(" > failed to inherit fd %i", InheritHandle->fd);
            stdio_handle_destroy(handle, 0);
        }
    }
    else {
        WARNING(" > failed to create inheritted handle with fd %i", InheritHandle->fd);
    }
}

void StdioConfigureStandardHandles(
    _In_ void* inheritanceBlock)
{
    stdio_inheritation_block_t* block = inheritanceBlock;
    stdio_handle_t*             handle_out;
    stdio_handle_t*             handle_in;
    stdio_handle_t*             handle_err;
    int                         i;
    TRACE("[libc] [parse_inherit] 0x%" PRIxIN, block);
    
    // Handle inheritance
    if (block != NULL) {
        TRACE("[libc] [parse_inherit] handle count %i", block->handle_count);
        for (i = 0; i < block->handle_count; i++) {
            StdioInheritObject(&block->handles[i]);
        }
    }

    // Make sure all default handles have been set for std. The operations for
    // stdout and stderr will be null operations, as no output has been specified
    // for this process. If the process wants to get output it must reopen the
    // stdout/stderr handles.
    handle_out = stdio_handle_get(STDOUT_FILENO);
    if (handle_out == NULL) {
        stdio_handle_create(STDOUT_FILENO, WX_DONTINHERIT, &handle_out);
    }

    handle_in = stdio_handle_get(STDIN_FILENO);
    if (handle_in == NULL) {
        stdio_handle_create(STDIN_FILENO, WX_DONTINHERIT, &handle_in);
    }
    
    handle_err = stdio_handle_get(STDERR_FILENO);
    if (handle_err == NULL) {
        stdio_handle_create(STDERR_FILENO, WX_DONTINHERIT, &handle_err);
    }
    
    stdio_handle_set_buffered(handle_out, &__GlbStdout, _IOWRT);
    stdio_handle_set_buffered(handle_in,  &__GlbStdin,  _IOREAD);
    stdio_handle_set_buffered(handle_err, &__GlbStderr, _IOWRT);
}

static int
stdio_close_all_handles(void)
{
    stdio_handle_t* handle;
    int             files_closed = 0;
    
    LOCK_FILES();
    while (CollectionBegin(&stdio_objects) != NULL) {
        CollectionItem_t* Node = CollectionBegin(&stdio_objects);
        handle = (stdio_handle_t*)Node->Data;
        
        // Is it a buffered stream or raw?
        if (handle->buffered_stream) {
            fclose(handle->buffered_stream);
        }
        else {
            close(handle->fd);
        }
        files_closed++;
    }
    UNLOCK_FILES();
    return files_closed;
}

void StdioInitialize(void)
{
    stdio_bitmap_initialize();
}

_CRTIMP void
StdioCleanup(void)
{
    // Flush all file buffers and close handles
    os_flush_all_buffers(_IOWRT | _IOREAD);
    stdio_close_all_handles();
}

int stdio_handle_create(int fd, int flags, stdio_handle_t** handle_out)
{
    stdio_handle_t* handle;
    DataKey_t       key;
    int             updated_fd;

    // the bitmap allocator handles both cases if we want to allocate a specific
    // or just the first free fd
    updated_fd = stdio_bitmap_allocate(fd);
    if (updated_fd == -1) {
        _set_errno(EMFILE);
        return -1;
    }

    handle = (stdio_handle_t*)malloc(sizeof(stdio_handle_t));
    if (!handle) {
        _set_errno(ENOMEM);
        return -1;
    }
    memset(handle, 0, sizeof(stdio_handle_t));
    
    handle->fd            = updated_fd;
    handle->object.handle = UUID_INVALID;
    handle->object.type   = STDIO_HANDLE_INVALID;
    
    handle->wxflag          = WX_OPEN | flags;
    handle->lookahead[0]    = '\n';
    handle->lookahead[1]    = '\n';
    handle->lookahead[2]    = '\n';
    spinlock_init(&handle->lock, spinlock_recursive);
    stdio_get_null_operations(&handle->ops);

    key.Value.Integer = updated_fd;
    CollectionAppend(&stdio_objects, CollectionCreateNode(key, handle));
    TRACE("[stdio_handle_create] success %i", updated_fd);
    
    *handle_out = handle;
    return EOK;
}

int stdio_handle_set_handle(stdio_handle_t* handle, UUId_t io_handle)
{
    if (!handle) {
        return EBADF;
    }
    handle->object.handle = io_handle;
    return EOK;
}

int stdio_handle_set_ops_type(stdio_handle_t* handle, int type)
{
    if (!handle) {
        return EBADF;
    }
    
    // Get io operations
    handle->object.type = type;
    switch (type) {
        case STDIO_HANDLE_PIPE: {
            stdio_get_pipe_operations(&handle->ops);
        } break;
        case STDIO_HANDLE_FILE: {
            stdio_get_file_operations(&handle->ops);
        } break;
        case STDIO_HANDLE_SOCKET: {
            stdio_get_net_operations(&handle->ops);
        } break;
        case STDIO_HANDLE_IPCONTEXT: {
            stdio_get_ipc_operations(&handle->ops);
        } break;
        case STDIO_HANDLE_SET: {
            stdio_get_set_operations(&handle->ops);
        } break;
        case STDIO_HANDLE_EVENT: {
            stdio_get_evt_operations(&handle->ops);
        } break;
        
        default: {
            stdio_get_null_operations(&handle->ops);
        } break;
    }
    return EOK;
}

int stdio_handle_set_buffered(stdio_handle_t* handle, FILE* stream, unsigned int stream_flags)
{
    if (!handle) {
        return EBADF;
    }
    
    if (!stream) {
        stream = (FILE*)malloc(sizeof(FILE));
        if (!stream) {
            return ENOMEM;
        }
    }
    
    // Reset the stream structure
    stream->_ptr      = stream->_base = NULL;
    stream->_cnt      = 0;
    stream->_fd       = handle->fd;
    stream->_flag     = stream_flags;
    stream->_tmpfname = NULL;
    
    // associate the stream object
    handle->buffered_stream = stream;
    return EOK;
}

int stdio_handle_destroy(stdio_handle_t* handle, int flags)
{
    DataKey_t key;
    
    if (!handle) {
        return EBADF;
    }
    
    key.Value.Integer = handle->fd;
    CollectionRemoveByKey(&stdio_objects, key);
    stdio_bitmap_free(handle->fd);
    free(handle);
    return EOK;
}

int stdio_handle_activity(stdio_handle_t* handle , int activity)
{
    OsStatus_t status = handle_post_notification(handle->object.handle, activity);
    if (status != OsSuccess) {
        OsStatusToErrno(status);
        return -1;
    }
    return 0;
}

stdio_handle_t* stdio_handle_get(int iod)
{
    DataKey_t Key = { .Value.Integer = iod };
    return (stdio_handle_t*)CollectionGetDataByKey(&stdio_objects, Key, 0);
}

FILE* stdio_get_std(int n)
{
    switch (n) {
        case STDOUT_FILENO: {
            return &__GlbStdout;
        }
        case STDIN_FILENO: {
            return &__GlbStdin;
        }
        case STDERR_FILENO: {
            return &__GlbStderr;
        }
        default: {
            return NULL;
        }
    }
}

int isatty(int fd)
{
    stdio_handle_t* handle = stdio_handle_get(fd);
    if (!handle) {
        return EBADF;
    }
    return handle->wxflag & WX_TTY;
}

Collection_t* stdio_get_handles(void)
{
    return &stdio_objects;
}


UUId_t GetNativeHandle(int iod)
{
    stdio_handle_t* handle = stdio_handle_get(iod);
    if (!handle) {
        return UUID_INVALID;
    }
    return handle->object.handle;
}

extern void GetKeyFromSystemKeyEnUs(struct ctt_input_button_event*);

/* TranslateSystemKey
 * Performs the translation on the keycode in the system key structure. This fills
 * in the <KeyUnicode> and <KeyAscii> members by translation of the active keymap. */
OsStatus_t
TranslateSystemKey(
    _In_ struct ctt_input_button_event* key)
{
    if (key->key_code != VK_INVALID) {
        GetKeyFromSystemKeyEnUs(key);
        return OsSuccess;
    }
    return OsError;
}

#endif
