/**
 * MollenOS
 *
 * Copyright 2011, Philip Meulengracht
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
 * X86-32 Virtual Memory Manager
 * - Contains the implementation of virtual memory management
 *   for the X86-32 Architecture 
 */

#define __MODULE "MEM1"
//#define __TRACE
#define __COMPILE_ASSERT

#include <arch.h>
#include <assert.h>
#include <handle.h>
#include <heap.h>
#include <debug.h>
#include <machine.h>
#include <memory.h>
#include <memoryspace.h>
#include <string.h>

extern void memory_reload_cr3(void);

// Disable the atomic wrong alignment, as they are aligned and are sanitized
// by the static assert
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Watomic-alignment"
#endif

PageDirectory_t*
MmVirtualGetMasterTable(
    _In_  SystemMemorySpace_t* MemorySpace,
    _In_  VirtualAddress_t     Address,
    _Out_ PageDirectory_t**    ParentDirectory,
    _Out_ int*                 IsCurrent)
{
    PageDirectory_t* Directory = (PageDirectory_t*)MemorySpace->Data[MEMORY_SPACE_DIRECTORY];
    PageDirectory_t* Parent    = NULL;

	assert(Directory != NULL);

    // If there is no parent then we ignore it as we don't have to synchronize with kernel directory.
    // We always have the shared page-tables mapped. The address must be below the thread-specific space
    if (MemorySpace->ParentHandle != UUID_INVALID) {
        if (Address < MEMORY_LOCATION_RING3_THREAD_START) {
            SystemMemorySpace_t* MemorySpaceParent = (SystemMemorySpace_t*)LookupHandleOfType(
                MemorySpace->ParentHandle, HandleTypeMemorySpace);
            Parent = (PageDirectory_t*)MemorySpaceParent->Data[MEMORY_SPACE_DIRECTORY];
        }
    }
    *IsCurrent       = (MemorySpace == GetCurrentMemorySpace()) ? 1 : 0;
    *ParentDirectory = Parent;
    return Directory;
}

PageTable_t*
MmVirtualGetTable(
    _In_ PageDirectory_t* ParentPageDirectory,
    _In_ PageDirectory_t* PageDirectory,
    _In_ uintptr_t        Address,
    _In_ int              IsCurrent,
    _In_ int              CreateIfMissing,
    _Out_ int*            Update)
{
    PageTable_t* Table          = NULL;
    int          PageTableIndex = PAGE_DIRECTORY_INDEX(Address);
    uint32_t     ParentMapping;
    uint32_t     Mapping;
    int          Result;

    // Load the entry from the table
    Mapping = atomic_load(&PageDirectory->pTables[PageTableIndex]);
    *Update = 0; // Not used on x32, only 64

    // Sanitize PRESENT status
	if (Mapping & PAGE_PRESENT) {
        Table = (PageTable_t*)PageDirectory->vTables[PageTableIndex];
	    assert(Table != NULL);
	}
    else {
        // Table not present, before attemping to create, sanitize parent
        ParentMapping = 0;
        if (ParentPageDirectory != NULL) {
            ParentMapping = atomic_load_explicit(&ParentPageDirectory->pTables[PageTableIndex],
                memory_order_acquire);
        }

SyncWithParent:
        // Check the parent-mapping
        if (ParentMapping & PAGE_PRESENT) {
            // Update our page-directory and reload
            atomic_store_explicit(&PageDirectory->pTables[PageTableIndex], 
                ParentMapping | PAGETABLE_INHERITED, memory_order_release);
            PageDirectory->vTables[PageTableIndex] = ParentPageDirectory->vTables[PageTableIndex];
            
            // By performing an immediate readback we ensure changes
            Table = (PageTable_t*)PageDirectory->vTables[PageTableIndex];
            assert(Table != NULL);
        }
        else if (CreateIfMissing) {
            // Allocate, do a CAS and see if it works, if it fails retry our operation
            uintptr_t TablePhysical;
            Table = (PageTable_t*)kmalloc_p(PAGE_SIZE, &TablePhysical);
            if (!Table) {
                return NULL;
            }
            
            memset((void*)Table, 0, sizeof(PageTable_t));
            TablePhysical |= PAGE_PRESENT | PAGE_WRITE;
            if (Address > MEMORY_LOCATION_KERNEL_END) {
                TablePhysical |= PAGE_USER;
            }

            // Now perform the synchronization
            if (ParentPageDirectory != NULL) {
                Result = atomic_compare_exchange_strong(&ParentPageDirectory->pTables[PageTableIndex], 
                    &ParentMapping, TablePhysical);
                if (!Result) {
                    // Start over as someone else beat us to the punch
                    kfree((void*)Table);
                    goto SyncWithParent;
                }
                
                // Ok we just transferred successfully, mark our copy inheritted
                ParentPageDirectory->vTables[PageTableIndex] = (uint32_t)Table;
                TablePhysical |= PAGETABLE_INHERITED;
            }

            // Update our copy
            atomic_store(&PageDirectory->pTables[PageTableIndex], TablePhysical);
            PageDirectory->vTables[PageTableIndex] = (uintptr_t)Table;
        }

		// Reload CR3 directory to force the MMIO to see our changes 
		if (IsCurrent) {
			memory_reload_cr3();
		}
    }
    return Table;
}

OsStatus_t
CloneVirtualSpace(
    _In_ SystemMemorySpace_t*   MemorySpaceParent, 
    _In_ SystemMemorySpace_t*   MemorySpace,
    _In_ int                    Inherit)
{
    PageDirectory_t* SystemDirectory = (PageDirectory_t*)GetDomainMemorySpace()->Data[MEMORY_SPACE_DIRECTORY];
    PageDirectory_t* ParentDirectory = NULL;
    PageDirectory_t* PageDirectory;
    uintptr_t        PhysicalAddress;
    int i;

    // Lookup which table-region is the stack region
    int ThreadRegion    = PAGE_DIRECTORY_INDEX(MEMORY_LOCATION_RING3_THREAD_START);
    int ThreadRegionEnd = PAGE_DIRECTORY_INDEX(MEMORY_LOCATION_RING3_THREAD_END);

    PageDirectory = (PageDirectory_t*)kmalloc_p(sizeof(PageDirectory_t), &PhysicalAddress);
    if (!PageDirectory) {
        return OsOutOfMemory;
    }
    
    memset(PageDirectory, 0, sizeof(PageDirectory_t));

    // Determine parent
    if (MemorySpaceParent != NULL) {
        ParentDirectory = (PageDirectory_t*)MemorySpaceParent->Data[MEMORY_SPACE_DIRECTORY];
    }

    // Initialize base mappings
    for (i = 0; i < ENTRIES_PER_PAGE; i++) {
        uint32_t KernelMapping, CurrentMapping;

        // Sanitize stack region, never copy
        if (i >= ThreadRegion && i <= ThreadRegionEnd) {
            continue;
        }

        // Sanitize if it's inside kernel region
        if (SystemDirectory->vTables[i] != 0) {
            // Update the physical table
            KernelMapping = atomic_load_explicit(&SystemDirectory->pTables[i], memory_order_acquire);
            atomic_store_explicit(&PageDirectory->pTables[i], KernelMapping | PAGETABLE_INHERITED,
                memory_order_release);

            // Copy virtual
            PageDirectory->vTables[i] = SystemDirectory->vTables[i];
            continue;
        }

        // Inherit? We must mark that table inherited to avoid
        // it being freed again
        if (Inherit && ParentDirectory != NULL) {
            CurrentMapping = atomic_load(&ParentDirectory->pTables[i]);
            if (CurrentMapping & PAGE_PRESENT) {
                atomic_store(&PageDirectory->pTables[i], CurrentMapping | PAGETABLE_INHERITED);
                PageDirectory->vTables[i] = ParentDirectory->vTables[i];
            }
        }
    }

    // Update the configuration data for the memory space
	MemorySpace->Data[MEMORY_SPACE_CR3]       = PhysicalAddress;
	MemorySpace->Data[MEMORY_SPACE_DIRECTORY] = (uintptr_t)PageDirectory;

    // Create new resources for the happy new parent :-)
    if (MemorySpaceParent == NULL) {
        MemorySpace->Data[MEMORY_SPACE_IOMAP] = (uintptr_t)kmalloc(GDT_IOMAP_SIZE);
        if (MemorySpace->Flags & MEMORY_SPACE_APPLICATION) {
            memset((void*)MemorySpace->Data[MEMORY_SPACE_IOMAP], 0xFF, GDT_IOMAP_SIZE);
        }
        else {
            memset((void*)MemorySpace->Data[MEMORY_SPACE_IOMAP], 0, GDT_IOMAP_SIZE);
        }
    }
    return OsSuccess;
}

OsStatus_t
DestroyVirtualSpace(
    _In_ SystemMemorySpace_t* SystemMemorySpace)
{
    PageDirectory_t* Pd = (PageDirectory_t*)SystemMemorySpace->Data[MEMORY_SPACE_DIRECTORY];
    int              i, j;

    // Iterate page-mappings
    for (i = 0; i < ENTRIES_PER_PAGE; i++) {
        PageTable_t *Table;
        uint32_t CurrentMapping;

        // Do some initial checks on the virtual member to avoid atomics
        // If it's empty or if it's a kernel page table ignore it
        if (Pd->vTables[i] == 0) {
            continue;
        }

        // Load the mapping, then perform checks for inheritation or a system
        // mapping which is done by kernel page-directory
        CurrentMapping = atomic_load_explicit(&Pd->pTables[i], memory_order_relaxed);
        if ((CurrentMapping & PAGETABLE_INHERITED) || !(CurrentMapping & PAGE_PRESENT)) {
            continue;
        }

        // Iterate pages in table
        Table = (PageTable_t*)Pd->vTables[i];
        for (j = 0; j < ENTRIES_PER_PAGE; j++) {
            CurrentMapping = atomic_load_explicit(&Table->Pages[j], memory_order_relaxed);
            if ((CurrentMapping & PAGE_PERSISTENT) || !(CurrentMapping & PAGE_PRESENT)) {
                continue;
            }

            // If it has a mapping - free it
            if ((CurrentMapping & PAGE_MASK) != 0) {
                CurrentMapping &= PAGE_MASK;
                IrqSpinlockAcquire(&GetMachine()->PhysicalMemoryLock);
                bounded_stack_push(&GetMachine()->PhysicalMemory, (void*)CurrentMapping);
                IrqSpinlockRelease(&GetMachine()->PhysicalMemoryLock);
            }
        }
        kfree(Table);
    }
    kfree(Pd);

    // Free the resources allocated specifically for this
    if (SystemMemorySpace->ParentHandle == UUID_INVALID) {
        kfree((void*)SystemMemorySpace->Data[MEMORY_SPACE_IOMAP]);
    }
    return OsSuccess;
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
