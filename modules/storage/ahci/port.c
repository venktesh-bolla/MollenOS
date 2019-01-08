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
 * Advanced Host Controller Interface Driver
 * TODO:
 *    - Port Multiplier Support
 *    - Power Management
 */
//#define __TRACE

#include <os/mollenos.h>
#include <os/utils.h>
#include "manager.h"
#include <threads.h>
#include <stdlib.h>
#include <assert.h>

AhciPort_t*
AhciPortCreate(
    _In_ AhciController_t*  Controller, 
    _In_ int                Port, 
    _In_ int                Index)
{
    AhciPort_t* AhciPort;

    // Sanitize the port, don't create an already existing
    // and make sure port is valid
    if (Controller->Ports[Port] != NULL || Port >= AHCI_MAX_PORTS) {
        if (Controller->Ports[Port] != NULL) {
            return Controller->Ports[Port];
        }
        return NULL;
    }

    AhciPort = (AhciPort_t*)malloc(sizeof(AhciPort_t));
    memset(AhciPort, 0, sizeof(AhciPort_t));

    AhciPort->Id           = Port;     // Sequential port number
    AhciPort->Index        = Index;    // Index in validity map
    AhciPort->Registers    = (AHCIPortRegisters_t*)((uintptr_t)Controller->Registers + AHCI_REGISTER_PORTBASE(Index)); // @todo port nr or bit index?
    AhciPort->Transactions = CollectionCreate(KeyInteger);
    return AhciPort;
}

/* AhciPortCleanup
 * Destroys a port, cleans up device, cleans up memory and resources */
void
AhciPortCleanup(
    _In_ AhciController_t* Controller, 
    _In_ AhciPort_t*       Port)
{
    CollectionItem_t* Node;

    // Null out the port-entry in the controller
    Controller->Ports[Port->Index] = NULL;

    // Cleanup all transactions
    _foreach(Node, Port->Transactions) {
        cnd_destroy((cnd_t*)Node->Data);
    }

    // Free the memory resources allocated
    if (Port->RecievedFisTable != NULL) {
        free((void*)Port->RecievedFisTable);
    }
    CollectionDestroy(Port->Transactions);
    free(Port);
}

/* AhciPortIdentifyDevice
 * Identifies connection on a port, and initializes connection/device */
OsStatus_t
AhciPortIdentifyDevice(
    _In_ AhciController_t* Controller, 
    _In_ AhciPort_t*       Port)
{
    // Detect present ports using
    // PxTFD.STS.BSY = 0, PxTFD.STS.DRQ = 0, and PxSSTS.DET = 3
    if (Port->Registers->TaskFileData & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)
        || (AHCI_PORT_STSS_DET(Port->Registers->AtaStatus) != AHCI_PORT_SSTS_DET_ENABLED)) {
        WARNING(" > no device detected 0x%x - 0x%x", Port->Registers->TaskFileData, Port->Registers->AtaStatus);
        return OsError;
    }

    TRACE(" > device present 0x%x on port %i", Port->Registers->Signature, Port->Id);
    Port->Connected = 1;
    return AhciManagerCreateDevice(Controller, Port);
}

/* AhciPortInitiateSetup
 * Initiates the setup sequence, this function needs at-least 500ms to complete before touching the port again. */
void
AhciPortInitiateSetup(
    _In_ AhciController_t* Controller,
    _In_ AhciPort_t*       Port)
{
    // Make sure the port has stopped
    Port->Registers->InterruptEnable = 0;
    if (Port->Registers->CommandAndStatus & (AHCI_PORT_ST | AHCI_PORT_FRE | AHCI_PORT_CR | AHCI_PORT_FR)) {
        Port->Registers->CommandAndStatus &= ~AHCI_PORT_ST;
    }
}

/* AhciPortFinishSetup
 * Finishes setup of port by completing a reset sequence. */
OsStatus_t
AhciPortFinishSetup(
    _In_ AhciController_t* Controller,
    _In_ AhciPort_t*       Port)
{
    int Hung = 0;

    // Step 1 -> wait for the command engine to stop by waiting for AHCI_PORT_CR to clear
    WaitForConditionWithFault(Hung, (Port->Registers->CommandAndStatus & AHCI_PORT_CR) == 0, 6, 100);
    if (Hung) {
        ERROR(" > failed to stop command engine: 0x%x", Port->Registers->CommandAndStatus);
        return OsError;
    }

    // Step 2 -> wait for the fis receive engine to stop by waiting for AHCI_PORT_FR to clear
    Port->Registers->CommandAndStatus &= ~AHCI_PORT_FRE;
    WaitForConditionWithFault(Hung, (Port->Registers->CommandAndStatus & AHCI_PORT_FR) == 0, 10, 10);
    if (Hung) {
        ERROR(" > failed to stop fis receive engine: 0x%x", Port->Registers->CommandAndStatus);
        return OsError;
    }

    // Software causes a port reset (COMRESET) by writing 1h to the PxSCTL.DET
    // Also disable slumber and partial state
    Port->Registers->AtaControl = AHCI_PORT_SCTL_DISABLE_PARTIAL_STATE | AHCI_PORT_SCTL_DISABLE_SLUMBER_STATE | AHCI_PORT_SCTL_RESET;
    thrd_sleepex(50);

    // After clearing PxSCTL.DET to 0h, software should wait for 
    // communication to be re-established as indicated by PxSSTS.DET being set to 3h.
    Port->Registers->AtaControl = AHCI_PORT_SCTL_DISABLE_PARTIAL_STATE | AHCI_PORT_SCTL_DISABLE_SLUMBER_STATE;
    WaitForConditionWithFault(Hung, AHCI_PORT_STSS_DET(Port->Registers->AtaStatus) == AHCI_PORT_SSTS_DET_ENABLED, 5, 10);
    if (Hung && Port->Registers->AtaStatus != 0) {
        // When PxSCTL.DET is set to 1h, the HBA shall reset PxTFD.STS to 7Fh and 
        // shall reset PxSSTS.DET to 0h. When PxSCTL.DET is set to 0h, upon receiving a 
        // COMINIT from the attached device, PxTFD.STS.BSY shall be set to 1 by the HBA.
        ERROR(" > failed to re-establish communication: 0x%x", Port->Registers->AtaStatus);
        if (AHCI_PORT_STSS_DET(Port->Registers->AtaStatus) == AHCI_PORT_SSTS_DET_NOPHYCOM) {
            return OsError;
        }
        else {
            return OsError;
        }
    }

    if ((AHCI_PORT_STSS_DET(Port->Registers->AtaStatus) == AHCI_PORT_SSTS_DET_ENABLED)) {
        // Handle staggered spin up support
        if (Controller->Registers->Capabilities & AHCI_CAPABILITIES_SSS) {
            Port->Registers->CommandAndStatus |= AHCI_PORT_SUD | AHCI_PORT_POD | AHCI_PORT_ICC_ACTIVE;
        }
    }

    // Then software should write all 1s to the PxSERR register to clear 
    // any bits that were set as part of the port reset.
    Port->Registers->AtaError        = 0xFFFFFFFF;
    Port->Registers->InterruptStatus = 0xFFFFFFFF;
    return OsSuccess;
}

/* AhciPortRebase
 * Rebases the port by setting up allocated memory tables and command memory. This can only be done
 * when the port is in a disabled state. */
void
AhciPortRebase(
    _In_ AhciController_t* Controller,
    _In_ AhciPort_t*       Port)
{
    uintptr_t CommandTablePointerPhysical = 0;
    int       i;

    // Initialize memory structures - both RecievedFIS and PRDT
    Port->CommandList  = (AHCICommandList_t*)((uint8_t*)Controller->CommandListBase + (sizeof(AHCICommandList_t) * Port->Id));
    Port->CommandTable = (void*)((uint8_t*)Controller->CommandTableBase + ((AHCI_COMMAND_TABLE_SIZE  * 32) * Port->Id));
    
    CommandTablePointerPhysical = Controller->CommandTableBasePhysical + ((AHCI_COMMAND_TABLE_SIZE * 32) * Port->Id);

    // Setup FIS Area
    if (Controller->Registers->Capabilities & AHCI_CAPABILITIES_FBSS) {
        Port->RecievedFis = (AHCIFis_t*)((uint8_t*)Controller->FisBase + (0x1000 * Port->Id));
    }
    else {
        Port->RecievedFis = (AHCIFis_t*)((uint8_t*)Controller->FisBase + (256 * Port->Id));
    }
    
    // Setup Recieved-FIS table
    Port->RecievedFisTable = (AHCIFis_t*)malloc(Controller->CommandSlotCount * AHCI_RECIEVED_FIS_SIZE);
    memset((void*)Port->RecievedFisTable, 0, Controller->CommandSlotCount * AHCI_RECIEVED_FIS_SIZE);
    
    // Iterate the 32 command headers
    for (i = 0; i < 32; i++) {
        Port->CommandList->Headers[i].Flags        = 0;
        Port->CommandList->Headers[i].TableLength  = 0;
        Port->CommandList->Headers[i].PRDByteCount = 0;

        // Load the command table address (physical)
        Port->CommandList->Headers[i].CmdTableBaseAddress = 
            LODWORD(CommandTablePointerPhysical);

        // Set command table address upper register if supported and we are in 64 bit
        if (Controller->Registers->Capabilities & AHCI_CAPABILITIES_S64A) {
            Port->CommandList->Headers[i].CmdTableBaseAddressUpper = 
                (sizeof(void*) > 4) ? HIDWORD(CommandTablePointerPhysical) : 0;
        }
        else {
            Port->CommandList->Headers[i].CmdTableBaseAddressUpper = 0;
        }
        CommandTablePointerPhysical += AHCI_COMMAND_TABLE_SIZE;
    }
}

/* AhciPortStart
 * Starts the port, the port must have been in a disabled state and must have been rebased at-least once. */
OsStatus_t
AhciPortStart(
    _In_ AhciController_t* Controller,
    _In_ AhciPort_t*       Port)
{
    uintptr_t PhysicalAddress;
    int       Hung = 0;

    // Setup the physical data addresses
    if (Controller->Registers->Capabilities & AHCI_CAPABILITIES_FBSS) {
        PhysicalAddress = Controller->FisBasePhysical + (0x1000 * Port->Id);
    }
    else {
        PhysicalAddress = Controller->FisBasePhysical + (256 * Port->Id);
    }

    Port->Registers->FISBaseAddress = LOWORD(PhysicalAddress);
    if (Controller->Registers->Capabilities & AHCI_CAPABILITIES_S64A) {
        Port->Registers->FISBaseAdressUpper = (sizeof(void*) > 4) ? HIDWORD(PhysicalAddress) : 0;
    }
    else {
        Port->Registers->FISBaseAdressUpper = 0;
    }

    PhysicalAddress                     = Controller->CommandListBasePhysical + (sizeof(AHCICommandList_t) * Port->Id);
    Port->Registers->CmdListBaseAddress = LODWORD(PhysicalAddress);
    if (Controller->Registers->Capabilities & AHCI_CAPABILITIES_S64A) {
        Port->Registers->CmdListBaseAddressUpper = (sizeof(void*) > 4) ? HIDWORD(PhysicalAddress) : 0;   
    }
    else {
        Port->Registers->CmdListBaseAddressUpper = 0;
    }

    // Make sure AHCI_PORT_CR is not set
    WaitForConditionWithFault(Hung, (Port->Registers->CommandAndStatus & AHCI_PORT_CR) == 0, 10, 25);
    if (Hung) {
        ERROR(" > command engine is hung: 0x%x", Port->Registers->CommandAndStatus);
        return OsTimeout;
    }
    
    // Setup the interesting interrupts we want
    Port->Registers->InterruptEnable = (reg32_t)(AHCI_PORT_IE_CPDE | AHCI_PORT_IE_TFEE
        | AHCI_PORT_IE_PCE | AHCI_PORT_IE_DSE | AHCI_PORT_IE_PSE | AHCI_PORT_IE_DHRE);

    // Set FRE before ST
    Port->Registers->CommandAndStatus |= AHCI_PORT_FRE;
    WaitForConditionWithFault(Hung, Port->Registers->CommandAndStatus & AHCI_PORT_FR, 6, 100);
    if (Hung) {
        ERROR(" > fis receive engine failed to start: 0x%x", Port->Registers->CommandAndStatus);
        return OsTimeout;
    }

    Port->Registers->CommandAndStatus |= AHCI_PORT_ST;
    WaitForConditionWithFault(Hung, Port->Registers->CommandAndStatus & AHCI_PORT_CR, 6, 100);
    if (Hung) {
        ERROR(" > command engine failed to start: 0x%x", Port->Registers->CommandAndStatus);
        return OsTimeout;
    }
    return AhciPortIdentifyDevice(Controller, Port);
}

/* AhciPortAcquireCommandSlot
 * Allocates an available command slot on a port returns index on success, otherwise -1 */
OsStatus_t
AhciPortAcquireCommandSlot(
    _In_  AhciController_t* Controller,
    _In_  AhciPort_t*       Port,
    _Out_ int*              Index)
{
    reg32_t    AtaActive = Port->Registers->AtaActive;
    OsStatus_t Status    = OsError;
    int        i;

    if (Index == NULL) {
        return OsError;
    }

    // Iterate possible command slots
    for (i = 0; i < (int)Controller->CommandSlotCount; i++) {
        // Check availability status on this command slot
        if ((Port->SlotStatus & (1 << i)) != 0 || (AtaActive & (1 << i)) != 0) {
            continue;
        }

        // Allocate slot and update the out variables
        Status              = OsSuccess;
        Port->SlotStatus    |= (1 << i);
        *Index              = i;
        break;
    }
    return Status;
}

/* AhciPortReleaseCommandSlot
 * Deallocates a previously allocated command slot */
void
AhciPortReleaseCommandSlot(
    _In_ AhciPort_t* Port, 
    _In_ int         Slot)
{
    Port->SlotStatus &= ~(1 << Slot);
}

/* AhciPortStartCommandSlot
 * Starts a command slot on the given port */
void
AhciPortStartCommandSlot(
    _In_ AhciPort_t* Port, 
    _In_ int         Slot)
{
    Port->Registers->CommandIssue = (1 << Slot);
}

/* AhciPortInterruptHandler
 * Port specific interrupt handler 
 * handles interrupt for a specific port */
void
AhciPortInterruptHandler(
    _In_ AhciController_t* Controller, 
    _In_ AhciPort_t*       Port)
{
    AhciTransaction_t* Transaction;
    reg32_t            InterruptStatus;
    reg32_t            DoneCommands;
    CollectionItem_t*  tNode;
    DataKey_t          Key;
    int                i;
    
    // Check interrupt services 
    // Cold port detect, recieved fis etc
    TRACE("AhciPortInterruptHandler(Port %i, Interrupt Status 0x%x)",
        Port->Id, Controller->InterruptResource.PortInterruptStatus[Port->Index]);

HandleInterrupt:
    InterruptStatus = Controller->InterruptResource.PortInterruptStatus[Port->Index];
    Controller->InterruptResource.PortInterruptStatus[Port->Index] = 0;
    
    // Check for errors status's
    if (InterruptStatus & (AHCI_PORT_IE_TFEE | AHCI_PORT_IE_HBFE 
        | AHCI_PORT_IE_HBDE | AHCI_PORT_IE_IFE | AHCI_PORT_IE_INFE)) {
        if (InterruptStatus & AHCI_PORT_IE_TFEE) {
            PrintTaskDataErrorString(HIBYTE(Port->Registers->TaskFileData));
        }
        else {
            ERROR("AHCI::Port ERROR %i, CMD: 0x%x, CI 0x%x, IE: 0x%x, IS 0x%x, TFD: 0x%x", Port->Id,
                Port->Registers->CommandAndStatus, Port->Registers->CommandIssue,
                Port->Registers->InterruptEnable, InterruptStatus, Port->Registers->TaskFileData);
        }
    }

    // Check for hot-plugs
    if (InterruptStatus & AHCI_PORT_IE_PCE) {
        // Determine whether or not there is a device connected
        // Detect present ports using
        // PxTFD.STS.BSY = 0, PxTFD.STS.DRQ = 0, and PxSSTS.DET = 3
        if (Port->Registers->TaskFileData & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ)
            || (AHCI_PORT_STSS_DET(Port->Registers->AtaStatus) != AHCI_PORT_SSTS_DET_ENABLED)) {
            AhciManagerRemoveDevice(Controller, Port);
            Port->Connected = 0;
        }
        else {
            AhciPortIdentifyDevice(Controller, Port);
        }
    }

    // Get completed commands, by using our own slot-status
    DoneCommands = Port->SlotStatus ^ Port->Registers->AtaActive;
    TRACE("DoneCommands(0x%x) <= SlotStatus(0x%x) ^ AtaActive(0x%x)", 
        DoneCommands, Port->SlotStatus, Port->Registers->AtaActive);

    // Check for command completion
    // by iterating through the command slots
    if (DoneCommands != 0) {
        for (i = 0; i < AHCI_MAX_PORTS; i++) {
            if (DoneCommands & (1 << i)) {
                Key.Value.Integer   = i;
                tNode               = CollectionGetNodeByKey(Port->Transactions, Key, 0);
                
                assert(tNode != NULL);
                Transaction = (AhciTransaction_t*)tNode;

                // Remove and destroy node
                CollectionRemoveByNode(Port->Transactions, tNode);
                CollectionDestroyNode(Port->Transactions, tNode);

                // Copy data over - we make a copy of the recieved fis
                // to make the slot reusable as quickly as possible
                memcpy((void*)&Port->RecievedFisTable[i], (void*)Port->RecievedFis, sizeof(AHCIFis_t));
                AhciCommandFinish(Transaction);
            }
        }
    }

    // Re-handle?
    if (Controller->InterruptResource.PortInterruptStatus[Port->Index] != 0) {
        goto HandleInterrupt;
    }
}
