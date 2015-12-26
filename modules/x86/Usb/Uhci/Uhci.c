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
* MollenOS X86-32 USB UHCI Controller Driver
* Todo:
* Fix the interrupt spam of HcHalted
* Figure out how we can send transactions correctly
* For gods sake make it work, and get some sleep
*/

/* Includes */
#include <Module.h>
#include "Uhci.h"

#include <DeviceManager.h>
#include <UsbCore.h>
#include <Timers.h>
#include <Heap.h>

/* Needs abstraction */
#include <x86\Memory.h>
#include <x86\Pci.h>

/* CLib */
#include <string.h>

/* Globals */
uint32_t GlbUhciId = 0;

/* Prototypes (Internal) */
void UhciInitQueues(UhciController_t *Controller);
void UhciSetup(UhciController_t *Controller);
int UhciInterruptHandler(void *Args);

/* Port Callbacks */
void UhciPortSetup(void *Data, UsbHcPort_t *Port);
void UhciPortsCheck(void *Data);

/* Ep Callbacks */
void UhciEndpointSetup(void *Controller, UsbHcEndpoint_t *Endpoint);
void UhciEndpointDestroy(void *Controller, UsbHcEndpoint_t *Endpoint);

/* Helpers */
uint16_t UhciRead16(UhciController_t *Controller, uint16_t Register)
{
	/* Acquire Lock */
	SpinlockAcquire(&Controller->Lock);

	/* Read */
	uint16_t Value = inw((Controller->IoBase + Register));

	/* Release */
	SpinlockRelease(&Controller->Lock);

	/* Done! */
	return Value;
}

uint32_t UhciRead32(UhciController_t *Controller, uint16_t Register)
{
	/* Acquire Lock */
	SpinlockAcquire(&Controller->Lock);

	/* Read */
	uint32_t Value = inl((Controller->IoBase + Register));

	/* Release */
	SpinlockRelease(&Controller->Lock);

	/* Done! */
	return Value;
}

void UhciWrite8(UhciController_t *Controller, uint16_t Register, uint8_t Value)
{
	/* Acquire Lock */
	SpinlockAcquire(&Controller->Lock);

	/* Write new state */
	outb((Controller->IoBase + Register), Value);

	/* Release */
	SpinlockRelease(&Controller->Lock);
}

void UhciWrite16(UhciController_t *Controller, uint16_t Register, uint16_t Value)
{ 
	/* Acquire Lock */
	SpinlockAcquire(&Controller->Lock);

	/* Write new state */
	outw((Controller->IoBase + Register), Value);

	/* Release */
	SpinlockRelease(&Controller->Lock);
}

void UhciWrite32(UhciController_t *Controller, uint16_t Register, uint32_t Value)
{
	/* Acquire Lock */
	SpinlockAcquire(&Controller->Lock);
	
	/* Write new state */
	outl((Controller->IoBase + Register), Value);

	/* Release */
	SpinlockRelease(&Controller->Lock);
}

/* Entry point of a module */
MODULES_API void ModuleInit(void *Data)
{
	/* Vars */
	PciDevice_t *Device = (PciDevice_t*)Data;
	UhciController_t *Controller = NULL;
	uint16_t PciCommand;

	/* Allocate Resources for this controller */
	Controller = (UhciController_t*)kmalloc(sizeof(UhciController_t));
	Controller->PciDevice = Device;
	Controller->Id = GlbUhciId;

	/* Setup Lock */
	SpinlockReset(&Controller->Lock);
	GlbUhciId++;

	/* Enable i/o and Bus mastering and clear interrupt disable */
	PciCommand = (uint16_t)PciDeviceRead(Device, 0x4, 2);
	PciDeviceWrite(Device, 0x4, (PciCommand & ~(0x400)) | 0x1 | 0x4, 2);

	/* Get I/O Base from Bar4 */
	Controller->IoBase = (Device->Header->Bar4 & 0x0000FFFE);

	/* Get DMA */
	Controller->FrameListPhys = (Addr_t)MmPhysicalAllocateBlockDma();
	Controller->FrameList = (void*)Controller->FrameListPhys;

	/* Memset */
	memset(Controller->FrameList, 0, PAGE_SIZE);

	/* Install IRQ Handler */
	InterruptInstallPci(Device, UhciInterruptHandler, Controller);

	/* Reset Controller */
	UhciSetup(Controller);
}

/* Aligns address (with roundup if alignment is set) */
Addr_t UhciAlign(Addr_t BaseAddr, Addr_t AlignmentBits, Addr_t Alignment)
{
	/* Save, so we can modify */
	Addr_t AlignedAddr = BaseAddr;

	/* Only align if unaligned */
	if (AlignedAddr & AlignmentBits)
	{
		AlignedAddr &= ~AlignmentBits;
		AlignedAddr += Alignment;
	}

	/* Done */
	return AlignedAddr;
}

/* The two functions below are to setup QH Frame List */
uint32_t UhciFFS(uint32_t Value)
{
	/* Return Value */
	uint32_t RetNum = 0;

	/* 16 Bits */
	if (!(Value & 0xFFFF))
	{
		RetNum += 16;
		Value >>= 16;
	}

	/* 8 Bits */
	if (!(Value & 0xFF))
	{
		RetNum += 8;
		Value >>= 8;
	}

	/* 4 Bits */
	if (!(Value & 0xF))
	{
		RetNum += 4;
		Value >>= 4;
	}

	/* 2 Bits */
	if (!(Value & 0x3))
	{
		RetNum += 2;
		Value >>= 2;
	}

	/* 1 Bit */
	if (!(Value & 0x1))
		RetNum++;

	/* Done */
	return RetNum;
}

/* Determine Qh for Interrupt Transfer */
uint32_t UhciDetermineInterruptQh(UhciController_t *Controller, uint32_t Frame)
{
	/* Resulting Index */
	uint32_t Index;

	/* Determine index from first free bit 
	 * 8 queues */
	Index = 8 - UhciFFS(Frame | UHCI_NUM_FRAMES);

	/* Sanity */
	if (Index < 2 || Index > 8)
		Index = UHCI_POOL_ASYNC;

	/* Return Phys */
	return (Controller->QhPoolPhys[Index] | UHCI_TD_LINK_QH);
}

/* Start / Stop */
void UhciStart(UhciController_t *Controller)
{
	/* Send run command */
	uint16_t OldCmd = UhciRead16(Controller, UHCI_REGISTER_COMMAND);
	OldCmd |= (UHCI_CMD_CONFIGFLAG | UHCI_CMD_RUN | UHCI_CMD_MAXPACKET64);
	UhciWrite16(Controller, UHCI_REGISTER_COMMAND, OldCmd);

	/* Wait for it to start */	
	OldCmd = 0;
	WaitForConditionWithFault(OldCmd, 
		(UhciRead16(Controller, UHCI_REGISTER_STATUS) & UHCI_STATUS_HALTED) == 0, 100, 10);
}

void UhciStop(UhciController_t *Controller)
{
	/* Send stop command */
	uint16_t OldCmd = UhciRead16(Controller, UHCI_REGISTER_COMMAND);
	OldCmd &= ~(UHCI_CMD_RUN);
	UhciWrite16(Controller, UHCI_REGISTER_COMMAND, OldCmd);
}

/* Resets the Controller */
void UhciReset(UhciController_t *Controller)
{
	/* Vars */
	uint16_t Temp = 0;

	/* Write HCReset */
	UhciWrite16(Controller, UHCI_REGISTER_COMMAND, UHCI_CMD_HCRESET);

	/* Wait */
	WaitForConditionWithFault(Temp, (UhciRead16(Controller, UHCI_REGISTER_COMMAND) & UHCI_CMD_HCRESET) == 0, 100, 10);

	/* Sanity */
	if (Temp == 1)
		LogDebug("UHCI", "Reset signal is still active..");

	/* Clear out to be safe */
	UhciWrite16(Controller, UHCI_REGISTER_COMMAND, 0x0000);
	UhciWrite16(Controller, UHCI_REGISTER_INTR, 0x0000);

	/* Now re-configure it */
	UhciWrite8(Controller, UHCI_REGISTER_SOFMOD, 64); /* Frame Length 1 ms */
	UhciWrite32(Controller, UHCI_REGISTER_FRBASEADDR, Controller->FrameListPhys);
	UhciWrite16(Controller, UHCI_REGISTER_FRNUM, (0 & 2047));

	/* Enable interrupts */
	UhciWrite16(Controller, UHCI_REGISTER_INTR,
		(UHCI_INTR_TIMEOUT | UHCI_INTR_SHORT_PACKET
		| UHCI_INTR_RESUME | UHCI_INTR_COMPLETION));

	/* Start Controller */
	UhciStart(Controller);
}

/* Initializes the Controller */
void UhciSetup(UhciController_t *Controller)
{
	/* Vars */
	UsbHc_t *Hcd;
	uint16_t PortsEnabled = 0;
	uint16_t Temp = 0, i = 0;

	/* Disable interrupts while configuring (and stop controller) */
	UhciWrite16(Controller, UHCI_REGISTER_COMMAND, 0x0000);
	UhciWrite16(Controller, UHCI_REGISTER_INTR, 0x0000);

	/* Global Reset */
	UhciWrite16(Controller, UHCI_REGISTER_COMMAND, UHCI_CMD_GRESET);
	StallMs(100);
	UhciWrite16(Controller, UHCI_REGISTER_COMMAND, 0x0000);

	/* Disable stuff again, we don't know what state is set after reset */
	UhciWrite16(Controller, UHCI_REGISTER_COMMAND, 0x0000);
	UhciWrite16(Controller, UHCI_REGISTER_INTR, 0x0000);

	/* Setup Queues */
	UhciInitQueues(Controller);

	/* Reset */
	UhciWrite16(Controller, UHCI_REGISTER_COMMAND, UHCI_CMD_HCRESET);

	/* Wait */
	WaitForConditionWithFault(Temp, (UhciRead16(Controller, UHCI_REGISTER_COMMAND) & UHCI_CMD_HCRESET) == 0, 100, 10);

	/* Sanity */
	if (Temp == 1)
		LogDebug("UHCI", "Reset signal is still active..");

	/* Clear out to be safe */
	UhciWrite16(Controller, UHCI_REGISTER_COMMAND, 0x0000);
	UhciWrite16(Controller, UHCI_REGISTER_INTR, 0x0000);

	/* Now re-configure it */
	UhciWrite8(Controller, UHCI_REGISTER_SOFMOD, 64); /* Frame Length 1 ms */
	UhciWrite32(Controller, UHCI_REGISTER_FRBASEADDR, Controller->FrameListPhys);
	UhciWrite16(Controller, UHCI_REGISTER_FRNUM, (0 & 2047));

	/* We get port count & 0 them */
	for (i = 0; i <= UHCI_MAX_PORTS; i++)
	{
		Temp = UhciRead16(Controller, (UHCI_REGISTER_PORT_BASE + (i * 2)));

		/* Is it a valid port? */
		if (!(Temp & UHCI_PORT_RESERVED)
			|| Temp == 0xFFFF)
		{
			/* This reserved bit must be 1 */
			/* And we must have 2 ports atleast */
			break;
		}
	}

	/* Ports are now i */
	Controller->NumPorts = i;

	/* Enable PCI Interrupts */
	PciDeviceWrite(Controller->PciDevice, UHCI_USBLEGEACY, 0x2000, 2);

	/* If vendor is Intel we null out the intel register */
	if (Controller->PciDevice->Header->VendorId == 0x8086)
		PciDeviceWrite(Controller->PciDevice, UHCI_USBRES_INTEL, 0x00, 1);

	/* Enable interrupts */
	UhciWrite16(Controller, UHCI_REGISTER_INTR,
		(UHCI_INTR_TIMEOUT | UHCI_INTR_SHORT_PACKET
		| UHCI_INTR_RESUME | UHCI_INTR_COMPLETION));

	/* Start Controller */
	UhciStart(Controller);
	
	/* Debug */
	LogDebug("UHCI", "%u: Port Count %u, Command Register 0x%x", Controller->Id,
		Controller->NumPorts, UhciRead16(Controller, UHCI_REGISTER_COMMAND));

	/* Setup HCD */
	Hcd = UsbInitController((void*)Controller, UhciController, Controller->NumPorts);

	/* Setup functions */
	Hcd->RootHubCheck = UhciPortsCheck;
	Hcd->PortSetup = UhciPortSetup;
	Hcd->Reset = UhciReset;

	/* Ep Functions */
	Hcd->EndpointSetup = UhciEndpointSetup;
	Hcd->EndpointDestroy = UhciEndpointDestroy;

	/* Transaction Functions */

	/* Register it */
	Controller->HcdId = UsbRegisterController(Hcd);

	/* Install Periodic Check (NO HUB INTERRUPTS!?)
	 * Anyway this will initiate ports */
	//TimersCreateTimer(UhciPortsCheck, Controller, TimerPeriodic, 500);
}

/* Initialises Queue Heads & Interrupt Queeue */
void UhciInitQueues(UhciController_t *Controller)
{
	/* Setup Vars */
	uint32_t *FrameListPtr = (uint32_t*)Controller->FrameList;
	Addr_t Pool = 0, PoolPhysical = 0;
	uint32_t i;

	/* Setup Null Td */
	Pool = (Addr_t)kmalloc(sizeof(UhciTransferDescriptor_t) + UHCI_STRUCT_ALIGN);
	Controller->NullTd = (UhciTransferDescriptor_t*)UhciAlign(Pool, UHCI_STRUCT_ALIGN_BITS, UHCI_STRUCT_ALIGN);
	Controller->NullTdPhysical = MmVirtualGetMapping(NULL, (VirtAddr_t)Controller->NullTd);

	/* Memset it */
	memset((void*)Controller->NullTd, 0, sizeof(UhciTransferDescriptor_t));

	/* Set link invalid */
	Controller->NullTd->Header = UHCI_TD_PID_IN | UHCI_TD_DEVICE_ADDR(0x7F) | UHCI_TD_MAX_LEN(0x7FF);
	Controller->NullTd->Link = UHCI_TD_LINK_END;

	/* Setup Qh Pool */
	Pool = (Addr_t)kmalloc((sizeof(UhciQueueHead_t) * UHCI_POOL_NUM_QH) + UHCI_STRUCT_ALIGN);
	Pool = UhciAlign(Pool, UHCI_STRUCT_ALIGN_BITS, UHCI_STRUCT_ALIGN);
	PoolPhysical = (Addr_t)MmVirtualGetMapping(NULL, Pool);

	/* Null out pool */
	memset((void*)Pool, 0, sizeof(UhciQueueHead_t) * UHCI_POOL_NUM_QH);

	/* Set them up */
	for (i = 0; i < UHCI_POOL_NUM_QH; i++)
	{
		/* Set QH */
		Controller->QhPool[i] = (UhciQueueHead_t*)Pool;
		Controller->QhPoolPhys[i] = PoolPhysical;

		/* Set its index */
		Controller->QhPool[i]->Flags = UHCI_QH_INDEX(i);

		/* Increase */
		Pool += sizeof(UhciQueueHead_t);
		PoolPhysical += sizeof(UhciQueueHead_t);
	}

	/* Setup interrupt queues */
	for (i = 2; i < UHCI_POOL_ASYNC; i++)
	{
		/* Set QH Link */
		Controller->QhPool[i]->Link = (Controller->QhPoolPhys[UHCI_POOL_ASYNC] | UHCI_TD_LINK_QH);
		Controller->QhPool[i]->LinkVirtual = (uint32_t)Controller->QhPool[UHCI_POOL_ASYNC];

		/* Disable TD List */
		Controller->QhPool[i]->Child = UHCI_TD_LINK_END;
		Controller->QhPool[i]->ChildVirtual = 0;

		/* Set in use */
		Controller->QhPool[i]->Flags |= (UHCI_QH_SET_POOL_NUM(i) | UHCI_QH_ACTIVE);
	}
	
	/* Setup Iso Qh */

	/* Setup async Qh */
	Controller->QhPool[UHCI_POOL_ASYNC]->Link = UHCI_TD_LINK_END;
	Controller->QhPool[UHCI_POOL_ASYNC]->LinkVirtual = 0;
	Controller->QhPool[UHCI_POOL_ASYNC]->Child = Controller->NullTdPhysical;
	Controller->QhPool[UHCI_POOL_ASYNC]->ChildVirtual = (uint32_t)Controller->NullTd;

	/* Setup null QH */
	Controller->QhPool[UHCI_POOL_NULL]->Link = (Controller->QhPoolPhys[UHCI_POOL_NULL] | UHCI_TD_LINK_QH | UHCI_TD_LINK_END);
	Controller->QhPool[UHCI_POOL_NULL]->LinkVirtual = (uint32_t)Controller->QhPool[UHCI_POOL_NULL];
	Controller->QhPool[UHCI_POOL_NULL]->Child = Controller->NullTdPhysical;
	Controller->QhPool[UHCI_POOL_NULL]->ChildVirtual = (uint32_t)Controller->NullTd;

	/* 1024 Entries 
	 * Set all entries to the 8 interrupt queues, and we 
	 * want them interleaved such that some queues get visited more than others */
	for (i = UHCI_POOL_ISOCHRONOUS + 1; i < UHCI_POOL_ASYNC; i++)
		Controller->QhPool[i]->Link = Controller->QhPoolPhys[UHCI_POOL_ASYNC] | UHCI_TD_LINK_QH;
	for (i = 0; i < UHCI_NUM_FRAMES; i++)
		FrameListPtr[i] = UhciDetermineInterruptQh(Controller, i);

	/* Terminate */
	Controller->QhPool[UHCI_POOL_ASYNC]->Link |= UHCI_TD_LINK_END;
	Controller->QhPool[UHCI_POOL_ASYNC]->LinkVirtual = 0;
	Controller->QhPool[UHCI_POOL_ASYNC]->Child = Controller->NullTdPhysical;
	Controller->QhPool[UHCI_POOL_ASYNC]->ChildVirtual = (uint32_t)Controller->NullTd;

	/* Init transaction list */
	Controller->TransactionList = list_create(LIST_SAFE);
}

/* Ports */
void UhciPortReset(UhciController_t *Controller, uint32_t Port)
{
	/* Calc */
	uint16_t Temp, i;
	uint16_t pOffset = (UHCI_REGISTER_PORT_BASE + ((uint16_t)Port * 2));

	/* Step 1. Send reset signal */
	UhciWrite16(Controller, pOffset, UHCI_PORT_RESET);

	/* Wait atlest 50 ms (per USB specification) */
	StallMs(60);

	/* Now deassert reset signal */
	Temp = UhciRead16(Controller, pOffset);
	UhciWrite16(Controller, pOffset, Temp & ~UHCI_PORT_RESET);

	/* Recovery Wait */
	StallMs(10);

	/* Step 2. Enable Port */
	Temp = UhciRead16(Controller, pOffset) & 0xFFF5;
	UhciWrite16(Controller, pOffset, Temp | UHCI_PORT_ENABLED);

	/* Wait for enable, with timeout */
	i = 0;
	while (i < 10)
	{
		/* Increase */
		i++;

		/* Stall */
		StallMs(10);

		/* Check status */
		Temp = UhciRead16(Controller, pOffset);

		/* Is device still connected? */
		if (!(Temp & UHCI_PORT_CONNECT_STATUS))
			return;

		/* Has it raised any event bits? In that case clear'em */
		if (Temp & (UHCI_PORT_CONNECT_EVENT | UHCI_PORT_ENABLED_EVENT))
		{
			UhciWrite16(Controller, pOffset, Temp);
			continue;
		}

		/* Done? */
		if (Temp & UHCI_PORT_ENABLED)
			break;
	}

	/* Sanity */
	if (i == 10)
	{
		LogDebug("UHCI", "Port %u Reset Failed!", Port);
		return;
	}
}

/* Detect any port changes */
void UhciPortCheck(UhciController_t *Controller, int Port)
{
	/* Get port status */
	uint16_t pStatus = UhciRead16(Controller, (UHCI_REGISTER_PORT_BASE + ((uint16_t)Port * 2)));
	UsbHc_t *Hcd;

	/* Has there been a connection event? */
	if (!(pStatus & UHCI_PORT_CONNECT_EVENT))
		return;

	/* Clear connection event */
	UhciWrite16(Controller, (UHCI_REGISTER_PORT_BASE + ((uint16_t)Port * 2)), UHCI_PORT_CONNECT_EVENT);

	/* Get HCD data */
	Hcd = UsbGetHcd(Controller->HcdId);

	/* Sanity */
	if (Hcd == NULL)
		return;

	/* Connect event? */
	if (pStatus & UHCI_PORT_CONNECT_STATUS)
	{
		/* Connection Event */
		UsbEventCreate(Hcd, Port, HcdConnectedEvent);
	}
	else
	{
		/* Disconnect Event */
		UsbEventCreate(Hcd, Port, HcdDisconnectedEvent);
	}
}

/* Go through ports */
void UhciPortsCheck(void *Data)
{
	UhciController_t *Controller = (UhciController_t*)Data;
	int i;

	for (i = 0; i < (int)Controller->NumPorts; i++)
		UhciPortCheck(Controller, i);
}

/* Gets port status */
void UhciPortSetup(void *Data, UsbHcPort_t *Port)
{
	UhciController_t *Controller = (UhciController_t*)Data;
	uint16_t pStatus = 0;

	/* Reset Port */
	UhciPortReset(Controller, Port->Id);

	/* Dump info */
	pStatus = UhciRead16(Controller, (UHCI_REGISTER_PORT_BASE + ((uint16_t)Port->Id * 2)));
	LogDebug("UHCI", "UHCI %u.%u Status: 0x%x\n", Controller->Id, Port->Id, pStatus);

	/* Is it connected? */
	if (pStatus & UHCI_PORT_CONNECT_STATUS)
		Port->Connected = 1;
	else
		Port->Connected = 0;

	/* Enabled? */
	if (pStatus & UHCI_PORT_ENABLED)
		Port->Enabled = 1;
	else
		Port->Enabled = 0;

	/* Lowspeed? */
	if (pStatus & UHCI_PORT_LOWSPEED)
		Port->FullSpeed = 0;
	else
		Port->FullSpeed = 1;
}

/* QH Functions */
Addr_t UhciAllocateQh(UhciController_t *Controller, UsbTransferType_t Type)
{
	UhciQueueHead_t *Qh = NULL;
	Addr_t cIndex = 0;
	int i;

	/* Pick a QH */
	SpinlockAcquire(&Controller->Lock);

	/* Grap it, locked operation */
	if (Type == ControlTransfer
		|| Type == BulkTransfer)
	{
		/* Grap Index */
		for (i = UHCI_POOL_START; i < UHCI_POOL_NUM_QH; i++)
		{
			/* Sanity */
			if (Controller->QhPool[i]->Flags & UHCI_QH_ALLOCATED)
				continue;

			/* Yay!! */
			Controller->QhPool[i]->Flags |= UHCI_QH_ALLOCATED;
			cIndex = (Addr_t)i;
			break;
		}

		/* Sanity */
		if (i == UHCI_POOL_NUM_QH)
			kernel_panic("USB_UHCI::WTF RAN OUT OF EDS\n");
	}
	else if (Type == InterruptTransfer
		|| Type == IsochronousTransfer)
	{
		/* Allocate */
		Addr_t aSpace = (Addr_t)kmalloc(sizeof(UhciQueueHead_t) + UHCI_STRUCT_ALIGN);
		cIndex = UhciAlign(aSpace, UHCI_STRUCT_ALIGN_BITS, UHCI_STRUCT_ALIGN);

		/* Zero it out */
		memset((void*)cIndex, 0, sizeof(UhciQueueHead_t));
	}

	/* Release lock */
	SpinlockRelease(&Controller->Lock);

	/* Done */
	return cIndex;
}

/* TD Functions */
Addr_t OhciAllocateTd(UhciEndpoint_t *Ep, UsbTransferType_t Type)
{
	size_t i;
	Addr_t cIndex = 0xFFFF;
	UhciTransferDescriptor_t *Td;

	/* Pick a QH */
	SpinlockAcquire(&Ep->Lock);

	/* Sanity */
	if (Type == ControlTransfer
		|| Type == BulkTransfer)
	{
		/* Grap it, locked operation */
		for (i = 0; i < Ep->TdsAllocated; i++)
		{
			/* Sanity */
			if (Ep->TDPool[i]->Flags & UHCI_TD_ACTIVE)
				continue;

			/* Yay!! */
			Ep->TDPool[i]->Flags |= UHCI_TD_ACTIVE;
			cIndex = (Addr_t)i;
			break;
		}

		/* Sanity */
		if (i == Ep->TdsAllocated)
			kernel_panic("USB_UHCI::WTF ran out of TD's!!!!\n");
	}
	else
	{
		/* Isochronous & Interrupt */

		/* Allocate a new */
		Td = (UhciTransferDescriptor_t*)UhciAlign(((Addr_t)kmalloc(sizeof(UhciTransferDescriptor_t) + UHCI_STRUCT_ALIGN)), UHCI_STRUCT_ALIGN_BITS, UHCI_STRUCT_ALIGN);

		/* Null it */
		memset((void*)Td, 0, sizeof(UhciTransferDescriptor_t));

		/* Set as index */
		cIndex = (Addr_t)Td;
	}

	/* Release Lock */
	SpinlockRelease(&Ep->Lock);

	return cIndex;
}

/* Setup TD */
uhci_transfer_desc_t *uhci_td_setup(uhci_controller_t *controller, addr_t next_td,
	uint32_t lowspeed, uint32_t device_addr, uint32_t ep_addr, uint32_t toggle, uint8_t request_direction,
	uint8_t request_type, uint8_t request_value_low, uint8_t request_value_high, uint16_t request_index,
	uint16_t request_length, void **td_buffer)
{
	uint32_t td_index;
	void *buffer;
	uhci_transfer_desc_t *td;
	addr_t td_phys;
	usb_packet_t *packet;

	/* Start out by grapping a TD */
	td_index = uhci_get_td(controller);
	buffer = controller->td_pool_buffers[td_index];
	td = controller->td_pool[td_index];
	td_phys = controller->td_pool_phys[td_index];

	/* Set link to next TD */
	if (next_td == X86_UHCI_TD_LINK_INVALID)
		td->link_ptr = X86_UHCI_TD_LINK_INVALID;
	else
		td->link_ptr = memory_getmap(NULL, (virtaddr_t)next_td);

	/* Setup TD Control Status */
	td->control = 0;
	td->control |= X86_UHCI_TD_CTRL_ACTIVE;
	td->control |= X86_UHCI_TD_SET_ERR_CNT(3);

	if (lowspeed)
		td->control |= X86_UHCI_TD_LOWSPEED;

	/* Setup TD Header Packet */
	td->header = X86_UHCI_TD_PID_SETUP;
	td->header |= X86_UHCI_TD_DEVICE_ADDR(device_addr);
	td->header |= X86_UHCI_TD_EP_ADDR(ep_addr);
	td->header |= X86_UHCI_TD_DATA_TOGGLE(toggle);
	td->header |= X86_UHCI_TD_MAX_LEN((sizeof(usb_packet_t) - 1));

	/* Setup SETUP packet */
	*td_buffer = buffer;
	packet = (usb_packet_t*)buffer;
	packet->direction = request_direction;
	packet->type = request_type;
	packet->value_low = request_value_low;
	packet->value_high = request_value_high;
	packet->index = request_index;
	packet->length = request_length;

	/* Set buffer */
	td->buffer = memory_getmap(NULL, (virtaddr_t)buffer);

	/* Done */
	return td;
}

/* In/Out TD */
uhci_transfer_desc_t *uhci_td_io(uhci_controller_t *controller, addr_t next_td,
	uint32_t lowspeed, uint32_t device_addr, uint32_t ep_addr, uint32_t toggle, 
	uint32_t pid, uint32_t length, void **td_buffer)
{
	uint32_t td_index;
	void *buffer;
	uhci_transfer_desc_t *td;
	addr_t td_phys;

	/* Start out by grapping a TD */
	td_index = uhci_get_td(controller);
	buffer = controller->td_pool_buffers[td_index];
	td = controller->td_pool[td_index];
	td_phys = controller->td_pool_phys[td_index];

	/* Set link to next TD */
	if (next_td == X86_UHCI_TD_LINK_INVALID)
		td->link_ptr = X86_UHCI_TD_LINK_INVALID;
	else
		td->link_ptr = memory_getmap(NULL, (virtaddr_t)next_td);

	/* Setup TD Control Status */
	td->control = 0;
	td->control |= X86_UHCI_TD_CTRL_ACTIVE;
	td->control |= X86_UHCI_TD_SET_ERR_CNT(3);

	if (lowspeed)
		td->control |= X86_UHCI_TD_LOWSPEED;

	/* Setup TD Header Packet */
	td->header = pid;
	td->header |= X86_UHCI_TD_DEVICE_ADDR(device_addr);
	td->header |= X86_UHCI_TD_EP_ADDR(ep_addr);
	td->header |= X86_UHCI_TD_DATA_TOGGLE(toggle);

	if (length > 0)
		td->header |= X86_UHCI_TD_MAX_LEN((length - 1));
	else
		td->header |= X86_UHCI_TD_MAX_LEN(0x7FF);

	/* Set buffer */
	*td_buffer = buffer;
	td->buffer = memory_getmap(NULL, (virtaddr_t)buffer);

	/* Done */
	return td;
}

/* Endpoint Functions */
void UhciEndpointSetup(void *Controller, UsbHcEndpoint_t *Endpoint)
{
	/* Cast */
	UhciController_t *uCtrl = (UhciController_t*)Controller;
	Addr_t BufAddr = 0, BufAddrMax = 0;
	Addr_t Pool, PoolPhys;
	size_t i;

	/* Allocate a structure */
	UhciEndpoint_t *uEp = (UhciEndpoint_t*)kmalloc(sizeof(UhciEndpoint_t));

	/* Construct the lock */
	SpinlockReset(&uEp->Lock);

	/* Woah */
	_CRT_UNUSED(uCtrl);

	/* Now, we want to allocate some TD's
	* but it largely depends on what kind of endpoint this is */
	if (Endpoint->Type == X86_USB_EP_TYPE_CONTROL)
		uEp->TdsAllocated = UHCI_ENDPOINT_MIN_ALLOCATED;
	else if (Endpoint->Type == X86_USB_EP_TYPE_BULK)
	{
		/* Depends on the maximum transfer */
		uEp->TdsAllocated = DEVICEMANAGER_MAX_IO_SIZE / Endpoint->MaxPacketSize;

		/* Take in account control packets and other stuff */
		uEp->TdsAllocated += UHCI_ENDPOINT_MIN_ALLOCATED;
	}
	else
	{
		/* We handle interrupt & iso dynamically
		* we don't predetermine their sizes */
		uEp->TdsAllocated = 0;
		Endpoint->AttachedData = uEp;
		return;
	}

	/* Now, we do the actual allocation */
	uEp->TDPool = (UhciTransferDescriptor_t**)kmalloc(sizeof(UhciTransferDescriptor_t*) * uEp->TdsAllocated);
	uEp->TDPoolBuffers = (Addr_t**)kmalloc(sizeof(Addr_t*) * uEp->TdsAllocated);
	uEp->TDPoolPhysical = (Addr_t*)kmalloc(sizeof(Addr_t) * uEp->TdsAllocated);

	/* Allocate a TD block */
	Pool = (Addr_t)kmalloc((sizeof(UhciTransferDescriptor_t) * uEp->TdsAllocated) + UHCI_STRUCT_ALIGN);
	Pool = OhciAlign(Pool, UHCI_STRUCT_ALIGN_BITS, UHCI_STRUCT_ALIGN);
	PoolPhys = MmVirtualGetMapping(NULL, Pool);

	/* Allocate buffers */
	BufAddr = (Addr_t)kmalloc_a(PAGE_SIZE);
	BufAddrMax = BufAddr + PAGE_SIZE - 1;

	/* Memset it */
	memset((void*)Pool, 0, sizeof(UhciTransferDescriptor_t) * uEp->TdsAllocated);

	/* Iterate it */
	for (i = 0; i < uEp->TdsAllocated; i++)
	{
		/* Set */
		uEp->TDPool[i] = (UhciTransferDescriptor_t*)Pool;
		uEp->TDPoolPhysical[i] = PoolPhys;

		/* Allocate another page? */
		if (BufAddr > BufAddrMax)
		{
			BufAddr = (Addr_t)kmalloc_a(PAGE_SIZE);
			BufAddrMax = BufAddr + PAGE_SIZE - 1;
		}

		/* Setup Buffer */
		uEp->TDPoolBuffers[i] = (Addr_t*)BufAddr;
		uEp->TDPool[i]->Buffer = MmVirtualGetMapping(NULL, BufAddr);
		uEp->TDPool[i]->Link = UHCI_TD_LINK_END;

		/* Increase */
		Pool += sizeof(UhciTransferDescriptor_t);
		PoolPhys += sizeof(UhciTransferDescriptor_t);
		BufAddr += Endpoint->MaxPacketSize;
	}

	/* Done! Save */
	Endpoint->AttachedData = uEp;
}

void UhciEndpointDestroy(void *Controller, UsbHcEndpoint_t *Endpoint)
{
	/* Cast */
	UhciController_t *uCtrl = (UhciController_t*)Controller;
	UhciEndpoint_t *uEp = (UhciEndpoint_t*)Endpoint->AttachedData;

	/* Sanity */
	if (uEp == NULL)
		return;

	UhciTransferDescriptor_t *uTd = uEp->TDPool[0];
	size_t i;

	/* Woah */
	_CRT_UNUSED(uCtrl);

	/* Sanity */
	if (uEp->TdsAllocated != 0)
	{
		/* Let's free all those resources */
		for (i = 0; i < uEp->TdsAllocated; i++)
		{
			/* free buffer */
			kfree(uEp->TDPoolBuffers[i]);
		}

		/* Free blocks */
		kfree(uTd);
		kfree(uEp->TDPoolBuffers);
		kfree(uEp->TDPoolPhysical);
		kfree(uEp->TDPool);
	}

	/* Free the descriptor */
	kfree(uEp);
}

/* Transaction Functions */

/* This one prepaires an ED */
void UhciTransactionInit(void *Controller, UsbHcRequest_t *Request)
{	
	/* Vars */
	UhciController_t *Ctrl = (UhciController_t*)Controller;
	Addr_t Temp = 0;

	/* Get a QH */
	Temp = UhciAllocateQh(Ctrl, Request->Type);

	/* We allocate new ep descriptors for Iso & Int */
	if (Request->Type == ControlTransfer
		|| Request->Type == BulkTransfer)
	{
		Ctrl->QhPool[Temp]->Link |= UHCI_TD_LINK_END;
		Ctrl->QhPool[Temp]->Child |= UHCI_TD_LINK_END;
		Request->Data = Ctrl->QhPool[Temp];
	}
	else
		Request->Data = (void*)Temp;

	/* Set as not Completed for start */
	Request->Status = TransferNotProcessed;
}

usb_hc_transaction_t *uhci_transaction_setup(void *controller, usb_hc_request_t *request)
{
	uhci_controller_t *ctrl = (uhci_controller_t*)controller;
	usb_hc_transaction_t *transaction;

	/* Allocate transaction */
	transaction = (usb_hc_transaction_t*)kmalloc(sizeof(usb_hc_transaction_t));
	transaction->io_buffer = 0;
	transaction->io_length = 0;
	transaction->link = NULL;

	/* Create a setup TD */
	transaction->transfer_descriptor = (void*)uhci_td_setup(ctrl, X86_UHCI_TD_LINK_INVALID, request->lowspeed,
		request->device->address, request->endpoint, request->toggle, request->packet.direction, request->packet.type,
		request->packet.value_low, request->packet.value_high, request->packet.index, request->packet.length,
		&transaction->transfer_buffer);

	/* If previous transaction */
	if (request->transactions != NULL)
	{
		uhci_transfer_desc_t *prev_td;
		usb_hc_transaction_t *ctrans = request->transactions;

		while (ctrans->link)
			ctrans = ctrans->link;

		prev_td = (uhci_transfer_desc_t*)ctrans->transfer_descriptor;
		prev_td->link_ptr = (memory_getmap(NULL, (virtaddr_t)transaction->transfer_descriptor) | X86_UHCI_TD_LINK_DEPTH);
	}

	return transaction;
}

usb_hc_transaction_t *uhci_transaction_in(void *controller, usb_hc_request_t *request)
{
	uhci_controller_t *ctrl = (uhci_controller_t*)controller;
	usb_hc_transaction_t *transaction;

	/* Allocate transaction */
	transaction = (usb_hc_transaction_t*)kmalloc(sizeof(usb_hc_transaction_t));
	transaction->io_buffer = request->io_buffer;
	transaction->io_length = request->io_length;
	transaction->link = NULL;

	/* Create a In TD */
	transaction->transfer_descriptor = (void*)uhci_td_io(ctrl, X86_UHCI_TD_LINK_INVALID, request->lowspeed,
		request->device->address, request->endpoint, request->toggle, X86_UHCI_TD_PID_IN, request->io_length,
		&transaction->transfer_buffer);

	/* If previous transaction */
	if (request->transactions != NULL)
	{
		uhci_transfer_desc_t *prev_td;
		usb_hc_transaction_t *ctrans = request->transactions;

		while (ctrans->link)
			ctrans = ctrans->link;

		prev_td = (uhci_transfer_desc_t*)ctrans->transfer_descriptor;
		prev_td->link_ptr = (memory_getmap(NULL, (virtaddr_t)transaction->transfer_descriptor) | X86_UHCI_TD_LINK_DEPTH);
	}

	return transaction;
}

usb_hc_transaction_t *uhci_transaction_out(void *controller, usb_hc_request_t *request)
{
	uhci_controller_t *ctrl = (uhci_controller_t*)controller;
	usb_hc_transaction_t *transaction;

	/* Allocate transaction */
	transaction = (usb_hc_transaction_t*)kmalloc(sizeof(usb_hc_transaction_t));
	transaction->io_buffer = 0;
	transaction->io_length = 0;
	transaction->link = NULL;

	/* Create a In TD */
	transaction->transfer_descriptor = (void*)uhci_td_io(ctrl, X86_UHCI_TD_LINK_INVALID, request->lowspeed,
		request->device->address, request->endpoint, request->toggle, X86_UHCI_TD_PID_OUT, request->io_length,
		&transaction->transfer_buffer);

	/* Copy Data */
	if (request->io_buffer != NULL && request->io_length != 0)
		memcpy(transaction->transfer_buffer, request->io_buffer, request->io_length);

	/* If previous transaction */
	if (request->transactions != NULL)
	{
		uhci_transfer_desc_t *prev_td;
		usb_hc_transaction_t *ctrans = request->transactions;

		while (ctrans->link)
			ctrans = ctrans->link;

		prev_td = (uhci_transfer_desc_t*)ctrans->transfer_descriptor;
		prev_td->link_ptr = (memory_getmap(NULL, (virtaddr_t)transaction->transfer_descriptor) | X86_UHCI_TD_LINK_DEPTH);
	}

	return transaction;
}

void uhci_transaction_send(void *controller, usb_hc_request_t *request)
{
	/* Wuhu */
	usb_hc_transaction_t *transaction = request->transactions;
	uhci_controller_t *ctrl = (uhci_controller_t*)controller;
	uhci_transfer_desc_t *td = NULL;
	uhci_queue_head_t *qh = NULL, *parent_qh = NULL;
	int completed = 1;
	addr_t qh_address;

	/* Get physical */
	qh_address = memory_getmap(NULL, (virtaddr_t)request->data);

	/* Set as not completed for start */
	request->completed = 0;

	/* Initialize QH */
	qh = (uhci_queue_head_t*)request->data;
	
	/* Set TD List */
	qh->head_ptr = memory_getmap(NULL, (virtaddr_t)transaction->transfer_descriptor);
	qh->head = (uint32_t)transaction->transfer_descriptor;

	/* Set next link to NULL, we insert it as tail :-) */
	qh->link_ptr = X86_UHCI_TD_LINK_INVALID;
	qh->link = 0;

	/* Set HCD data */
	qh->hcd_data = (uint32_t)request->transactions;

	/* Get spinlock */
	spinlock_acquire(&ctrl->lock);

	/* Debug */
	printf("QH at 0x%x, Head 0x%x, Link 0x%x\n", (addr_t)qh, qh->head_ptr, qh->link_ptr);

	/* Set last TD to produce an interrupt */
	transaction = request->transactions;
	while (transaction)
	{
		/* Get TD */
		td = (uhci_transfer_desc_t*)transaction->transfer_descriptor;

		/* If this is last, we set it to IOC */
		if (transaction->link == NULL)
			td->control |= X86_UHCI_TD_IOC;

		/* Debug */
		printf("TD at 0x%x, Control 0x%x, Header 0x%x, Buffer 0x%x, Link 0x%x\n",
			(addr_t)td, td->control, td->header, td->buffer, td->link_ptr);
		
		/* Next Link */
		transaction = transaction->link;
	}

	/* Stop the controller, we are going to modify the frame-list */
	uhci_stop(ctrl);

	/* Update the QH List */
	/* Link this to the async list */
	parent_qh = ctrl->qh_pool[X86_UHCI_POOL_ASYNC];

	/* Should not take too long this loop */
	while (parent_qh->link != 0)
		parent_qh = (uhci_queue_head_t*)parent_qh->link;

	/* Now insert us at the tail */
	parent_qh->link_ptr = (qh_address | X86_UHCI_TD_LINK_QH);
	parent_qh->link = (uint32_t)qh;

	/* Add our transaction :-) */
	list_append(ctrl->transactions_list, list_create_node(0, request->data));

	/* Wait for interrupt */
	//scheduler_sleep_thread((addr_t*)request->data);

	/* Start controller */
	uhci_start(ctrl);

	/* Release lock */
	spinlock_release(&ctrl->lock);

	/* Yield */
	_yield();

	printf("Heya! Got woken up!\n");

	/* Check Conditions (WithOUT dummy) */
	transaction = request->transactions;
	while (transaction)
	{
		td = (uhci_transfer_desc_t*)transaction->transfer_descriptor;

		/* Debug */
		printf("TD at 0x%x, Control 0x%x, Header 0x%x, Buffer 0x%x, Link 0x%x\n",
			(addr_t)td, td->control, td->header, td->buffer, td->link_ptr);
		
		/* Error? :s */
		if (X86_UHCI_TD_STATUS(td->control))
		{
			completed = 0;
			break;
		}

		transaction = transaction->link;
	}

	/* Lets see... */
	if (completed)
	{
		/* Build Buffer */
		transaction = request->transactions;

		while (transaction)
		{
			/* Copy Data? */
			if (transaction->io_buffer != NULL && transaction->io_length != 0)
			{
				printf("Buffer Copy 0x%x, Length 0x%x\n", transaction->io_buffer, transaction->io_length);
				memcpy(transaction->io_buffer, transaction->transfer_buffer, transaction->io_length);
			}

			/* Next Link */
			transaction = transaction->link;
		}

		/* Set as completed */
		request->completed = 1;
	}
}

/* Interrupt Handler */
int UhciInterruptHandler(void *Args)
{
	uint16_t intr_state = 0;
	uhci_controller_t *controller = (uhci_controller_t*)args;

	/* Get INTR state */
	intr_state = uhci_read16(controller, X86_UHCI_REGISTER_STATUS);
	
	/* Did this one come from us? */
	if (!(intr_state & 0x1F))
		return X86_IRQ_NOT_HANDLED;

	/* Debug */
	printf("UHCI_INTERRUPT Controller %u: 0x%x\n", controller->id, intr_state);

	/* Clear Interrupt Bits :-) */
	uhci_write16(controller, X86_UHCI_REGISTER_STATUS, intr_state);

	/* Sanity */
	if (controller->initialized == 0)
	{
		/* Bleh */
		return X86_IRQ_HANDLED;
	}

	/* So.. */
	if (intr_state & (X86_UHCI_STATUS_USBINT | X86_UHCI_STATUS_INTR_ERROR))
	{
		/* Transaction is completed / Failed */
		list_t *transaction_list = (list_t*)controller->transactions_list;
		uhci_queue_head_t *qh;
		list_node_t *ta;
		int n = 0;

		/* Get transactions in progress and find the offender */
		ta = list_get_node_by_id(transaction_list, 0, n);
		while (ta != NULL)
		{
			usb_hc_transaction_t *transactions;
			uint32_t completed = 1;
			
			/* Get transactions linked to his QH */
			qh = (uhci_queue_head_t*)ta->data;
			transactions = (usb_hc_transaction_t*)qh->hcd_data;

			/* Loop through transactions */
			while (transactions && completed != 0)
			{
				uhci_transfer_desc_t *td;

				/* Get transfer descriptor */
				td = (uhci_transfer_desc_t*)transactions->transfer_descriptor;

				/* Check status */
				if (td->control & X86_UHCI_TD_CTRL_ACTIVE)
				{
					/* If its still active this can't possibly be the transfer */
					completed = 0;
					break;
				}

				/* Error Transfer ? */
				if ((X86_UHCI_TD_ERROR_COUNT(td->control) == 0 && X86_UHCI_TD_STATUS(td->control))
					|| (intr_state & X86_UHCI_STATUS_INTR_ERROR))
				{
					/* Error */
					printf("Interrupt ERROR: Td Control 0x%x, Header 0x%x\n", td->control, td->header);
				}

				/* Get next transaction */
				transactions = transactions->link;
			}

			/* Was it a completed transaction ? ? */
			if (completed)
			{
				/* Wuhuuu... */

				/* Mark EP Descriptor as free SEHR IMPORTANTE */
				qh->flags &= ~(X86_UHCI_QH_ACTIVE);

				/* Wake a node */
				scheduler_wakeup_one((addr_t*)qh);

				/* Remove from list */
				list_remove_by_node(transaction_list, ta);

				/* Cleanup node */
				kfree(ta);

				/* Done */
				break;
			}

			/* Get next head */
			n++;
			ta = list_get_node_by_id(transaction_list, 0, n);
		}
	}

	/* Resume Detected */
	if (intr_state & X86_UHCI_STATUS_RESUME_DETECT)
	{
		/* Set controller to working state :/ */
		if (controller->initialized != 0)
		{
			uint16_t temp = (X86_UHCI_CMD_CF | X86_UHCI_CMD_RUN | X86_UHCI_CMD_MAXPACKET64);
			uhci_write16(controller, X86_UHCI_REGISTER_COMMAND, temp);
		}
	}

	/* Host Error */
	if (intr_state & X86_UHCI_STATUS_HOST_SYSERR)
	{
		/* Reset Controller */
	}

	/* TD Processing Error */
	if (intr_state & X86_UHCI_STATUS_PROCESS_ERR)
	{
		/* Fatal Error 
		 * Unschedule TDs and restart controller */
		printf("UHCI: Processing Error :/ \n");
	}

	return X86_IRQ_HANDLED;
}
