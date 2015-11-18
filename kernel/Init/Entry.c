/* MollenOS
*
* Copyright 2011 - 2014, Philip Meulengracht
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
#include <Devices/Cpu.h>
#include <Devices/Video.h>
#include <DeviceManager.h>
#include <Modules/ModuleManager.h>
#include <Scheduler.h>
#include <Threading.h>
#include <Vfs\Vfs.h>
#include <Heap.h>
#include <Log.h>

/* Globals */
MCoreCpuDevice_t BootCpu = { 0 };
MCoreVideoDevice_t BootVideo = { 0 };

/* Print Header Information */
void PrintHeader(MCoreBootInfo_t *BootInfo)
{
	Log("MollenOS Operating System - Platform: %s - Version %i.%i.%i\n",
		ARCHITECTURE_NAME, REVISION_MAJOR, REVISION_MINOR, REVISION_BUILD);
	Log("Written by Philip Meulengracht, Copyright 2011-2014, All Rights Reserved.\n");
	Log("Bootloader - %s\n", BootInfo->BootloaderName);
	Log("VC Build %s - %s\n\n", BUILD_DATE, BUILD_TIME);
}

/* Shared Entry in MollenOS
 * */
void MCoreInitialize(MCoreBootInfo_t *BootInfo)
{
	/* Initialize Cpu */
	CpuInit(&BootCpu, BootInfo->ArchBootInfo);

	/* Setup Video Boot */
	VideoInit(&BootVideo, BootInfo);

	/* Now init log */
	LogInit(LogConsole, LogLevel1);

	/* Print Header */
	PrintHeader(BootInfo);

	/* Init HAL */
	BootInfo->InitHAL(BootInfo->ArchBootInfo);

	/* Init the heap */
	HeapInit();

	/* Init post-heap systems */
	DmInit();
	DmCreateDevice("Processor", DeviceCpu, &BootCpu);
	DmCreateDevice("BootVideo", DeviceVideo, &BootVideo);

	/* Init ModuleManager */
	ModuleMgrInit(BootInfo->RamDiskAddr, BootInfo->RamDiskSize);

	/* Init Threading & Scheduler for boot cpu */
	SchedulerInit(0);
	ThreadingInit();

	LogFatal("SYST", "End of kernel");
	Idle();

	/* Init post-systems */
	//printf("  - Initializing Post Memory Systems\n");
	BootInfo->InitPostSystems();

	/* Beyond this point we need timers 
	 * and right now we have no timers,
	 * and worst of all, timers are VERY 
	 * arch-specific, so we let the underlying
	 * architecture load them */
	//printf("  - Installing Timers...\n");
	BootInfo->InitTimers();

	/* Start out any extra cores */
	//printf("  - Initializing SMP\n");
	CpuInitSmp(BootInfo->ArchBootInfo);

	/* Start the request handle */
	//printf("  - Initializing Device Requests\n");
	DmStart();

	/* Virtual Filesystem */
	//printf("  - Initializing VFS\n");
	VfsInit();

	/* From this point, we should start seperate threads and
	* let this thread die out, because initial system setup
	* is now totally done, and the moment we start another
	* thread, it will take over as this is the idle thread */

	/* Drivers */
	//printf("  - Initializing Drivers...\n");
	ThreadingCreateThread("DriverSetup", DevicesInit, NULL, 0);

	/* Enter Idle Loop */
	while (1)
		Idle();
}