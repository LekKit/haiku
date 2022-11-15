/*
 * Copyright 2003-2010, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */
#ifndef _KERNEL_INT_H
#define _KERNEL_INT_H


#include <KernelExport.h>

#ifdef __cplusplus

// ABI compatible C++ interface for struct interrupt_source
class InterruptSource {
public:
	virtual void EnableIoInterrupt(int irq) = 0;
	virtual void DisableIoInterrupt(int irq) = 0;
	virtual void ConfigureIoInterrupt(int irq, uint32 config) = 0;
	virtual int32 AssignToCpu(int32 irq, int32 cpu) = 0;
};

#endif

#include <arch/int.h>

#include <util/list.h>

// private install_io_interrupt_handler() flags
#define B_NO_LOCK_VECTOR	0x100
#define B_NO_HANDLED_INFO	0x200

struct kernel_args;


enum interrupt_type {
	INTERRUPT_TYPE_EXCEPTION,
	INTERRUPT_TYPE_IRQ,
	INTERRUPT_TYPE_LOCAL_IRQ,
	INTERRUPT_TYPE_SYSCALL,
	INTERRUPT_TYPE_ICI,
	INTERRUPT_TYPE_UNKNOWN
};

struct irq_assignment {
	list_link	link;

	uint32		irq;
	uint32		count;

	int32		handlers_count;

	int32		load;
	int32		cpu;
};

typedef struct interrupt_source {
	struct interrupt_source_vtable* vt;
} interrupt_source;

typedef struct interrupt_source_vtable {
	void (*enable_io_interrupt)(interrupt_source* src, int irq);
	void (*disable_io_interrupt)(interrupt_source* src, int irq);
	void (*configure_io_interrupt)(interrupt_source* src, int irq, uint32 config);
	int32 (*assign_to_cpu)(interrupt_source* src, int32 irq, int32 cpu);
} interrupt_source_vtable;


#ifdef __cplusplus
extern "C" {
#endif

status_t int_init(struct kernel_args* args);
status_t int_init_post_vm(struct kernel_args* args);
status_t int_init_io(struct kernel_args* args);
status_t int_init_post_device_manager(struct kernel_args* args);
int int_io_interrupt_handler(int vector, bool levelTriggered);

bool interrupts_enabled(void);

static inline void
enable_interrupts(void)
{
	arch_int_enable_interrupts();
}

static inline bool
are_interrupts_enabled(void)
{
	return arch_int_are_interrupts_enabled();
}

#ifdef __cplusplus
}
#endif


// map those directly to the arch versions, so they can be inlined
#define disable_interrupts()		arch_int_disable_interrupts()
#define restore_interrupts(status)	arch_int_restore_interrupts(status)


#ifdef __cplusplus
extern "C++" {

status_t reserve_io_interrupt_vectors(long count, long startVector,
	enum interrupt_type type, interrupt_source* source);
status_t allocate_io_interrupt_vectors(long count, long *startVector,
	enum interrupt_type type, interrupt_source* source);
void free_io_interrupt_vectors(long count, long startVector);

void assign_io_interrupt_to_cpu(long vector, int32 cpu);

#ifndef __riscv
status_t reserve_io_interrupt_vectors(long count, long startVector,
	enum interrupt_type type);

status_t allocate_io_interrupt_vectors(long count, long *startVector,
	enum interrupt_type type);
#endif

inline status_t
reserve_io_interrupt_vectors(long count, long startVector,
	enum interrupt_type type, InterruptSource* source)
{
	return reserve_io_interrupt_vectors(count, startVector, type, (interrupt_source*)source);
}


inline status_t
allocate_io_interrupt_vectors(long count, long *startVector,
	enum interrupt_type type, InterruptSource* source)
{
	return allocate_io_interrupt_vectors(count, startVector, type, (interrupt_source*)source);
}

}
#endif

#endif /* _KERNEL_INT_H */
