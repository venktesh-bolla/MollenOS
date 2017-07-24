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
 * MollenOS MCore - Enhanced Host Controller Interface Driver
 * TODO:
 * - Power Management
 * - Isochronous Transport
 * - Transaction Translator Support
 */
//#define __TRACE

/* Includes 
 * - System */
#include <os/mollenos.h>
#include <os/condition.h>
#include <os/thread.h>
#include <os/utils.h>

#include "../common/manager.h"
#include "ehci.h"

/* Includes
 * - Library */
#include <ds/list.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* Globals
 * Use these for state-keeping the thread */
static UUId_t __GlbFinalizerThreadId = UUID_INVALID;
static Condition_t *__GlbFinalizerEvent = NULL;

/* FinalizerEntry 
 * Entry of the finalizer thread, this thread handles
 * all completed transactions to notify users */
int FinalizerEntry(void *Argument)
{
	// Variables
	ListNode_t *cNode = NULL;
	Mutex_t EventLock;

	// Unused
	_CRT_UNUSED(Argument);

	// Create the mutex
	MutexConstruct(&EventLock, MUTEX_PLAIN);

	// Forever-loop
	while (1) {
		// Wait for events
		ConditionWait(__GlbFinalizerEvent, &EventLock);

		// Iterate through all transactions for all controllers
		_foreach(cNode, UsbManagerGetControllers()) {
			// Instantiate a controller pointer
			EhciController_t *Controller = 
				(EhciController_t*)cNode->Data;
			
			// Iterate transactions
			foreach_nolink(tNode, Controller->QueueControl.TransactionList) {
				// Instantiate a transaction pointer
				UsbManagerTransfer_t *Transfer = 
					(UsbManagerTransfer_t*)tNode->Data;

				// Cleanup?
				if (Transfer->Cleanup) {
					// Temporary copy of pointer
					ListNode_t *Temp = tNode;

					// Notify requester and finalize
					OhciTransactionFinalize(Controller, Transfer, 1);
				
					// Remove from list (in-place, tricky)
					tNode = ListUnlinkNode(
						Controller->QueueControl.TransactionList,
						tNode);

					// Cleanup
					ListDestroyNode(
						Controller->QueueControl.TransactionList, 
						Temp);
				}
				else {
					tNode = ListNext(tNode);
				}
			}
		}
	}

	// Done
	return 0;
}

/* OnInterrupt
 * Is called when one of the registered devices
 * produces an interrupt. On successful handled
 * interrupt return OsSuccess, otherwise the interrupt
 * won't be acknowledged */
InterruptStatus_t
OnInterrupt(
	_In_ void *InterruptData)
{
	// Variables
	EhciController_t *Controller = NULL;
	reg32_t InterruptStatus;

	// Instantiate the pointer
	Controller = (EhciController_t*)InterruptData;

	// Calculate the kinds of interrupts this controller accepts
	InterruptStatus = 
		(Controller->OpRegisters->UsbStatus & Controller->OpRegisters->UsbIntr);

	// Trace
	TRACE("EHCI-Interrupt - Status 0x%x", InterruptStatus);

	// Was the interrupt even from this controller?
	if (!InterruptStatus) {
		return InterruptNotHandled;
	}

	// Transaction update, either error or completion
	if (InterruptStatus & (EHCI_STATUS_PROCESS | EHCI_STATUS_PROCESSERROR)) {
		EhciProcessTransfers(Controller);
	}

	// Hub change? We should enumerate ports and detect
	// which events occured
	if (InterruptStatus & EHCI_STATUS_PORTCHANGE) {
		EhciPortScan(Controller);
	}

	// HC Fatal Error
	// Clear all queued, reset controller
	if (InterruptStatus & EHCI_STATUS_HOSTERROR) {
		if (EhciRestart(Controller) != OsSuccess) {
			ERROR("EHCI-Failure: Failed to reset controller after Fatal Error");
		}
	}

	// Doorbell? Process transactions in progress
	if (InterruptStatus & EHCI_STATUS_ASYNC_DOORBELL) {
		EhciProcessDoorBell(Controller);
	}

	// Acknowledge the interrupt by clearing
	Controller->OpRegisters->UsbStatus = InterruptStatus;

	// Done
	return InterruptHandled;
}

/* OnTimeout
 * Is called when one of the registered timer-handles
 * times-out. A new timeout event is generated and passed
 * on to the below handler */
OsStatus_t
OnTimeout(
	_In_ UUId_t Timer,
	_In_ void *Data)
{
	return OsSuccess;
}

/* OnLoad
 * The entry-point of a driver, this is called
 * as soon as the driver is loaded in the system */
OsStatus_t OnLoad(void)
{
	// Create event semaphore
	__GlbFinalizerEvent = ConditionCreate();

	// Start finalizer thread
	__GlbFinalizerThreadId = ThreadCreate(FinalizerEntry, NULL);

	// Initialize the device manager here
	return UsbManagerInitialize();
}

/* OnUnload
 * This is called when the driver is being unloaded
 * and should free all resources allocated by the system */
OsStatus_t OnUnload(void)
{
	// Stop thread
	ThreadKill(__GlbFinalizerThreadId);

	// Cleanup semaphore
	ConditionDestroy(__GlbFinalizerEvent);

	// Cleanup the internal device manager
	return UsbManagerDestroy();
}

/* OnRegister
 * Is called when the device-manager registers a new
 * instance of this driver for the given device */
OsStatus_t OnRegister(MCoreDevice_t *Device)
{
	// Variables
	EhciController_t *Controller = NULL;
	
	// Register the new controller
	Controller = EhciControllerCreate(Device);

	// Sanitize
	if (Controller == NULL) {
		return OsError;
	}

	// Done - Register with service
	return UsbManagerCreateController(&Controller->Base);
}

/* OnUnregister
 * Is called when the device-manager wants to unload
 * an instance of this driver from the system */
OsStatus_t OnUnregister(MCoreDevice_t *Device)
{
	// Variables
	EhciController_t *Controller = NULL;
	
	// Lookup controller
	Controller = (EhciController_t*)UsbManagerGetController(Device->Id);

	// Sanitize lookup
	if (Controller == NULL) {
		return OsError;
	}

	// Unregister, then destroy
	UsbManagerDestroyController(&Controller->Base);

	// Destroy it
	return EhciControllerDestroy(Controller);
}

/* OnQuery
 * Occurs when an external process or server quries
 * this driver for data, this will correspond to the query
 * function that is defined in the contract */
OsStatus_t 
OnQuery(_In_ MContractType_t QueryType, 
		_In_ int QueryFunction, 
		_In_Opt_ RPCArgument_t *Arg0,
		_In_Opt_ RPCArgument_t *Arg1,
		_In_Opt_ RPCArgument_t *Arg2, 
		_In_ UUId_t Queryee, 
		_In_ int ResponsePort)
{
	// Variables
	UsbManagerTransfer_t *Transfer = NULL;
	EhciController_t *Controller = NULL;
	UUId_t Device = UUID_INVALID, Pipe = UUID_INVALID;
	OsStatus_t Result = OsError;

	// Instantiate some variables
	Device = (UUId_t)Arg0->Data.Value;
	Pipe = (UUId_t)Arg1->Data.Value;
	
	// Lookup controller
	Controller = (EhciController_t*)UsbManagerGetController(Device);

	// Sanitize we have a controller
	if (Controller == NULL) {
		// Null response
		return PipeSend(Queryee, ResponsePort, 
			(void*)&Result, sizeof(OsStatus_t));
	}

	switch (QueryFunction) {
		// Generic Queue
		case __USBHOST_QUEUETRANSFER: {
			// Create and setup new transfer
			Transfer = UsbManagerCreateTransfer(
				(UsbTransfer_t*)Arg2->Data.Buffer,
				Queryee, ResponsePort, Device, Pipe);

			// Queue the generic transfer
			return UsbQueueTransferGeneric(Transfer);
		} break;

		// Periodic Queue
		case __USBHOST_QUEUEPERIODIC: {
			// Variables
			UsbTransferResult_t ResPackage;

			// Create and setup new transfer
			Transfer = UsbManagerCreateTransfer(
				(UsbTransfer_t*)Arg2->Data.Buffer,
				Queryee, ResponsePort, Device, Pipe);

			// Queue the periodic transfer
			Result = UsbQueueTransferGeneric(Transfer);

			// Get id
			ResPackage.Id = Transfer->Id;
			ResPackage.BytesTransferred = 0;
			if (Result == OsSuccess) {
				ResPackage.Status = TransferNotProcessed;
			}
			else {
				ResPackage.Status = TransferInvalidData;
			}

			// Send back package
			return PipeSend(Queryee, ResponsePort, 
				(void*)&ResPackage, sizeof(UsbTransferResult_t));
		} break;

		// Dequeue Transfer
		case __USBHOST_DEQUEUEPERIODIC: {
			
			// Extract transfer-id
			UsbManagerTransfer_t *Transfer = NULL;
			UUId_t Id = (UUId_t)Arg1->Data.Value;

			// Lookup transfer by iterating through
			// available transfers
			foreach(tNode, Controller->QueueControl.TransactionList) {
				// Cast data to our type
				UsbManagerTransfer_t *NodeTransfer = 
					(UsbManagerTransfer_t*)tNode->Data;
				if (NodeTransfer->Id == Id) {
					Transfer = NodeTransfer;
					break;
				}
			}

			// Dequeue and send result back
			if (Transfer != NULL) {
				Result = UsbDequeueTransferGeneric(Transfer);
			}
		} break;

		// Reset port
		case __USBHOST_RESETPORT: {
			// Call reset procedure, then let it fall through
			// to QueryPort
			EhciPortPrepare(Controller, (int)Pipe);
		};
		// Query port
		case __USBHOST_QUERYPORT: {
			// Variables
			UsbHcPortDescriptor_t Descriptor;

			// Fill port descriptor
			EhciPortGetStatus(Controller, (int)Pipe, &Descriptor);

			// Send descriptor back
			return PipeSend(Queryee, ResponsePort, 
				(void*)&Descriptor, sizeof(UsbHcPortDescriptor_t));
		} break;

		// Fall-through, error
		default:
			break;
	}

	// Dunno, fall-through case
	// Return status response
	return PipeSend(Queryee, ResponsePort, (void*)&Result, sizeof(OsStatus_t));
}
