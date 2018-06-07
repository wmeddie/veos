/*
 * Copyright (C) 2017-2018 NEC Corporation
 * This file is part of the VEOS.
 *
 * The VEOS is free software; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * The VEOS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with the VEOS; if not, see
 * <http://www.gnu.org/licenses/>.
 */
/**
 * @file dma_private.h
 * @brief DMA manager private header: users of DMA API may not include this.
 */
#ifndef VE_VEOS_DMA_PRIVATE_H
#define VE_VEOS_DMA_PRIVATE_H

#include <stdint.h>
#include <pthread.h>
#include <libved.h>

#include "ve_list.h"
#include "vedma_hw.h"

#define VE_DMA_MAX_LENGTH 0x7FFFFFFFFFFFFFF8UL
#define VH_PAGE_SHIFT (12)
#define VH_PAGE_SIZE (1 << VH_PAGE_SHIFT)
/**
 * VE page size is always consiedered as 2 MB.
 * This causes no problems since 64 MB, the page size of huge page,
 * is a multiple of 2 MB and DMA manager merges physically contiguous
 * requests as possible.
 */
#define VE_PAGE_SHIFT (21)
#define VE_PAGE_SIZE (1 << VE_PAGE_SHIFT)
#define VH_PAGE_MASK (~(VH_PAGE_SIZE - 1))
#define VE_PAGE_MASK (~(VE_PAGE_SIZE - 1))
#define VH_PAGE_ALIGN(addr) ((addr) & VH_PAGE_MASK)

/**
 * msg should include only one '%s' specifier for printf(3)-family,
 * converted to an error message by strerror(3).
 */
#define print_oserr(msgfmt) \
do { \
	int saved_errno = errno; \
	VE_DMA_CRIT(msgfmt, strerror(saved_errno)); \
	errno = saved_errno; \
} while (0)

struct ve_dma_reqlist_entry;

/**
 * @brief DMA handle
 */
struct ve_dma_hdl_struct {
	struct list_head waiting_list;/*!< wait queue */
	struct list_head deallocate_list;/*!< deallocate queue */
	vedl_handle *vedl_handle;/*!< VEDL handle of the node */
	pthread_t helper;/*!< interrupt helper thread for the DMA engine */
	pthread_t deallocator;/*!< DMA request deallocator thread */
	pthread_mutex_t mutex;/*!< mutex for this DMA handle */
	pthread_mutex_t deallocate_list_mutex;/*!< mutex for deallocate_list in this DMA handle */
	pthread_cond_t deallocator_cond;/*!< condition variable for deallocator */
	int should_stop;/*!< flag denoting that DMA engine should stop and should not accept any more requests */
	int desc_used_begin;/*!< the start number of used DMA descriptors */
	int desc_num_used;/*!< the number of used DMA descriptors */
	struct ve_dma_reqlist_entry *req_entry[VE_DMA_NUM_DESC];/*!< DMA reqlist entry on each DMA descriptor */
	system_common_reg_t *control_regs;/*!< pointer to node control registers area */
};

/**
 * @brief DMA request handle
 */
struct ve_dma_req_hdl_struct {
	struct ve_dma_hdl_struct *engine;/*!< DMA engine on which this request is posted */
	pthread_cond_t cond;/*!< condition variable to wait for status of DMA reqlist entries in reqlist to change */
	struct list_head reqlist;/*!< a list of DMA reqlist entries composing this request */
	struct list_head deallocate_list;/*!< deallocate queue */
};

/* in dma_intr.c */
void ve_dma__drain_waiting_list(ve_dma_hdl *);
void ve_dma__stop_engine(ve_dma_hdl *);

#endif
