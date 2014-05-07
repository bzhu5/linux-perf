/*
 * thread-stack.c: Synthesize a thread's stack using call / return events
 * Copyright (c) 2014, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include "thread.h"
#include "event.h"
#include "util.h"
#include "thread-stack.h"

#define STACK_GROWTH 4096

struct thread_stack_entry {
	u64 ret_addr;
};

struct thread_stack {
	struct thread_stack_entry *stack;
	size_t cnt;
	size_t sz;
	u64 trace_nr;
};

static void thread_stack__grow(struct thread_stack *ts)
{
	struct thread_stack_entry *new_stack;
	size_t sz, new_sz;

	new_sz = ts->sz + STACK_GROWTH;
	sz = new_sz * sizeof(struct thread_stack_entry);
	new_stack = realloc(ts->stack, sz);
	if (new_stack) {
		ts->stack = new_stack;
		ts->sz = new_sz;
	}
}

static struct thread_stack *thread_stack__new(void)
{
	struct thread_stack *ts;

	ts = zalloc(sizeof(struct thread_stack));
	if (!ts)
		return NULL;

	thread_stack__grow(ts);
	if (!ts->stack) {
		free(ts);
		return NULL;
	}

	return ts;
}

static void thread_stack__push(struct thread_stack *ts, u64 ret_addr)
{
	if (ts->cnt == ts->sz) {
		thread_stack__grow(ts);
		if (ts->cnt == ts->sz)
			ts->cnt = 0;
	}

	ts->stack[ts->cnt++].ret_addr = ret_addr;
}

static void thread_stack__pop(struct thread_stack *ts, u64 ret_addr)
{
	if (!ts->cnt)
		return;

	if (ts->stack[ts->cnt - 1].ret_addr == ret_addr) {
		ts->cnt -= 1;
	} else {
		size_t i = ts->cnt - 1;

		while (i--) {
			if (ts->stack[i].ret_addr == ret_addr) {
				ts->cnt = i;
				return;
			}
		}
	}
}

void thread_stack__event(struct thread *thread, u32 flags, u64 from_ip,
			 u64 to_ip, u16 insn_len, u64 trace_nr)
{
	if (!thread)
		return;

	if (!thread->ts) {
		thread->ts = thread_stack__new();
		if (!thread->ts)
			return;
		thread->ts->trace_nr = trace_nr;
	}

	if (trace_nr != thread->ts->trace_nr) {
		thread->ts->trace_nr = trace_nr;
		thread->ts->cnt = 0;
	}

	if (flags & PERF_FLAG_CALL) {
		u64 ret_addr;

		if (!to_ip)
			return;
		ret_addr = from_ip + insn_len;
		if (ret_addr == to_ip)
			return; /* Zero-length calls are excluded */
		thread_stack__push(thread->ts, ret_addr);
	} else if (flags & PERF_FLAG_RETURN) {
		if (!from_ip)
			return;
		thread_stack__pop(thread->ts, to_ip);
	}
}

void thread_stack__free(struct thread *thread)
{
	if (thread->ts) {
		zfree(&thread->ts->stack);
		zfree(&thread->ts);
	}
}

void thread_stack__sample(struct thread *thread, struct ip_callchain *chain,
			  size_t sz, u64 ip)
{
	size_t i;

	if (!thread || !thread->ts)
		chain->nr = 1;
	else
		chain->nr = min(sz, thread->ts->cnt + 1);

	chain->ips[0] = ip;

	for (i = 1; i < chain->nr; i++)
		chain->ips[i] = thread->ts->stack[thread->ts->cnt - i].ret_addr;
}
