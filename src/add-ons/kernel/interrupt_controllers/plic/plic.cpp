/*
 * Copyright 2022, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#include <KernelExport.h>
#include <bus/FDT.h>

#include <AutoDeleterOS.h>
#include <AutoDeleterDrivers.h>
#include <interrupt_controller2.h>

#include <Plic.h>

#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}


#define PLIC_MODULE_NAME	"interrupt_controllers/plic/driver_v1"


static device_manager_info *sDeviceManager;


class PlicInterruptController: public InterruptSource {
private:
	AreaDeleter fRegsArea;
	PlicRegs volatile* fRegs {};
	uint32 fIrqCount {};
	uint32 fPlicContexts[SMP_MAX_CPUS] {};

public:
	virtual ~PlicInterruptController() = default;

	static float SupportsDevice(device_node* parent);
	static status_t RegisterDevice(device_node* parent);
	static status_t InitDriver(device_node* node, PlicInterruptController*& driver);
	void UninitDriver();

	status_t GetVector(uint64 irq, long& vector);

	static int32 HandleInterrupt(void* arg);

	void EnableIoInterrupt(int irq) final;
	void DisableIoInterrupt(int irq) final;
	void ConfigureIoInterrupt(int irq, uint32 config) final {}
	int32 AssignToCpu(int32 irq, int32 cpu) final;
};


float
PlicInterruptController::SupportsDevice(device_node* parent)
{
	const char* bus;
	status_t status = sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false);
	if (status < B_OK)
		return -1.0f;

	// detect virtio device from FDT
	if (strcmp(bus, "fdt") != 0)
		return 0.0f;
	
	const char* compatible;
	status = sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible, false);
	if (status < B_OK)
		return -1.0f;

	if (strcmp(compatible, "riscv,plic0") != 0
		&& strcmp(compatible, "sifive,fu540-c000-plic") != 0
		&& strcmp(compatible, "sifive,plic-1.0.0") != 0)
		return 0.0f;

	return 1.0f;
}


status_t
PlicInterruptController::RegisterDevice(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {string: "PLIC"} },
		{}
	};

	return sDeviceManager->register_node(parent, PLIC_MODULE_NAME, attrs, NULL, NULL);
}


status_t
PlicInterruptController::InitDriver(device_node* node, PlicInterruptController*& outDriver)
{
	ObjectDeleter<PlicInterruptController> driver(new(std::nothrow) PlicInterruptController());
	if (!driver.IsSet())
		return B_NO_MEMORY;

	DeviceNodePutter<&sDeviceManager> parent(sDeviceManager->get_parent_node(node));

	const char* bus;
	CHECK_RET(sDeviceManager->get_attr_string(parent.Get(), B_DEVICE_BUS, &bus, false));
	if (strcmp(bus, "fdt") != 0)
		return B_ERROR;

	fdt_device_module_info *parentModule;
	fdt_device* parentDev;
	CHECK_RET(sDeviceManager->get_driver(parent.Get(), (driver_module_info**)&parentModule, (void**)&parentDev));

	const void* prop;
	int propLen;
	prop = parentModule->get_prop(parentDev, "riscv,ndev", &propLen);
	if (prop == NULL || propLen != 4)
		return B_ERROR;

	driver->fIrqCount = B_BENDIAN_TO_HOST_INT32(*(const uint32*)prop);
	dprintf("irqCount: %" B_PRIu32 "\n", driver->fIrqCount);

	int32 cpuCount = smp_get_num_cpus();
	uint32 cookie = 0;
	device_node* hartIntcNode;
	uint64 cause;
	while (parentModule->get_interrupt(parentDev, cookie, &hartIntcNode, &cause)) {
		uint32 plicContext = cookie++;
		device_node* hartNode = sDeviceManager->get_parent_node(hartIntcNode);
		DeviceNodePutter<&sDeviceManager> hartNodePutter(hartNode);
		fdt_device* hartDev;
		CHECK_RET(sDeviceManager->get_driver(hartNode, NULL, (void**)&hartDev));
		const void* prop;
		int propLen;
		prop = parentModule->get_prop(hartDev, "reg", &propLen);
		if (prop == NULL || propLen != 4)
			return B_ERROR;

		uint32 hartId = B_BENDIAN_TO_HOST_INT32(*(const uint32*)prop);
		dprintf("%" B_PRIu32 "\n", plicContext);
		dprintf("  cause: %" B_PRIu64 "\n", cause);
		dprintf("  hartId: %" B_PRIu32 "\n", hartId);

		if (cause == sExternInt) {
			int32 cpu = 0;
			while (cpu < cpuCount && !(gCPU[cpu].arch.hartId == hartId))
				cpu++;

			if (cpu < cpuCount)
				driver->fPlicContexts[cpu] = plicContext;
		}
	}

	uint64 regs = 0;
	uint64 regsLen = 0;
	if (!parentModule->get_reg(parentDev, 0, &regs, &regsLen))
		return B_ERROR;

	driver->fRegsArea.SetTo(map_physical_memory("PLIC MMIO", regs, regsLen, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&driver->fRegs));
	if (!driver->fRegsArea.IsSet())
		return driver->fRegsArea.Get();

	reserve_io_interrupt_vectors(driver->fIrqCount + 1, 0, INTERRUPT_TYPE_IRQ, driver.Get());
	install_io_interrupt_handler(0, HandleInterrupt, driver.Get(), B_NO_LOCK_VECTOR);

	for (int32 cpu = 0; cpu < cpuCount; cpu++)
		driver->fRegs->contexts[driver->fPlicContexts[cpu]].priorityThreshold = 0;

	// unmask interrupts
	for (uint32 irq = 1; irq < driver->fIrqCount + 1; irq++)
		driver->fRegs->priority[irq] = 1;

	outDriver = driver.Detach();
	return B_OK;
}


void
PlicInterruptController::UninitDriver()
{
	delete this;
}


status_t
PlicInterruptController::GetVector(uint64 irq, long& vector)
{
	if (irq < 1 || irq >= fIrqCount + 1)
		return B_BAD_INDEX;

	vector = irq;
	return B_OK;
}


int32
PlicInterruptController::HandleInterrupt(void* arg)
{
	PlicInterruptController* ctrl = (PlicInterruptController*)arg;
	uint32 context = ctrl->fPlicContexts[smp_get_current_cpu()];
	uint64 irq = ctrl->fRegs->contexts[context].claimAndComplete;
	if (irq == 0)
		return B_HANDLED_INTERRUPT;
	int_io_interrupt_handler(irq, true);
	ctrl->fRegs->contexts[context].claimAndComplete = irq;	
	return B_HANDLED_INTERRUPT;
}


void
PlicInterruptController::EnableIoInterrupt(int irq)
{
	dprintf("PlicInterruptController::EnableIoInterrupt(%d)\n", irq);
	fRegs->enable[fPlicContexts[0]][irq / 32] |= 1 << (irq % 32);
}


void
PlicInterruptController::DisableIoInterrupt(int irq)
{
	dprintf("PlicInterruptController::DisableIoInterrupt(%d)\n", irq);
	fRegs->enable[fPlicContexts[0]][irq / 32] &= ~(1 << (irq % 32));
}


int32
PlicInterruptController::AssignToCpu(int32 irq, int32 cpu)
{
	// Not yet supported.
	return 0;
}


static interrupt_controller_module_info sControllerModuleInfo = {
	{
		{
			.name = PLIC_MODULE_NAME,
		},
		.supports_device = [](device_node* parent) {
			return PlicInterruptController::SupportsDevice(parent);
		},
		.register_device = [](device_node* parent) {
			return PlicInterruptController::RegisterDevice(parent);
		},
		.init_driver = [](device_node* node, void** driverCookie) {
			return PlicInterruptController::InitDriver(node, *(PlicInterruptController**)driverCookie);
		},
		.uninit_driver = [](void* driverCookie) {
			return static_cast<PlicInterruptController*>(driverCookie)->UninitDriver();
		},
	},
	.get_vector = [](void* cookie, uint64 irq, long* vector) {
		return static_cast<PlicInterruptController*>(cookie)->GetVector(irq, *vector);
	},
};

_EXPORT module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info **)&sDeviceManager },
	{}
};

_EXPORT module_info *modules[] = {
	(module_info *)&sControllerModuleInfo,
	NULL
};
