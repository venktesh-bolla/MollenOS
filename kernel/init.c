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
 * MollenOS Common Entry Point
 */

/* Includes */
#include <revision.h>
#include <MollenOS.h>
#include <Arch.h>
#include <AcpiInterface.h>
#include <GarbageCollector.h>
#include <DeviceManager.h>
#include <modules/modules.h>
#include <process/phoenix.h>
#include <Scheduler.h>
#include <Threading.h>
#include <Vfs\Vfs.h>
#include <Heap.h>
#include <Log.h>

/* C-Library */
#include <stddef.h>

/* Print Header Information */
void PrintHeader(MCoreBootInfo_t *BootInfo)
{
	Log("MollenOS - Platform: %s - Version %i.%i.%i",
		ARCHITECTURE_NAME, REVISION_MAJOR, REVISION_MINOR, REVISION_BUILD);
	Log("Written by Philip Meulengracht, Copyright 2011-2016.");
	Log("Bootloader - %s", BootInfo->BootloaderName);
	Log("VC Build %s - %s\n", BUILD_DATE, BUILD_TIME);
}

/* * 
 * Shared Entry in MollenOS
 * */
void MCoreInitialize(MCoreBootInfo_t *BootInfo)
{
	/* Initialize Log */
	LogInit();

	/* Print Header */
	PrintHeader(BootInfo);

	/* Init HAL */
	BootInfo->InitHAL(BootInfo->ArchBootInfo, &BootInfo->Descriptor);

	/* Init the heap */
	HeapInit();

	/* The first memory operaiton we will
	 * be performing is upgrading the log away
	 * from the static buffer */
	LogUpgrade(LOG_PREFFERED_SIZE);

	/* We want to initialize IoSpaces as soon
	 * as possible so devices and systems 
	 * can register/claim their io-spaces */
	IoSpaceInit();

	/* Parse the ramdisk early, but we don't run
	 * servers yet, this is not needed, but since there
	 * is no dependancies yet, just do it */
	if (ModulesInit(&BootInfo->Descriptor) != OsNoError) {
		Idle();
	}

	/* Init Threading & Scheduler for boot cpu */
	SchedulerInit(0);
	ThreadingInit();

	/* Now we can do some early ACPI
	 * initialization if ACPI is present
	 * on this system */
	AcpiEnumerate();

	/* Now we initialize some systems that 
	 * rely on the presence of ACPI tables
	 * or the absence of them */
	BootInfo->InitPostSystems();

	/* Now we finish the ACPI setup IF 
	 * ACPI is present on the system */
	if (AcpiAvailable() == ACPI_AVAILABLE) {
		AcpiInitialize();
		AcpiScan();
	}

	/* Initialize the GC 
	 * It recycles threads, ashes and 
	 * keeps the heap clean ! */
	GcInit();

	/* STOP
	 * - Micro Conversion */

	/* Virtual Filesystem */
	VfsInit();

	/* Phoenix */
	PhoenixInit();

	/* Enter Idle Loop */
	while (1)
		Idle();
}