/*
 * Copyright 2002-04, Thomas Kurschel. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*
	Generic ATA adapter library.
*/

#include <KernelExport.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <ata_adapter.h>
#include <tracing.h>
#include <AutoDeleterOS.h>

#define debug_level_flow 0
#define debug_level_error 3
#define debug_level_info 3

#define DEBUG_MSG_PREFIX "ATA PCI -- "

#include "wrapper.h"

#define TRACE dprintf

#define INTERRUPT_TRACING 0
#if INTERRUPT_TRACING
	#define TRACE_INT(a...) 	ktrace_printf(a)
#else
	#define TRACE_INT(a...)
#endif


#if ATA_DMA_TRACING
	#define TRACE_DMA(x...)		ktrace_printf(x)
#else
	#define TRACE_DMA(x...)
#endif

static ata_for_controller_interface *sATA;
static device_manager_info *sDeviceManager;

static area_id sRegsArea = -1;
uint8 *sMappedRegs;

static void MmioOp(uint64 offset, uint32 &val, uint32 size, bool isWrite)
{
	//dprintf("MmioOp(%#" B_PRIx64 ", %" B_PRIu32 ", %d)\n", offset, size, isWrite);
	
	if (sMappedRegs == 0) {
		phys_addr_t regs = 0x40000000;
		size_t regsSize = 0x3000;
		sRegsArea = map_physical_memory("ATA MMIO", regs, regsSize,
			B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			(void**)&sMappedRegs);
	}

	uint8 *adr = sMappedRegs + offset - 0x1000;

	if (!isWrite) {
		switch (size) {
			case 1: val = *(vuint8*)adr; break;
			case 2: val = *(vuint16*)adr; break;
			case 4: val = *(vuint32*)adr; break;
			default: panic("illegal size");
		}
	} else {
		switch (size) {
			case 1: *(vuint8*)adr = val; break;
			case 2: *(vuint16*)adr = val; break;
			case 4: *(vuint32*)adr = val; break;
			default: panic("illegal size");
		}
	}
	//dprintf("-MmioOp\n");
}

static void Write8(uint64 offset, uint32 val) {MmioOp(offset, val, 1, true);}
static void Write16(uint64 offset, uint32 val) {MmioOp(offset, val, 2, true);}
static void Write32(uint64 offset, uint32 val) {MmioOp(offset, val, 4, true);}
static uint8 Read8(uint64 offset) {uint32 val; MmioOp(offset, val, 1, false); return (uint8)val;}
static uint16 Read16(uint64 offset) {uint32 val; MmioOp(offset, val, 2, false); return (uint16)val;}
static uint32 Read32(uint64 offset) {uint32 val; MmioOp(offset, val, 4, false); return val;}


static void
set_channel(ata_adapter_channel_info *channel, ata_channel ataChannel)
{
	channel->ataChannel = ataChannel;
}


static status_t
ata_adapter_write_command_block_regs(ata_adapter_channel_info *channel,
	ata_task_file *tf, ata_reg_mask mask)
{
	//pci_device_module_info *pci = channel->pci;
	//pci_device *device = channel->device;
	int i;

	uint16 ioaddr = channel->command_block_base;

	if (channel->lost)
		return B_ERROR;

	for (i = 0; i < 7; i++) {
		// LBA48 registers must be written twice
		if (((1 << (i + 7)) & mask) != 0) {
			SHOW_FLOW( 4, "%x->HI(%x)", tf->raw.r[i + 7], i );
			Write8(ioaddr + 1 + i, tf->raw.r[i + 7]);
		}

		if (((1 << i) & mask) != 0) {
			SHOW_FLOW( 4, "%x->LO(%x)", tf->raw.r[i], i );
			Write8(ioaddr + 1 + i, tf->raw.r[i]);
		}
	}

	return B_OK;
}


static status_t
ata_adapter_read_command_block_regs(ata_adapter_channel_info *channel,
	ata_task_file *tf, ata_reg_mask mask)
{
	//pci_device_module_info *pci = channel->pci;
	//pci_device *device = channel->device;
	int i;

	uint16 ioaddr = channel->command_block_base;

	if (channel->lost)
		return B_ERROR;

	for (i = 0; i < 7; i++) {
		if (((1 << i) & mask) != 0) {
			tf->raw.r[i] = Read8(ioaddr + 1 + i);
			SHOW_FLOW( 4, "%x: %x", i, (int)tf->raw.r[i] );
		}
	}

	return B_OK;
}


static uint8
ata_adapter_get_altstatus(ata_adapter_channel_info *channel)
{
	//pci_device_module_info *pci = channel->pci;
	//pci_device *device = channel->device;
	uint16 altstatusaddr = channel->control_block_base;

	if (channel->lost)
		return 0x01; // Error bit

	return Read8(altstatusaddr);
}


static status_t
ata_adapter_write_device_control(ata_adapter_channel_info *channel, uint8 val)
{
	//pci_device_module_info *pci = channel->pci;
	//pci_device *device = channel->device;
	uint16 device_control_addr = channel->control_block_base;

	SHOW_FLOW(3, "%x", (int)val);

	if (channel->lost)
		return B_ERROR;

	Write8(device_control_addr, val);

	return B_OK;
}


static status_t
ata_adapter_write_pio(ata_adapter_channel_info *channel, uint16 *data,
	int count, bool force_16bit)
{
	//pci_device_module_info *pci = channel->pci;
	//pci_device *device = channel->device;

	uint16 ioaddr = channel->command_block_base;

	if (channel->lost)
		return B_ERROR;

	force_16bit = true;

	if ((count & 1) != 0 || force_16bit) {
		for (; count > 0; --count)
			Write16(ioaddr, *(data++));
	} else {
		uint32 *cur_data = (uint32 *)data;

		for (; count > 0; count -= 2)
			Write32(ioaddr, *(cur_data++));
	}

	return B_OK;
}


static status_t
ata_adapter_read_pio(ata_adapter_channel_info *channel, uint16 *data,
	int count, bool force_16bit)
{
	//pci_device_module_info *pci = channel->pci;
	//pci_device *device = channel->device;

	uint16 ioaddr = channel->command_block_base;

	if (channel->lost)
		return B_ERROR;

	force_16bit = true;

	if ((count & 1) != 0 || force_16bit) {
		for (; count > 0; --count)
			*(data++) = Read16(ioaddr );
	} else {
		uint32 *cur_data = (uint32 *)data;

		for (; count > 0; count -= 2)
			*(cur_data++) = Read32(ioaddr);
	}

	return B_OK;
}


static int32
ata_adapter_inthand(void *arg)
{
	ata_adapter_channel_info *channel = (ata_adapter_channel_info *)arg;
	//pci_device_module_info *pci = channel->pci;
	//pci_device *device = channel->device;
	uint8 statusATA, statusBM;

	TRACE_INT("ata_adapter_inthand\n");

	// need to read bus master status first, because some controllers
	// will clear the interrupt status bit once ATA status is read
	statusBM = Read8(channel->bus_master_base
		+ ATA_BM_STATUS_REG);
	TRACE_INT("ata_adapter_inthand: BM-status 0x%02x\n", statusBM);

	// test if the interrupt was really generated by our controller
	if (statusBM & ATA_BM_STATUS_INTERRUPT) {
		// read ATA status register to acknowledge interrupt
		statusATA = Read8(channel->command_block_base + 7);
		TRACE_INT("ata_adapter_inthand: ATA-status 0x%02x\n", statusATA);

		// clear pending PCI bus master DMA interrupt, for those
		// controllers who don't clear it themselves
		Write8(channel->bus_master_base + ATA_BM_STATUS_REG,
			(statusBM & 0xf8) | ATA_BM_STATUS_INTERRUPT);

		if (!channel->dmaing) {
			// we check this late so that potential spurious interrupts
			// are acknoledged above
			TRACE_INT("ata_adapter_inthand: no DMA transfer active\n");
			return B_UNHANDLED_INTERRUPT;
		}

		// signal interrupt to ATA stack
		return sATA->interrupt_handler(channel->ataChannel, statusATA);
	} else {
		TRACE_INT("ata_adapter_inthand: not BM\n");
		return B_UNHANDLED_INTERRUPT;
	}
}


static status_t
ata_adapter_prepare_dma(ata_adapter_channel_info *channel,
	const physical_entry *sgList, size_t sgListCount, bool writeToDevice)
{
	//pci_device_module_info *pci = channel->pci;
	//pci_device *device = channel->device;
	uint8 command;
	uint8 status;
	prd_entry *prd = channel->prdt;
	int i;

	TRACE_DMA("ata_adapter: prepare_dma (%s) %lu entrys:\n",
		writeToDevice ? "write" : "read", sgListCount);

	for (i = sgListCount - 1, prd = channel->prdt; i >= 0; --i, ++prd, ++sgList) {
		prd->address = B_HOST_TO_LENDIAN_INT32(sgList->address);
		// 0 means 64K - this is done automatically be discarding upper 16 bits
		prd->count = B_HOST_TO_LENDIAN_INT16((uint16)sgList->size);
		prd->EOT = i == 0;

		TRACE_DMA("ata_adapter: %#" B_PRIxPHYSADDR ", %" B_PRIuPHYSADDR " => "
			"%#010" B_PRIx32 ", %" B_PRIu16 ", %d\n",
			sgList->address, sgList->size,
			prd->address, prd->count, prd->EOT);
		SHOW_FLOW( 4, "%#010" B_PRIx32 ", %" B_PRIu16 ", %d",
			prd->address, prd->count, prd->EOT);
	}

	Write32(channel->bus_master_base + ATA_BM_PRDT_ADDRESS,
		(Read32(channel->bus_master_base + ATA_BM_PRDT_ADDRESS) & 3)
		| (B_HOST_TO_LENDIAN_INT32(channel->prdt_phys) & ~3));

	// reset interrupt and error signal
	status = Read8(channel->bus_master_base
		+ ATA_BM_STATUS_REG) | ATA_BM_STATUS_INTERRUPT | ATA_BM_STATUS_ERROR;
	Write8(
		channel->bus_master_base + ATA_BM_STATUS_REG, status);

	// set data direction
	command = Read8(channel->bus_master_base
		+ ATA_BM_COMMAND_REG);
	if (writeToDevice)
		command &= ~ATA_BM_COMMAND_READ_FROM_DEVICE;
	else
		command |= ATA_BM_COMMAND_READ_FROM_DEVICE;

	Write8(channel->bus_master_base + ATA_BM_COMMAND_REG,
		command);

	return B_OK;
}


static status_t
ata_adapter_start_dma(ata_adapter_channel_info *channel)
{
	//pci_device_module_info *pci = channel->pci;
	//pci_device *device = channel->device;
	uint8 command;

	command = Read8(channel->bus_master_base
		+ ATA_BM_COMMAND_REG);

	command |= ATA_BM_COMMAND_START_STOP;

	channel->dmaing = true;

	Write8(channel->bus_master_base + ATA_BM_COMMAND_REG,
		command);

	return B_OK;
}


static status_t
ata_adapter_finish_dma(ata_adapter_channel_info *channel)
{
	//pci_device_module_info *pci = channel->pci;
	//pci_device *device = channel->device;
	uint8 command;
	uint8 status;

	// read BM status first
	status = Read8(channel->bus_master_base
		+ ATA_BM_STATUS_REG);

	// stop DMA engine, this also clears ATA_BM_STATUS_ACTIVE
	// in the BM status register
	command = Read8(channel->bus_master_base
		+ ATA_BM_COMMAND_REG);
	Write8(channel->bus_master_base + ATA_BM_COMMAND_REG,
		command & ~ATA_BM_COMMAND_START_STOP);
	channel->dmaing = false;

	// reset error flag
	Write8(channel->bus_master_base + ATA_BM_STATUS_REG,
		status | ATA_BM_STATUS_ERROR);

	if ((status & ATA_BM_STATUS_ACTIVE) != 0)
		return B_DEV_DATA_OVERRUN;

	if ((status & ATA_BM_STATUS_ERROR) != 0)
		return B_ERROR;

	return B_OK;
}


static status_t
ata_adapter_init_channel(device_node *node,
	ata_adapter_channel_info **cookie, size_t total_data_size,
	int32 (*inthand)(void *arg))
{
	ata_adapter_controller_info *controller;
	ata_adapter_channel_info *channel;
	uint16 command_block_base, control_block_base;
	uint8 intnum;
	int prdt_size;
	physical_entry pe[1];
	uint8 channel_index;
	status_t res;

	TRACE("PCI-ATA: init channel...\n");

#if 0
	if (1 /* debug */){
		uint8 bus, device, function;
		uint16 vendorID, deviceID;
		sDeviceManager->get_attr_uint8(node, PCI_DEVICE_BUS_ITEM, &bus, true);
		sDeviceManager->get_attr_uint8(node, PCI_DEVICE_DEVICE_ITEM, &device, true);
		sDeviceManager->get_attr_uint8(node, PCI_DEVICE_FUNCTION_ITEM, &function, true);
		sDeviceManager->get_attr_uint16(node, PCI_DEVICE_VENDOR_ID_ITEM, &vendorID, true);
		sDeviceManager->get_attr_uint16(node, PCI_DEVICE_DEVICE_ID_ITEM, &deviceID, true);
		TRACE("PCI-ATA: bus %3d, device %2d, function %2d: vendor %04x, device %04x\n",
			bus, device, function, vendorID, deviceID);
	}
#endif

	// get device data
	if (sDeviceManager->get_attr_uint16(node, ATA_ADAPTER_COMMAND_BLOCK_BASE, &command_block_base, false) != B_OK
		|| sDeviceManager->get_attr_uint16(node, ATA_ADAPTER_CONTROL_BLOCK_BASE, &control_block_base, false) != B_OK
		|| sDeviceManager->get_attr_uint8(node, ATA_ADAPTER_INTNUM, &intnum, true) != B_OK
		|| sDeviceManager->get_attr_uint8(node, ATA_ADAPTER_CHANNEL_INDEX, &channel_index, false) != B_OK)
		return B_ERROR;

	{
		device_node *parent = sDeviceManager->get_parent_node(node);
		sDeviceManager->get_driver(parent, NULL, (void **)&controller);
		sDeviceManager->put_node(parent);
	}

	channel = (ata_adapter_channel_info *)malloc(total_data_size);
	if (channel == NULL) {
		res = B_NO_MEMORY;
		goto err;
	}

	TRACE("PCI-ATA: channel index %d\n", channel_index);

	channel->node = node;
	channel->pci = controller->pci;
	channel->device = controller->device;
	channel->lost = false;
	channel->command_block_base = command_block_base;
	channel->control_block_base = control_block_base;
	channel->bus_master_base = controller->bus_master_base + (channel_index * 8);
	channel->intnum = intnum;
	channel->dmaing = false;
	channel->inthand = inthand;

	TRACE("PCI-ATA: bus master base %#x\n", channel->bus_master_base);

	// PRDT must be contiguous, dword-aligned and must not cross 64K boundary
// TODO: Where's the handling for the 64 K boundary? create_area_etc() can be
// used.
	prdt_size = (ATA_ADAPTER_MAX_SG_COUNT * sizeof( prd_entry ) + (B_PAGE_SIZE - 1)) & ~(B_PAGE_SIZE - 1);
	channel->prd_area = create_area("prd", (void **)&channel->prdt, B_ANY_KERNEL_ADDRESS,
		prdt_size, B_32_BIT_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (channel->prd_area < B_OK) {
		res = channel->prd_area;
		goto err2;
	}

	get_memory_map(channel->prdt, prdt_size, pe, 1);
	channel->prdt_phys = pe[0].address;

	SHOW_FLOW(3, "virt=%p, phys=%x", channel->prdt, (int)channel->prdt_phys);

	res = install_io_interrupt_handler(channel->intnum,
		inthand, channel, 0);

	if (res < 0) {
		SHOW_ERROR(0, "couldn't install irq handler @%d", channel->intnum);
		goto err3;
	}

	TRACE("PCI-ATA: init channel done\n");

	// disable interrupts
	ata_adapter_write_device_control(channel, ATA_DEVICE_CONTROL_BIT3 | ATA_DEVICE_CONTROL_DISABLE_INTS);

	*cookie = channel;

	return B_OK;

err3:
	delete_area(channel->prd_area);
err2:
err:
	free(channel);

	return res;
}


static void
ata_adapter_uninit_channel(ata_adapter_channel_info *channel)
{
	// disable IRQs
	ata_adapter_write_device_control(channel, ATA_DEVICE_CONTROL_BIT3 | ATA_DEVICE_CONTROL_DISABLE_INTS);

	// catch spurious interrupt
	// (some controllers generate an IRQ when you _disable_ interrupts,
	//  they are delayed by less then 40 µs, so 1 ms is safe)
	snooze(1000);

	remove_io_interrupt_handler(channel->intnum, channel->inthand, channel);

	delete_area(channel->prd_area);
	free(channel);
}


static void
ata_adapter_channel_removed(ata_adapter_channel_info *channel)
{
	SHOW_FLOW0( 3, "" );

	if (channel != NULL)
		// disable access instantly
		atomic_or((int32*)&channel->lost, 1);
}


/** publish node of ata channel */

static status_t
ata_adapter_publish_channel(device_node *controller_node,
	const char *channel_module_name, uint16 command_block_base,
	uint16 control_block_base, uint8 intnum, bool can_dma,
	uint8 channel_index, const char *name, const io_resource *resources,
	device_node **node)
{
	char prettyName[25];
	sprintf(prettyName, "ATA Channel %" B_PRIu8, channel_index);

	device_attr attrs[] = {
		// info about ourself and our consumer
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ string: prettyName }},
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ string: ATA_FOR_CONTROLLER_MODULE_NAME }},

		// private data to identify channel
		{ ATA_ADAPTER_COMMAND_BLOCK_BASE, B_UINT16_TYPE,
			{ ui16: command_block_base }},
		{ ATA_ADAPTER_CONTROL_BLOCK_BASE, B_UINT16_TYPE,
			{ ui16: control_block_base }},
		{ ATA_CONTROLLER_CAN_DMA_ITEM, B_UINT8_TYPE, { ui8: can_dma }},
		{ ATA_ADAPTER_INTNUM, B_UINT8_TYPE, { ui8: intnum }},
		{ ATA_ADAPTER_CHANNEL_INDEX, B_UINT8_TYPE, { ui8: channel_index }},
		{ NULL }
	};

	SHOW_FLOW0(2, "");

	return sDeviceManager->register_node(controller_node, channel_module_name, attrs,
		resources, node);
}


/** detect IDE channel */

static status_t
ata_adapter_detect_channel(pci_device_module_info *pci, pci_device *pci_device,
	device_node *controller_node, const char *channel_module_name,
	bool controller_can_dma, uint16 command_block_base, uint16 control_block_base,
	uint16 bus_master_base, uint8 intnum, uint8 channel_index, const char *name,
	device_node **node, bool supports_compatibility_mode)
{
	uint8 api;
	uint16 pcicmdOld;
	uint16 pcicmdNew;
	uint16 pciVendor;

	SHOW_FLOW0( 3, "" );

	// if channel works in compatibility mode, addresses and interrupt are fixed
	api = pci->read_pci_config(pci_device, PCI_class_api, 1);

	if (supports_compatibility_mode
		&& channel_index == 0 && (api & PCI_ide_primary_native) == 0) {
		command_block_base = 0x1f0;
		control_block_base = 0x3f6;
		intnum = 14;
		TRACE("PCI-ATA: Controller in legacy mode: cmd %#x, ctrl %#x, irq %d\n",
			  command_block_base, control_block_base, intnum);
	} else if (supports_compatibility_mode
		&& channel_index == 1 && (api & PCI_ide_secondary_native) == 0) {
		command_block_base = 0x170;
		control_block_base = 0x376;
		intnum = 15;
		TRACE("PCI-ATA: Controller in legacy mode: cmd %#x, ctrl %#x, irq %d\n",
			  command_block_base, control_block_base, intnum);
	} else {
		if (command_block_base == 0 || control_block_base == 0) {
			TRACE("PCI-ATA: Command/Control Block base is not configured\n");
			return B_ERROR;
		}
		if (intnum == 0 || intnum == 0xff) {
			TRACE("PCI-ATA: Interrupt is not configured\n");
			return B_ERROR;
		}

		// historically, they start at 3f6h/376h, but PCI spec requires registers
		// to be aligned at 4 bytes, so only 3f4h/374h can be specified; thus
		// PCI IDE defines that control block starts at offset 2
		control_block_base += 2;
		TRACE("PCI-ATA: Controller in native mode: cmd %#x, ctrl %#x, irq %d\n",
			  command_block_base, control_block_base, intnum);
	}


	// this should be done in ata_adapter_init_controller but there is crashes
	pcicmdOld = pcicmdNew = pci->read_pci_config(pci_device, PCI_command, 2);
	if ((pcicmdNew & (1 << 10)) != 0) {
		TRACE("PCI-ATA: enabling interrupts\n");
		pcicmdNew &= ~(1 << 10);
	}
	if ((pcicmdNew & PCI_command_io) == 0) {
		TRACE("PCI-ATA: enabling io decoder\n");
		pcicmdNew |= PCI_command_io;
	}
	if ((pcicmdNew & PCI_command_master) == 0) {
		TRACE("PCI-ATA: enabling bus mastering\n");
		pcicmdNew |= PCI_command_master;
	}
	if (pcicmdOld != pcicmdNew) {
		pci->write_pci_config(pci_device, PCI_command, 2, pcicmdNew);
		TRACE("PCI-ATA: pcicmd changed from 0x%04x to 0x%04x\n",
			pcicmdOld, pcicmdNew);
	}


	if (supports_compatibility_mode) {
		// read status of primary(!) channel to detect simplex
		uint8 status = Read8(bus_master_base
			+ ATA_BM_STATUS_REG);

		if (status & ATA_BM_STATUS_SIMPLEX_DMA && channel_index != 0) {
			// in simplex mode, channels cannot operate independantly of each other;
			// we simply disable bus mastering of second channel to satisfy that;
			// better were to use a controller lock, but this had to be done in the IDE
			// bus manager, and I don't see any reason to add extra code for old
			// simplex controllers

			// Intel controllers use this bit for something else and are not simplex.
			pciVendor = pci->read_pci_config(pci_device, PCI_vendor_id, 2);

			if (pciVendor != 0x8086) {
				TRACE("PCI-ATA: Simplex controller - disabling DMA of secondary channel\n");
				controller_can_dma = false;
			} else {
				TRACE("PCI-ATA: Simplex bit ignored - Intel controller\n");
			}
		}
	}

	{
		io_resource resources[3] = {
			{ B_IO_PORT, command_block_base, 8 },
			{ B_IO_PORT, control_block_base, 1 },
			{}
		};

		return ata_adapter_publish_channel(controller_node, channel_module_name,
			command_block_base, control_block_base, intnum, controller_can_dma,
			channel_index, name, resources, node);
	}
}


static status_t
ata_adapter_init_controller(device_node *node,
	ata_adapter_controller_info **cookie, size_t total_data_size)
{
	pci_device_module_info *pci;
	pci_device *device;
	ata_adapter_controller_info *controller;
	uint16 bus_master_base;

	// get device data
	if (sDeviceManager->get_attr_uint16(node, ATA_ADAPTER_BUS_MASTER_BASE, &bus_master_base, false) != B_OK)
		return B_ERROR;

	{
		device_node *parent = sDeviceManager->get_parent_node(node);
		sDeviceManager->get_driver(parent, (driver_module_info **)&pci, (void **)&device);
		sDeviceManager->put_node(parent);
	}

	controller = (ata_adapter_controller_info *)malloc(total_data_size);
	if (controller == NULL)
		return B_NO_MEMORY;

#if 0
	pcicmdOld = pcicmdNew = pci->read_pci_config(node, PCI_command, 2);
	if ((pcicmdNew & PCI_command_io) == 0) {
		TRACE("PCI-ATA: adapter init: enabling io decoder\n");
		pcicmdNew |= PCI_command_io;
	}
	if ((pcicmdNew & PCI_command_master) == 0) {
		TRACE("PCI-ATA: adapter init: enabling bus mastering\n");
		pcicmdNew |= PCI_command_master;
	}
	if (pcicmdOld != pcicmdNew) {
		pci->write_pci_config(node, PCI_command, 2, pcicmdNew);
		TRACE("PCI-ATA: adapter init: pcicmd old 0x%04x, new 0x%04x\n",
			pcicmdOld, pcicmdNew);
	}
#endif

	controller->node = node;
	controller->pci = pci;
	controller->device = device;
	controller->lost = false;
	controller->bus_master_base = bus_master_base;

	*cookie = controller;

	return B_OK;
}


static void
ata_adapter_uninit_controller(ata_adapter_controller_info *controller)
{
	free(controller);
}


static void
ata_adapter_controller_removed(ata_adapter_controller_info *controller)
{
	SHOW_FLOW0(3, "");

	if (controller != NULL) {
		// disable access instantly; unit_device takes care of unregistering ioports
		atomic_or((int32*)&controller->lost, 1);
	}
}


/** publish node of ata controller */

static status_t
ata_adapter_publish_controller(device_node *parent, uint16 bus_master_base,
	io_resource *resources, const char *controller_driver,
	const char *controller_driver_type, const char *controller_name, bool can_dma,
	bool can_cq, uint32 dma_alignment, uint32 dma_boundary, uint32 max_sg_block_size,
	device_node **node)
{
	device_attr attrs[] = {
		// properties of this controller for ata bus manager
		// there are always max. 2 devices
		// (unless this is a Compact Flash Card with a built-in IDE controller,
		//  which has exactly 1 device)
		{ ATA_CONTROLLER_MAX_DEVICES_ITEM, B_UINT8_TYPE, { ui8: 2 }},
		// of course we can DMA
		{ ATA_CONTROLLER_CAN_DMA_ITEM, B_UINT8_TYPE, { ui8: can_dma }},
		// choose any name here
		{ ATA_CONTROLLER_CONTROLLER_NAME_ITEM, B_STRING_TYPE,
			{ string: controller_name }},

		// DMA properties
		// data must be word-aligned;
		// warning: some controllers are more picky!
		{ B_DMA_ALIGNMENT, B_UINT32_TYPE, { ui32: dma_alignment /*1*/}},
		// one S/G block must not cross 64K boundary
		{ B_DMA_BOUNDARY, B_UINT32_TYPE, { ui32: dma_boundary/*0xffff*/ }},
		// max size of S/G block is 16 bits with zero being 64K
		{ B_DMA_MAX_SEGMENT_BLOCKS, B_UINT32_TYPE,
			{ ui32: max_sg_block_size/*0x10000*/ }},
		{ B_DMA_MAX_SEGMENT_COUNT, B_UINT32_TYPE,
			{ ui32: ATA_ADAPTER_MAX_SG_COUNT }},
		{ B_DMA_HIGH_ADDRESS, B_UINT64_TYPE, { ui64: 0x100000000LL }},

		// private data to find controller
		{ ATA_ADAPTER_BUS_MASTER_BASE, B_UINT16_TYPE, { ui16: bus_master_base }},
		{ NULL }
	};

	SHOW_FLOW0( 2, "" );

	return sDeviceManager->register_node(parent, controller_driver, attrs, resources, node);
}


/** detect pure IDE controller, i.e. without channels */

static status_t
ata_adapter_detect_controller(pci_device_module_info *pci, pci_device *pci_device,
	device_node *parent, uint16 bus_master_base, const char *controller_driver,
	const char *controller_driver_type, const char *controller_name, bool can_dma,
	bool can_cq, uint32 dma_alignment, uint32 dma_boundary, uint32 max_sg_block_size,
	device_node **node)
{
	io_resource resources[2] = {
		{ B_IO_PORT, bus_master_base, 16 },
		{}
	};

	SHOW_FLOW0( 3, "" );

	if (bus_master_base == 0) {
		TRACE("PCI-ATA: Controller detection failed! bus master base not configured\n");
		return B_ERROR;
	}

	return ata_adapter_publish_controller(parent, bus_master_base, resources,
		controller_driver, controller_driver_type, controller_name, can_dma, can_cq,
		dma_alignment, dma_boundary, max_sg_block_size, node);
}


static status_t
ata_adapter_probe_controller(device_node *parent, const char *controller_driver,
	const char *controller_driver_type, const char *controller_name,
	const char *channel_module_name, bool can_dma, bool can_cq, uint32 dma_alignment,
	uint32 dma_boundary, uint32 max_sg_block_size, bool supports_compatibility_mode)
{
	pci_device_module_info *pci;
	pci_device *device;
	uint16 command_block_base[2];
	uint16 control_block_base[2];
	uint16 bus_master_base;
	device_node *controller_node;
	device_node *channels[2];
	uint8 intnum;
	status_t res;

	SHOW_FLOW0( 3, "" );

	sDeviceManager->get_driver(parent, (driver_module_info **)&pci, (void **)&device);
/*
	command_block_base[0] = pci->read_pci_config(device, PCI_base_registers, 4 );
	control_block_base[0] = pci->read_pci_config(device, PCI_base_registers + 4, 4);
	command_block_base[1] = pci->read_pci_config(device, PCI_base_registers + 8, 4);
	control_block_base[1] = pci->read_pci_config(device, PCI_base_registers + 12, 4);
	bus_master_base = pci->read_pci_config(device, PCI_base_registers + 16, 4);
	intnum = pci->read_pci_config(device, PCI_interrupt_line, 1);

	command_block_base[0] &= PCI_address_io_mask;
	control_block_base[0] &= PCI_address_io_mask;
	command_block_base[1] &= PCI_address_io_mask;
	control_block_base[1] &= PCI_address_io_mask;
	bus_master_base &= PCI_address_io_mask;
*/

	command_block_base[0] = 0x1000;
	control_block_base[0] = 0x2000;
	command_block_base[1] = 0;
	control_block_base[1] = 0;
	bus_master_base = 0x3000;
	intnum = 4;

	res = ata_adapter_detect_controller(pci, device, parent, bus_master_base,
		controller_driver, controller_driver_type, controller_name, can_dma,
		can_cq, dma_alignment, dma_boundary, max_sg_block_size, &controller_node);
	// don't register if controller is already registered!
	// (happens during rescan; registering new channels would kick out old channels)
	if (res != B_OK || controller_node == NULL)
		return res;

	// ignore errors during registration of channels - could be a simple rescan collision
	ata_adapter_detect_channel(pci, device, controller_node, channel_module_name,
		can_dma, command_block_base[0], control_block_base[0], bus_master_base,
		intnum, 0, "Primary Channel", &channels[0], supports_compatibility_mode);

	ata_adapter_detect_channel(pci, device, controller_node, channel_module_name,
		can_dma, command_block_base[1], control_block_base[1], bus_master_base,
		intnum, 1, "Secondary Channel", &channels[1], supports_compatibility_mode);

	return B_OK;
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		case B_MODULE_UNINIT:
			return B_OK;

		default:
			return B_ERROR;
	}
}


module_dependency module_dependencies[] = {
	{ ATA_FOR_CONTROLLER_MODULE_NAME, (module_info **)&sATA },
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info **)&sDeviceManager },
	{}
};


static ata_adapter_interface adapter_interface = {
	{
		ATA_ADAPTER_MODULE_NAME,
		0,
		std_ops
	},

	set_channel,

	ata_adapter_write_command_block_regs,
	ata_adapter_read_command_block_regs,

	ata_adapter_get_altstatus,
	ata_adapter_write_device_control,

	ata_adapter_write_pio,
	ata_adapter_read_pio,

	ata_adapter_prepare_dma,
	ata_adapter_start_dma,
	ata_adapter_finish_dma,

	ata_adapter_inthand,

	ata_adapter_init_channel,
	ata_adapter_uninit_channel,
	ata_adapter_channel_removed,

	ata_adapter_publish_channel,
	ata_adapter_detect_channel,

	ata_adapter_init_controller,
	ata_adapter_uninit_controller,
	ata_adapter_controller_removed,

	ata_adapter_publish_controller,
	ata_adapter_detect_controller,

	ata_adapter_probe_controller
};

module_info *modules[] = {
	&adapter_interface.info,
	NULL
};
