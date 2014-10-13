/*
 * itrace.h: Instruction Tracing support
 * Copyright (c) 2013-2014, Intel Corporation.
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

#ifndef __PERF_ITRACE_H
#define __PERF_ITRACE_H

#include <sys/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <linux/perf_event.h>
#include <linux/types.h>

#include "../perf.h"
#include "session.h"

union perf_event;
struct perf_session;
struct perf_evlist;
struct perf_tool;
struct record_opts;
struct itrace_info_event;

struct itrace {
	int (*process_event)(struct perf_session *session,
			     union perf_event *event,
			     struct perf_sample *sample,
			     struct perf_tool *tool);
	int (*flush_events)(struct perf_session *session,
			    struct perf_tool *tool);
	void (*free_events)(struct perf_session *session);
	void (*free)(struct perf_session *session);
	unsigned long long error_count;
};

/**
 * struct itrace_mmap - records an mmap of the itrace buffer file descriptor.
 * @base: address of mapped area
 * @userpg: pointer to buffer's perf_event_mmap_page
 * @mask: %0 if @len is not a power of two, otherwise (@len - %1)
 * @len: size of mapped area
 * @prev: previous aux_head
 * @idx: index of this mmap
 * @tid: tid for a per-thread mmap (also set if there is only 1 tid on a per-cpu
 *       mmap) otherwise %0
 * @cpu: cpu number for a per-cpu mmap otherwise %-1
 * @fd: itrace buffer file descriptor
 */
struct itrace_mmap {
	void		*base;
	void		*userpg;
	size_t		mask;
	size_t		len;
	u64		prev;
	int		idx;
	pid_t		tid;
	int		cpu;
	int		fd;
};

/**
 * struct itrace_mmap_params - parameters to set up struct itrace_mmap.
 * @mask: %0 if @len is not a power of two, otherwise (@len - %1)
 * @offset: file offset of mapped area
 * @len: size of mapped area
 * @prot: mmap memory protection
 * @idx: index of this mmap
 * @tid: tid for a per-thread mmap (also set if there is only 1 tid on a per-cpu
 *       mmap) otherwise %0
 * @cpu: cpu number for a per-cpu mmap otherwise %-1
 */
struct itrace_mmap_params {
	size_t		mask;
	off_t		offset;
	size_t		len;
	int		prot;
	int		idx;
	pid_t		tid;
	int		cpu;
};

struct itrace_record {
	int (*recording_options)(struct itrace_record *itr,
				 struct perf_evlist *evlist,
				 struct record_opts *opts);
	size_t (*info_priv_size)(struct itrace_record *itr);
	int (*info_fill)(struct itrace_record *itr,
			 struct perf_session *session,
			 struct itrace_info_event *itrace_info,
			 size_t priv_size);
	void (*free)(struct itrace_record *itr);
	u64 (*reference)(struct itrace_record *itr);
	int (*read_finish)(struct itrace_record *itr, int idx);
};

static inline u64 itrace_mmap__read_head(struct itrace_mmap *mm)
{
	struct perf_event_mmap_page *pc = mm->userpg;
#if BITS_PER_LONG == 64 || !defined(HAVE_SYNC_COMPARE_AND_SWAP_SUPPORT)
	u64 head = ACCESS_ONCE(pc->aux_head);
#else
	u64 head = __sync_val_compare_and_swap(&pc->aux_head, 0, 0);
#endif

	/* Ensure all reads are done after we read the head */
	rmb();
	return head;
}

static inline void itrace_mmap__write_tail(struct itrace_mmap *mm, u64 tail)
{
	struct perf_event_mmap_page *pc = mm->userpg;
#if BITS_PER_LONG != 64 && defined(HAVE_SYNC_COMPARE_AND_SWAP_SUPPORT)
	u64 old_tail;
#endif

	/* Ensure all reads are done before we write the tail out */
	mb();
#if BITS_PER_LONG == 64 || !defined(HAVE_SYNC_COMPARE_AND_SWAP_SUPPORT)
	pc->aux_tail = tail;
#else
	do {
		old_tail = __sync_val_compare_and_swap(&pc->aux_tail, 0, 0);
	} while (!__sync_bool_compare_and_swap(&pc->aux_tail, old_tail, tail));
#endif
}

int itrace_mmap__mmap(struct itrace_mmap *mm,
		      struct itrace_mmap_params *mp,
		      void *userpg, int fd);
void itrace_mmap__munmap(struct itrace_mmap *mm);
void itrace_mmap_params__init(struct itrace_mmap_params *mp,
			      off_t itrace_offset,
			      unsigned int itrace_pages, bool itrace_overwrite);
void itrace_mmap_params__set_idx(struct itrace_mmap_params *mp,
				 struct perf_evlist *evlist, int idx,
				 bool per_cpu);

typedef int (*process_itrace_t)(struct perf_tool *tool, union perf_event *event,
				void *data1, size_t len1, void *data2,
				size_t len2);

int itrace_mmap__read(struct itrace_mmap *mm,
			    struct itrace_record *itr, struct perf_tool *tool,
			    process_itrace_t fn);

struct itrace_record *itrace_record__init(int *err);

int itrace_record__options(struct itrace_record *itr,
			     struct perf_evlist *evlist,
			     struct record_opts *opts);
size_t itrace_record__info_priv_size(struct itrace_record *itr);
int itrace_record__info_fill(struct itrace_record *itr,
			     struct perf_session *session,
			     struct itrace_info_event *itrace_info,
			     size_t priv_size);
void itrace_record__free(struct itrace_record *itr);
u64 itrace_record__reference(struct itrace_record *itr);

int perf_event__synthesize_itrace_info(struct itrace_record *itr,
				       struct perf_tool *tool,
				       struct perf_session *session,
				       perf_event__handler_t process);
int perf_event__synthesize_itrace(struct perf_tool *tool,
				  perf_event__handler_t process,
				  size_t size, u64 offset, u64 ref, int idx,
				  u32 tid, u32 cpu);

static inline int itrace__process_event(struct perf_session *session,
					union perf_event *event,
					struct perf_sample *sample,
					struct perf_tool *tool)
{
	if (!session->itrace)
		return 0;

	return session->itrace->process_event(session, event, sample, tool);
}

static inline int itrace__flush_events(struct perf_session *session,
				       struct perf_tool *tool)
{
	if (!session->itrace)
		return 0;

	return session->itrace->flush_events(session, tool);
}

static inline void itrace__free_events(struct perf_session *session)
{
	if (!session->itrace)
		return;

	return session->itrace->free_events(session);
}

static inline void itrace__free(struct perf_session *session)
{
	if (!session->itrace)
		return;

	return session->itrace->free(session);
}

#endif
