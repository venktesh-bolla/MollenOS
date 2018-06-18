/* MollenOS
 *
 * Copyright 2011 - 2017, Philip Meulengracht
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
 * MollenOS Synchronization
 * - Counting semaphores implementation, using safe passages known as
 *   atomic sections in the operating system to synchronize in a kernel env
 */

#ifndef _MCORE_SEMAPHORE_H_
#define _MCORE_SEMAPHORE_H_

#include <os/osdefs.h>
#include <atomicsection.h>
#include <ds/mstring.h>

/* The semaphore structure, contains
 * an internal safe-passge lock, a creator id
 * and a Hash (for global support), and the current value */
typedef struct _Semaphore {
    AtomicSection_t     SyncObject;
	UUId_t              Creator;
	size_t              Hash;
	int                 Value;
    int                 MaxValue;
    int                 Cleanup;
} Semaphore_t;

/* SemaphoreCreate
 * Initializes and allocates a new semaphore
 * Semaphores use safe passages to avoid race-conditions */
KERNELAPI Semaphore_t* KERNELABI
SemaphoreCreate(
    _In_ int            InitialValue,
    _In_ int            MaximumValue);

/* SemaphoreCreateGlobal
 * Creates a global semaphore, identified by it's name
 * and makes sure only one can exist at the time. Returns
 * NULL if one already exists. */
KERNELAPI Semaphore_t* KERNELABI
SemaphoreCreateGlobal(
    _In_ MString_t*     Identifier, 
    _In_ int            InitialValue,
    _In_ int            MaximumValue);

/* SemaphoreConstruct
 * Constructs an already allocated semaphore and resets
 * it's value to the given initial value */
KERNELAPI void KERNELABI
SemaphoreConstruct(
    _In_ Semaphore_t*   Semaphore,
    _In_ int            InitialValue,
    _In_ int            MaximumValue);

/* SemaphoreDestroy
 * Destroys and frees a semaphore, releasing any
 * resources associated with it */
KERNELAPI void KERNELABI
SemaphoreDestroy(
    _In_ Semaphore_t*   Semaphore);

/* SemaphoreWait
 * Waits for the semaphore signal with the optional time-out.
 * Returns SCHEDULER_SLEEP_OK or SCHEDULER_SLEEP_TIMEOUT */
KERNELAPI int KERNELABI
SemaphoreWait(
    _In_ Semaphore_t*   Semaphore,
    _In_ size_t         Timeout);

/* SemaphoreSignal
 * Signals the semaphore with the given value, default is 1 */
KERNELAPI OsStatus_t KERNELABI
SemaphoreSignal(
    _In_ Semaphore_t*   Semaphore,
    _In_ int            Value);

#endif // !_MCORE_SEMAPHORE_H_
