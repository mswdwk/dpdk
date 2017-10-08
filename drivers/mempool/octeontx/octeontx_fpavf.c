/*
 *   BSD LICENSE
 *
 *   Copyright (C) Cavium Inc. 2017. All Right reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Cavium networks nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>

#include <rte_atomic.h>
#include <rte_eal.h>
#include <rte_pci.h>
#include <rte_errno.h>
#include <rte_memory.h>
#include <rte_malloc.h>
#include <rte_spinlock.h>
#include <rte_mbuf.h>

#include <rte_pmd_octeontx_ssovf.h>
#include "octeontx_fpavf.h"

/* FPA Mbox Message */
#define IDENTIFY		0x0

#define FPA_CONFIGSET		0x1
#define FPA_CONFIGGET		0x2
#define FPA_START_COUNT		0x3
#define FPA_STOP_COUNT		0x4
#define FPA_ATTACHAURA		0x5
#define FPA_DETACHAURA		0x6
#define FPA_SETAURALVL		0x7
#define FPA_GETAURALVL		0x8

#define FPA_COPROC		0x1

/* fpa mbox struct */
struct octeontx_mbox_fpa_cfg {
	int		aid;
	uint64_t	pool_cfg;
	uint64_t	pool_stack_base;
	uint64_t	pool_stack_end;
	uint64_t	aura_cfg;
};

struct __attribute__((__packed__)) gen_req {
	uint32_t	value;
};

struct __attribute__((__packed__)) idn_req {
	uint8_t	domain_id;
};

struct __attribute__((__packed__)) gen_resp {
	uint16_t	domain_id;
	uint16_t	vfid;
};

struct __attribute__((__packed__)) dcfg_resp {
	uint8_t	sso_count;
	uint8_t	ssow_count;
	uint8_t	fpa_count;
	uint8_t	pko_count;
	uint8_t	tim_count;
	uint8_t	net_port_count;
	uint8_t	virt_port_count;
};

#define FPA_MAX_POOL	32
#define FPA_PF_PAGE_SZ	4096

#define FPA_LN_SIZE	128
#define FPA_ROUND_UP(x, size) \
	((((unsigned long)(x)) + size-1) & (~(size-1)))
#define FPA_OBJSZ_2_CACHE_LINE(sz)	(((sz) + RTE_CACHE_LINE_MASK) >> 7)
#define FPA_CACHE_LINE_2_OBJSZ(sz)	((sz) << 7)

#define POOL_ENA			(0x1 << 0)
#define POOL_DIS			(0x0 << 0)
#define POOL_SET_NAT_ALIGN		(0x1 << 1)
#define POOL_DIS_NAT_ALIGN		(0x0 << 1)
#define POOL_STYPE(x)			(((x) & 0x1) << 2)
#define POOL_LTYPE(x)			(((x) & 0x3) << 3)
#define POOL_BUF_OFFSET(x)		(((x) & 0x7fffULL) << 16)
#define POOL_BUF_SIZE(x)		(((x) & 0x7ffULL) << 32)

struct fpavf_res {
	void		*pool_stack_base;
	void		*bar0;
	uint64_t	stack_ln_ptr;
	uint16_t	domain_id;
	uint16_t	vf_id;	/* gpool_id */
	uint16_t	sz128;	/* Block size in cache lines */
	bool		is_inuse;
};

struct octeontx_fpadev {
	rte_spinlock_t lock;
	uint8_t	total_gpool_cnt;
	struct fpavf_res pool[FPA_VF_MAX];
};

static struct octeontx_fpadev fpadev;

/* lock is taken by caller */
static int
octeontx_fpa_gpool_alloc(unsigned int object_size)
{
	struct fpavf_res *res = NULL;
	uint16_t gpool;
	unsigned int sz128;

	sz128 = FPA_OBJSZ_2_CACHE_LINE(object_size);

	for (gpool = 0; gpool < FPA_VF_MAX; gpool++) {

		/* Skip VF that is not mapped Or _inuse */
		if ((fpadev.pool[gpool].bar0 == NULL) ||
		    (fpadev.pool[gpool].is_inuse == true))
			continue;

		res = &fpadev.pool[gpool];

		RTE_ASSERT(res->domain_id != (uint16_t)~0);
		RTE_ASSERT(res->vf_id != (uint16_t)~0);
		RTE_ASSERT(res->stack_ln_ptr != 0);

		if (res->sz128 == 0) {
			res->sz128 = sz128;

			fpavf_log_dbg("gpool %d blk_sz %d\n", gpool, sz128);
			return gpool;
		}
	}

	return -ENOSPC;
}

/* lock is taken by caller */
static __rte_always_inline uintptr_t
octeontx_fpa_gpool2handle(uint16_t gpool)
{
	struct fpavf_res *res = NULL;

	RTE_ASSERT(gpool < FPA_VF_MAX);

	res = &fpadev.pool[gpool];
	if (unlikely(res == NULL))
		return 0;

	return (uintptr_t)res->bar0 | gpool;
}

static __rte_always_inline bool
octeontx_fpa_handle_valid(uintptr_t handle)
{
	struct fpavf_res *res = NULL;
	uint8_t gpool;
	int i;
	bool ret = false;

	if (unlikely(!handle))
		return ret;

	/* get the gpool */
	gpool = octeontx_fpa_bufpool_gpool(handle);

	/* get the bar address */
	handle &= ~(uint64_t)FPA_GPOOL_MASK;
	for (i = 0; i < FPA_VF_MAX; i++) {
		if ((uintptr_t)fpadev.pool[i].bar0 != handle)
			continue;

		/* validate gpool */
		if (gpool != i)
			return false;

		res = &fpadev.pool[i];

		if (res->sz128 == 0 || res->domain_id == (uint16_t)~0 ||
		    res->stack_ln_ptr == 0)
			ret = false;
		else
			ret = true;
		break;
	}

	return ret;
}

static int
octeontx_fpapf_pool_setup(unsigned int gpool, unsigned int buf_size,
			  signed short buf_offset, unsigned int max_buf_count)
{
	void *memptr = NULL;
	phys_addr_t phys_addr;
	unsigned int memsz;
	struct fpavf_res *fpa = NULL;
	uint64_t reg;
	struct octeontx_mbox_hdr hdr;
	struct dcfg_resp resp;
	struct octeontx_mbox_fpa_cfg cfg;
	int ret = -1;

	fpa = &fpadev.pool[gpool];
	memsz = FPA_ROUND_UP(max_buf_count / fpa->stack_ln_ptr, FPA_LN_SIZE) *
			FPA_LN_SIZE;

	/* Round-up to page size */
	memsz = (memsz + FPA_PF_PAGE_SZ - 1) & ~(uintptr_t)(FPA_PF_PAGE_SZ-1);
	memptr = rte_malloc(NULL, memsz, RTE_CACHE_LINE_SIZE);
	if (memptr == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	/* Configure stack */
	fpa->pool_stack_base = memptr;
	phys_addr = rte_malloc_virt2phy(memptr);

	buf_size /= FPA_LN_SIZE;

	/* POOL setup */
	hdr.coproc = FPA_COPROC;
	hdr.msg = FPA_CONFIGSET;
	hdr.vfid = fpa->vf_id;
	hdr.res_code = 0;

	buf_offset /= FPA_LN_SIZE;
	reg = POOL_BUF_SIZE(buf_size) | POOL_BUF_OFFSET(buf_offset) |
		POOL_LTYPE(0x2) | POOL_STYPE(0) | POOL_SET_NAT_ALIGN |
		POOL_ENA;

	cfg.aid = 0;
	cfg.pool_cfg = reg;
	cfg.pool_stack_base = phys_addr;
	cfg.pool_stack_end = phys_addr + memsz;
	cfg.aura_cfg = (1 << 9);

	ret = octeontx_ssovf_mbox_send(&hdr, &cfg,
					sizeof(struct octeontx_mbox_fpa_cfg),
					&resp, sizeof(resp));
	if (ret < 0) {
		ret = -EACCES;
		goto err;
	}

	fpavf_log_dbg(" vfid %d gpool %d aid %d pool_cfg 0x%x pool_stack_base %" PRIx64 " pool_stack_end %" PRIx64" aura_cfg %" PRIx64 "\n",
		      fpa->vf_id, gpool, cfg.aid, (unsigned int)cfg.pool_cfg,
		      cfg.pool_stack_base, cfg.pool_stack_end, cfg.aura_cfg);

	/* Now pool is in_use */
	fpa->is_inuse = true;

err:
	if (ret < 0)
		rte_free(memptr);

	return ret;
}

static int
octeontx_fpapf_pool_destroy(unsigned int gpool_index)
{
	struct octeontx_mbox_hdr hdr;
	struct dcfg_resp resp;
	struct octeontx_mbox_fpa_cfg cfg;
	struct fpavf_res *fpa = NULL;
	int ret = -1;

	fpa = &fpadev.pool[gpool_index];

	hdr.coproc = FPA_COPROC;
	hdr.msg = FPA_CONFIGSET;
	hdr.vfid = fpa->vf_id;
	hdr.res_code = 0;

	/* reset and free the pool */
	cfg.aid = 0;
	cfg.pool_cfg = 0;
	cfg.pool_stack_base = 0;
	cfg.pool_stack_end = 0;
	cfg.aura_cfg = 0;

	ret = octeontx_ssovf_mbox_send(&hdr, &cfg,
					sizeof(struct octeontx_mbox_fpa_cfg),
					&resp, sizeof(resp));
	if (ret < 0) {
		ret = -EACCES;
		goto err;
	}

	ret = 0;
err:
	/* anycase free pool stack memory */
	rte_free(fpa->pool_stack_base);
	fpa->pool_stack_base = NULL;
	return ret;
}

static int
octeontx_fpapf_aura_attach(unsigned int gpool_index)
{
	struct octeontx_mbox_hdr hdr;
	struct dcfg_resp resp;
	struct octeontx_mbox_fpa_cfg cfg;
	int ret = 0;

	if (gpool_index >= FPA_MAX_POOL) {
		ret = -EINVAL;
		goto err;
	}
	hdr.coproc = FPA_COPROC;
	hdr.msg = FPA_ATTACHAURA;
	hdr.vfid = gpool_index;
	hdr.res_code = 0;
	memset(&cfg, 0x0, sizeof(struct octeontx_mbox_fpa_cfg));
	cfg.aid = gpool_index; /* gpool is guara */

	ret = octeontx_ssovf_mbox_send(&hdr, &cfg,
					sizeof(struct octeontx_mbox_fpa_cfg),
					&resp, sizeof(resp));
	if (ret < 0) {
		fpavf_log_err("Could not attach fpa ");
		fpavf_log_err("aura %d to pool %d. Err=%d. FuncErr=%d\n",
			      gpool_index, gpool_index, ret, hdr.res_code);
		ret = -EACCES;
		goto err;
	}
err:
	return ret;
}

static int
octeontx_fpapf_aura_detach(unsigned int gpool_index)
{
	struct octeontx_mbox_fpa_cfg cfg = {0};
	struct octeontx_mbox_hdr hdr = {0};
	int ret = 0;

	if (gpool_index >= FPA_MAX_POOL) {
		ret = -EINVAL;
		goto err;
	}

	cfg.aid = gpool_index; /* gpool is gaura */
	hdr.coproc = FPA_COPROC;
	hdr.msg = FPA_DETACHAURA;
	hdr.vfid = gpool_index;
	ret = octeontx_ssovf_mbox_send(&hdr, &cfg, sizeof(cfg), NULL, 0);
	if (ret < 0) {
		fpavf_log_err("Couldn't detach FPA aura %d Err=%d FuncErr=%d\n",
			      gpool_index, ret, hdr.res_code);
		ret = -EINVAL;
	}

err:
	return ret;
}

static int
octeontx_fpavf_pool_setup(uintptr_t handle, unsigned long memsz,
			  void *memva, uint16_t gpool)
{
	uint64_t va_end;

	if (unlikely(!handle))
		return -ENODEV;

	va_end = (uintptr_t)memva + memsz;
	va_end &= ~RTE_CACHE_LINE_MASK;

	/* VHPOOL setup */
	fpavf_write64((uintptr_t)memva,
			 (void *)((uintptr_t)handle +
			 FPA_VF_VHPOOL_START_ADDR(gpool)));
	fpavf_write64(va_end,
			 (void *)((uintptr_t)handle +
			 FPA_VF_VHPOOL_END_ADDR(gpool)));
	return 0;
}

static int
octeontx_fpapf_start_count(uint16_t gpool_index)
{
	int ret = 0;
	struct octeontx_mbox_hdr hdr = {0};

	if (gpool_index >= FPA_MAX_POOL) {
		ret = -EINVAL;
		goto err;
	}

	hdr.coproc = FPA_COPROC;
	hdr.msg = FPA_START_COUNT;
	hdr.vfid = gpool_index;
	ret = octeontx_ssovf_mbox_send(&hdr, NULL, 0, NULL, 0);
	if (ret < 0) {
		fpavf_log_err("Could not start buffer counting for ");
		fpavf_log_err("FPA pool %d. Err=%d. FuncErr=%d\n",
			      gpool_index, ret, hdr.res_code);
		ret = -EINVAL;
		goto err;
	}

err:
	return ret;
}

static __rte_always_inline int
octeontx_fpavf_free(unsigned int gpool)
{
	int ret = 0;

	if (gpool >= FPA_MAX_POOL) {
		ret = -EINVAL;
		goto err;
	}

	/* Pool is free */
	fpadev.pool[gpool].is_inuse = false;

err:
	return ret;
}

static __rte_always_inline int
octeontx_gpool_free(uint16_t gpool)
{
	if (fpadev.pool[gpool].sz128 != 0) {
		fpadev.pool[gpool].sz128 = 0;
		return 0;
	}
	return -EINVAL;
}

/*
 * Return buffer size for a given pool
 */
int
octeontx_fpa_bufpool_block_size(uintptr_t handle)
{
	struct fpavf_res *res = NULL;
	uint8_t gpool;

	if (unlikely(!octeontx_fpa_handle_valid(handle)))
		return -EINVAL;

	/* get the gpool */
	gpool = octeontx_fpa_bufpool_gpool(handle);
	res = &fpadev.pool[gpool];
	return FPA_CACHE_LINE_2_OBJSZ(res->sz128);
}

uintptr_t
octeontx_fpa_bufpool_create(unsigned int object_size, unsigned int object_count,
				unsigned int buf_offset, char **va_start,
				int node_id)
{
	unsigned int gpool;
	void *memva;
	unsigned long memsz;
	uintptr_t gpool_handle;
	uintptr_t pool_bar;
	int res;

	RTE_SET_USED(node_id);
	FPAVF_STATIC_ASSERTION(sizeof(struct rte_mbuf) <=
				OCTEONTX_FPAVF_BUF_OFFSET);

	if (unlikely(*va_start == NULL))
		goto error_end;

	object_size = RTE_CACHE_LINE_ROUNDUP(object_size);
	if (object_size > FPA_MAX_OBJ_SIZE) {
		errno = EINVAL;
		goto error_end;
	}

	rte_spinlock_lock(&fpadev.lock);
	res = octeontx_fpa_gpool_alloc(object_size);

	/* Bail if failed */
	if (unlikely(res < 0)) {
		errno = res;
		goto error_unlock;
	}

	/* get fpavf */
	gpool = res;

	/* get pool handle */
	gpool_handle = octeontx_fpa_gpool2handle(gpool);
	if (!octeontx_fpa_handle_valid(gpool_handle)) {
		errno = ENOSPC;
		goto error_gpool_free;
	}

	/* Get pool bar address from handle */
	pool_bar = gpool_handle & ~(uint64_t)FPA_GPOOL_MASK;

	res = octeontx_fpapf_pool_setup(gpool, object_size, buf_offset,
					object_count);
	if (res < 0) {
		errno = res;
		goto error_gpool_free;
	}

	/* populate AURA fields */
	res = octeontx_fpapf_aura_attach(gpool);
	if (res < 0) {
		errno = res;
		goto error_pool_destroy;
	}

	/* vf pool setup */
	memsz = object_size * object_count;
	memva = *va_start;
	res = octeontx_fpavf_pool_setup(pool_bar, memsz, memva, gpool);
	if (res < 0) {
		errno = res;
		goto error_gaura_detach;
	}

	/* Release lock */
	rte_spinlock_unlock(&fpadev.lock);

	/* populate AURA registers */
	fpavf_write64(object_count, (void *)((uintptr_t)pool_bar +
			 FPA_VF_VHAURA_CNT(gpool)));
	fpavf_write64(object_count, (void *)((uintptr_t)pool_bar +
			 FPA_VF_VHAURA_CNT_LIMIT(gpool)));
	fpavf_write64(object_count + 1, (void *)((uintptr_t)pool_bar +
			 FPA_VF_VHAURA_CNT_THRESHOLD(gpool)));

	octeontx_fpapf_start_count(gpool);

	return gpool_handle;

error_gaura_detach:
	(void) octeontx_fpapf_aura_detach(gpool);
error_pool_destroy:
	octeontx_fpavf_free(gpool);
	octeontx_fpapf_pool_destroy(gpool);
error_gpool_free:
	octeontx_gpool_free(gpool);
error_unlock:
	rte_spinlock_unlock(&fpadev.lock);
error_end:
	return (uintptr_t)NULL;
}

static void
octeontx_fpavf_setup(void)
{
	uint8_t i;
	static bool init_once;

	if (!init_once) {
		rte_spinlock_init(&fpadev.lock);
		fpadev.total_gpool_cnt = 0;

		for (i = 0; i < FPA_VF_MAX; i++) {

			fpadev.pool[i].domain_id = ~0;
			fpadev.pool[i].stack_ln_ptr = 0;
			fpadev.pool[i].sz128 = 0;
			fpadev.pool[i].bar0 = NULL;
			fpadev.pool[i].pool_stack_base = NULL;
			fpadev.pool[i].is_inuse = false;
		}
		init_once = 1;
	}
}

static int
octeontx_fpavf_identify(void *bar0)
{
	uint64_t val;
	uint16_t domain_id;
	uint16_t vf_id;
	uint64_t stack_ln_ptr;

	val = fpavf_read64((void *)((uintptr_t)bar0 +
				FPA_VF_VHAURA_CNT_THRESHOLD(0)));

	domain_id = (val >> 8) & 0xffff;
	vf_id = (val >> 24) & 0xffff;

	stack_ln_ptr = fpavf_read64((void *)((uintptr_t)bar0 +
					FPA_VF_VHPOOL_THRESHOLD(0)));
	if (vf_id >= FPA_VF_MAX) {
		fpavf_log_err("vf_id(%d) greater than max vf (32)\n", vf_id);
		return -1;
	}

	if (fpadev.pool[vf_id].is_inuse) {
		fpavf_log_err("vf_id %d is_inuse\n", vf_id);
		return -1;
	}

	fpadev.pool[vf_id].domain_id = domain_id;
	fpadev.pool[vf_id].vf_id = vf_id;
	fpadev.pool[vf_id].bar0 = bar0;
	fpadev.pool[vf_id].stack_ln_ptr = stack_ln_ptr;

	/* SUCCESS */
	return vf_id;
}

/* FPAVF pcie device aka mempool probe */
static int
fpavf_probe(struct rte_pci_driver *pci_drv, struct rte_pci_device *pci_dev)
{
	uint8_t *idreg;
	int res;
	struct fpavf_res *fpa;

	RTE_SET_USED(pci_drv);
	RTE_SET_USED(fpa);

	/* For secondary processes, the primary has done all the work */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	if (pci_dev->mem_resource[0].addr == NULL) {
		fpavf_log_err("Empty bars %p ", pci_dev->mem_resource[0].addr);
		return -ENODEV;
	}
	idreg = pci_dev->mem_resource[0].addr;

	octeontx_fpavf_setup();

	res = octeontx_fpavf_identify(idreg);
	if (res < 0)
		return -1;

	fpa = &fpadev.pool[res];
	fpadev.total_gpool_cnt++;
	rte_wmb();

	fpavf_log_dbg("total_fpavfs %d bar0 %p domain %d vf %d stk_ln_ptr 0x%x",
		       fpadev.total_gpool_cnt, fpa->bar0, fpa->domain_id,
		       fpa->vf_id, (unsigned int)fpa->stack_ln_ptr);

	return 0;
}

static const struct rte_pci_id pci_fpavf_map[] = {
	{
		RTE_PCI_DEVICE(PCI_VENDOR_ID_CAVIUM,
				PCI_DEVICE_ID_OCTEONTX_FPA_VF)
	},
	{
		.vendor_id = 0,
	},
};

static struct rte_pci_driver pci_fpavf = {
	.id_table = pci_fpavf_map,
	.drv_flags = RTE_PCI_DRV_NEED_MAPPING | RTE_PCI_DRV_IOVA_AS_VA,
	.probe = fpavf_probe,
};

RTE_PMD_REGISTER_PCI(octeontx_fpavf, pci_fpavf);