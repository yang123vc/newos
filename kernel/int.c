/*
** Copyright 2001-2004, Travis Geiselbrecht. All rights reserved.
** Distributed under the terms of the NewOS License.
*/
#include <kernel/kernel.h>
#include <kernel/int.h>
#include <kernel/debug.h>
#include <kernel/heap.h>
#include <kernel/smp.h>
#include <kernel/thread.h>
#include <kernel/arch/int.h>
#include <newos/errors.h>
#include <boot/stage2.h>
#include <string.h>
#include <stdio.h>

struct io_handler {
	struct io_handler *next;
	int (*func)(void*);
	void* data;
	const char *name;
};

struct io_vector {
	struct io_handler *handler_list;
	spinlock_t         vector_lock;

	// statistics
	int call_count;
};

static struct io_vector io_vectors[ARCH_NUM_INT_VECTORS];

int int_init(kernel_args *ka)
{
	dprintf("init_int_handlers: entry\n");

	return arch_int_init(ka);
}

int int_init2(kernel_args *ka)
{
	// clear out all of the vectors
	memset(io_vectors, 0, sizeof(struct io_vector) * ARCH_NUM_INT_VECTORS);

	return arch_int_init2(ka);
}

int int_set_io_interrupt_handler(int vector, int (*func)(void*), void* data, const char *name)
{
	struct io_handler *io;

	if(vector < 0 || vector >= ARCH_NUM_INT_VECTORS)
		return ERR_INVALID_ARGS;

	// insert this io handler in the chain of interrupt
	// handlers registered for this io interrupt

	io = (struct io_handler *)kmalloc(sizeof(struct io_handler));
	if(io == NULL)
		return ERR_NO_MEMORY;

	io->name = kstrdup(name);
	if(io->name == NULL) {
		kfree(io);
		return ERR_NO_MEMORY;
	}
	io->func = func;
	io->data = data;

	int_disable_interrupts();
	acquire_spinlock(&io_vectors[vector].vector_lock);
	io->next = io_vectors[vector].handler_list;
	io_vectors[vector].handler_list = io;
	release_spinlock(&io_vectors[vector].vector_lock);
	int_restore_interrupts();

	arch_int_enable_io_interrupt(vector);

	return NO_ERROR;
}

int int_remove_io_interrupt_handler(int vector, int (*func)(void*), void* data)
{
	struct io_handler *io, *prev = NULL;

	if(vector < 0 || vector >= ARCH_NUM_INT_VECTORS)
		return ERR_INVALID_ARGS;

	// lock the structures down so it is not modified while we search
	int_disable_interrupts();
	acquire_spinlock(&io_vectors[vector].vector_lock);

	// start at the beginning
	io = io_vectors[vector].handler_list;

	// while not at end
	while(io != NULL) {
		// see if we match both the function & data
		if (io->func == func && io->data == data)
			break;

		// Store our backlink and move to next
		prev = io;
		io = io->next;
	}

	// If we found it
	if (io != NULL) {
		// unlink it, taking care of the change it was the first in line
		if (prev != NULL)
			prev->next = io->next;
		else
			io_vectors[vector].handler_list = io->next;
	}

	// release our lock as we're done with the vector
	release_spinlock(&io_vectors[vector].vector_lock);
	int_restore_interrupts();

	// and disable the IRQ if nothing left
	if (io != NULL) {
		if (prev == NULL && io->next == NULL)
			arch_int_disable_io_interrupt(vector);

		kfree((char *)io->name);
		kfree(io);
	}

	return (io != NULL) ? NO_ERROR : ERR_INVALID_ARGS;
}

int int_io_interrupt_handler(int vector)
{
	int ret = INT_NO_RESCHEDULE;

	acquire_spinlock(&io_vectors[vector].vector_lock);

	io_vectors[vector].call_count++;

	if(io_vectors[vector].handler_list == NULL) {
		dprintf("unhandled io interrupt 0x%x\n", vector);
	} else {
		struct io_handler *io;
		int temp_ret;

		io = io_vectors[vector].handler_list;
		while(io != NULL) {
			temp_ret = io->func(io->data);
			if(temp_ret == INT_RESCHEDULE)
				ret = INT_RESCHEDULE;
			io = io->next;
		}
	}

	release_spinlock(&io_vectors[vector].vector_lock);

	return ret;
}

void int_enable_interrupts(void)
{
	arch_int_enable_interrupts();
}

// increase the interrupt disable count in the current thread structure.
// if we go from 0 to 1, disable interrupts
void int_disable_interrupts(void)
{
	struct thread *t = thread_get_current_thread();
	if(!t)
		return;

	ASSERT(t->int_disable_level >= 0);

	t->int_disable_level++;
	if(t->int_disable_level == 1) {
		// we just crossed from 0 -> 1
		arch_int_disable_interrupts();
	}
}

// decrement the interrupt disable count. If we hit zero, re-enable interrupts
void int_restore_interrupts(void)
{
	struct thread *t = thread_get_current_thread();
	if(!t)
		return;

	t->int_disable_level--;

	ASSERT(t->int_disable_level >= 0);
	if(t->int_disable_level == 0) {
		// we just hit 0
		arch_int_enable_interrupts();
	}
}

bool int_are_interrupts_enabled(void)
{
	return arch_int_are_interrupts_enabled();
}

