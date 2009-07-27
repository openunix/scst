/*
 *  scst_mem.c
 *
 *  Copyright (C) 2006 - 2009 Vladislav Bolkhovitin <vst@vlnb.net>
 *  Copyright (C) 2007 - 2009 ID7 Ltd.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation, version 2
 *  of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/unistd.h>
#include <linux/string.h>

#include "scst.h"
#include "scst_priv.h"
#include "scst_mem.h"

#define PURGE_INTERVAL		(60 * HZ)
#define PURGE_TIME_AFTER	PURGE_INTERVAL
#define SHRINK_TIME_AFTER	(1 * HZ)

/* Max pages freed from a pool per shrinking iteration */
#define MAX_PAGES_PER_POOL	50

static struct sgv_pool sgv_norm_clust_pool, sgv_norm_pool, sgv_dma_pool;

static atomic_t sgv_pages_total = ATOMIC_INIT(0);

/* Both read-only */
static int sgv_hi_wmk;
static int sgv_lo_wmk;

static int sgv_max_local_order, sgv_max_trans_order;

static DEFINE_SPINLOCK(sgv_pools_lock); /* inner lock for sgv_pool_lock! */
static DEFINE_MUTEX(sgv_pools_mutex);

/* Both protected by sgv_pools_lock */
static struct sgv_pool *sgv_cur_purge_pool;
static LIST_HEAD(sgv_active_pools_list);

static atomic_t sgv_releases_on_hiwmk = ATOMIC_INIT(0);
static atomic_t sgv_releases_on_hiwmk_failed = ATOMIC_INIT(0);

static atomic_t sgv_other_total_alloc = ATOMIC_INIT(0);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 23))
static struct shrinker *sgv_shrinker;
#else
static struct shrinker sgv_shrinker;
#endif

/*
 * Protected by sgv_pools_mutex AND sgv_pools_lock for writes,
 * either one for reads.
 */
static LIST_HEAD(sgv_pools_list);

static void sgv_pool_get(struct sgv_pool *pool);
static void sgv_pool_put(struct sgv_pool *pool);

static inline bool sgv_pool_clustered(const struct sgv_pool *pool)
{
	return pool->clustering_type != sgv_no_clustering;
}

void scst_sgv_pool_use_norm(struct scst_tgt_dev *tgt_dev)
{
	tgt_dev->gfp_mask = __GFP_NOWARN;
	tgt_dev->pool = &sgv_norm_pool;
	clear_bit(SCST_TGT_DEV_CLUST_POOL, &tgt_dev->tgt_dev_flags);
}

void scst_sgv_pool_use_norm_clust(struct scst_tgt_dev *tgt_dev)
{
	TRACE_MEM("%s", "Use clustering");
	tgt_dev->gfp_mask = __GFP_NOWARN;
	tgt_dev->pool = &sgv_norm_clust_pool;
	set_bit(SCST_TGT_DEV_CLUST_POOL, &tgt_dev->tgt_dev_flags);
}

void scst_sgv_pool_use_dma(struct scst_tgt_dev *tgt_dev)
{
	TRACE_MEM("%s", "Use ISA DMA memory");
	tgt_dev->gfp_mask = __GFP_NOWARN | GFP_DMA;
	tgt_dev->pool = &sgv_dma_pool;
	clear_bit(SCST_TGT_DEV_CLUST_POOL, &tgt_dev->tgt_dev_flags);
}

/* Must be no locks */
static void sgv_dtor_and_free(struct sgv_pool_obj *obj)
{
	struct sgv_pool *pool = obj->owner_pool;

	TRACE_MEM("Destroying sgv obj %p", obj);

	if (obj->sg_count != 0) {
		pool->alloc_fns.free_pages_fn(obj->sg_entries,
			obj->sg_count, obj->allocator_priv);
	}
	if (obj->sg_entries != obj->sg_entries_data) {
		if (obj->trans_tbl !=
		    (struct trans_tbl_ent *)obj->sg_entries_data) {
			/* kfree() handles NULL parameter */
			kfree(obj->trans_tbl);
			obj->trans_tbl = NULL;
		}
		kfree(obj->sg_entries);
	}

	kmem_cache_free(pool->caches[obj->order_or_pages], obj);
	return;
}

/* Might be called under sgv_pool_lock */
static inline void sgv_del_from_active(struct sgv_pool *pool)
{
	struct list_head *next;

	TRACE_MEM("Deleting sgv pool %p from the active list", pool);

	spin_lock_bh(&sgv_pools_lock);

	next = pool->sgv_active_pools_list_entry.next;
	list_del(&pool->sgv_active_pools_list_entry);

	if (sgv_cur_purge_pool == pool) {
		TRACE_MEM("Sgv pool %p is sgv cur purge pool", pool);

		if (next == &sgv_active_pools_list)
			next = next->next;

		if (next == &sgv_active_pools_list) {
			sgv_cur_purge_pool = NULL;
			TRACE_MEM("%s", "Sgv active list now empty");
		} else {
			sgv_cur_purge_pool = list_entry(next, typeof(*pool),
				sgv_active_pools_list_entry);
			TRACE_MEM("New sgv cur purge pool %p",
				sgv_cur_purge_pool);
		}
	}

	spin_unlock_bh(&sgv_pools_lock);
	return;
}

/* Must be called under sgv_pool_lock held */
static void sgv_dec_cached_entries(struct sgv_pool *pool, int pages)
{
	pool->cached_entries--;
	pool->cached_pages -= pages;

	if (pool->cached_entries == 0)
		sgv_del_from_active(pool);

	return;
}

/* Must be called under sgv_pool_lock held */
static void __sgv_purge_from_cache(struct sgv_pool_obj *obj)
{
	int pages = 1 << obj->order_or_pages;
	struct sgv_pool *pool = obj->owner_pool;

	TRACE_MEM("Purging sgv obj %p from pool %p (new cached_entries %d)",
		obj, pool, pool->cached_entries-1);

	list_del(&obj->sorted_recycling_list_entry);
	list_del(&obj->recycling_list_entry);

	pool->inactive_cached_pages -= pages;
	sgv_dec_cached_entries(pool, pages);

	atomic_sub(pages, &sgv_pages_total);

	return;
}

/* Must be called under sgv_pool_lock held */
static bool sgv_purge_from_cache(struct sgv_pool_obj *obj, int after,
	unsigned long cur_time)
{
	EXTRACHECKS_BUG_ON(after < 0);

	TRACE_MEM("Checking if sgv obj %p should be purged (cur time %ld, "
		"obj time %ld, time to purge %ld)", obj, cur_time,
		obj->time_stamp, obj->time_stamp + after);

	if (time_after_eq(cur_time, (obj->time_stamp + after))) {
		__sgv_purge_from_cache(obj);
		return true;
	}
	return false;
}

/* No locks */
static int sgv_shrink_pool(struct sgv_pool *pool, int nr, int after,
	unsigned long cur_time)
{
	int freed = 0;

	TRACE_ENTRY();

	TRACE_MEM("Trying to shrink pool %p (nr %d, after %d)", pool, nr,
		after);

	spin_lock_bh(&pool->sgv_pool_lock);

	while (!list_empty(&pool->sorted_recycling_list) &&
			(atomic_read(&sgv_pages_total) > sgv_lo_wmk)) {
		struct sgv_pool_obj *obj = list_entry(
			pool->sorted_recycling_list.next,
			struct sgv_pool_obj, sorted_recycling_list_entry);

		if (sgv_purge_from_cache(obj, after, cur_time)) {
			int pages = 1 << obj->order_or_pages;

			freed += pages;
			nr -= pages;

			TRACE_MEM("%d pages purged from pool %p (nr left %d, "
				"total freed %d)", pages, pool, nr, freed);

			spin_unlock_bh(&pool->sgv_pool_lock);
			sgv_dtor_and_free(obj);
			spin_lock_bh(&pool->sgv_pool_lock);
		} else
			break;

		if ((nr <= 0) || (freed >= MAX_PAGES_PER_POOL)) {
			if (freed >= MAX_PAGES_PER_POOL)
				TRACE_MEM("%d pages purged from pool %p, "
					"leaving", freed, pool);
			break;
		}
	}

	spin_unlock_bh(&pool->sgv_pool_lock);

	TRACE_EXIT_RES(nr);
	return nr;
}

/* No locks */
static int __sgv_shrink(int nr, int after)
{
	struct sgv_pool *pool;
	unsigned long cur_time = jiffies;
	int prev_nr = nr;
	bool circle = false;

	TRACE_ENTRY();

	TRACE_MEM("Trying to shrink %d pages from all sgv pools (after %d)",
		nr, after);

	while (nr > 0) {
		struct list_head *next;

		spin_lock_bh(&sgv_pools_lock);

		pool = sgv_cur_purge_pool;
		if (pool == NULL) {
			if (list_empty(&sgv_active_pools_list)) {
				TRACE_MEM("%s", "Active pools list is empty");
				goto out_unlock;
			}

			pool = list_entry(sgv_active_pools_list.next,
					typeof(*pool),
					sgv_active_pools_list_entry);
		}
		sgv_pool_get(pool);

		next = pool->sgv_active_pools_list_entry.next;
		if (next == &sgv_active_pools_list) {
			if (circle && (prev_nr == nr)) {
				TRACE_MEM("Full circle done, but no progress, "
					"leaving (nr %d)", nr);
				goto out_unlock_put;
			}
			circle = true;
			prev_nr = nr;

			next = next->next;
		}

		sgv_cur_purge_pool = list_entry(next, typeof(*pool),
			sgv_active_pools_list_entry);
		TRACE_MEM("New cur purge pool %p", sgv_cur_purge_pool);

		spin_unlock_bh(&sgv_pools_lock);

		nr = sgv_shrink_pool(pool, nr, after, cur_time);

		sgv_pool_put(pool);
	}

out:
	TRACE_EXIT_RES(nr);
	return nr;

out_unlock:
	spin_unlock_bh(&sgv_pools_lock);
	goto out;

out_unlock_put:
	spin_unlock_bh(&sgv_pools_lock);
	sgv_pool_put(pool);
	goto out;
}

static int sgv_shrink(int nr, gfp_t gfpm)
{
	TRACE_ENTRY();

	if (nr > 0)
		nr = __sgv_shrink(nr, SHRINK_TIME_AFTER);
	else {
		struct sgv_pool *pool;
		int inactive_pages = 0;

		spin_lock_bh(&sgv_pools_lock);
		list_for_each_entry(pool, &sgv_active_pools_list,
				sgv_active_pools_list_entry) {
			inactive_pages += pool->inactive_cached_pages;
		}
		spin_unlock_bh(&sgv_pools_lock);

		nr = max((int)0, inactive_pages - sgv_lo_wmk);
	}

	TRACE_MEM("Returning %d", nr);

	TRACE_EXIT_RES(nr);
	return nr;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
static void sgv_purge_work_fn(void *p)
#else
static void sgv_purge_work_fn(struct delayed_work *work)
#endif
{
	unsigned long cur_time = jiffies;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
	struct sgv_pool *pool = (struct sgv_pool *)p;
#else
	struct sgv_pool *pool = container_of(work, struct sgv_pool,
					sgv_purge_work);
#endif

	TRACE_ENTRY();

	TRACE_MEM("Purge work for pool %p", pool);

	spin_lock_bh(&pool->sgv_pool_lock);

	pool->purge_work_scheduled = false;

	while (!list_empty(&pool->sorted_recycling_list)) {
		struct sgv_pool_obj *obj = list_entry(
			pool->sorted_recycling_list.next,
			struct sgv_pool_obj, sorted_recycling_list_entry);

		if (sgv_purge_from_cache(obj, PURGE_TIME_AFTER, cur_time)) {
			spin_unlock_bh(&pool->sgv_pool_lock);
			sgv_dtor_and_free(obj);
			spin_lock_bh(&pool->sgv_pool_lock);
		} else {
			/*
			 * Let's reschedule it for full period to not get here
			 * too often. In the worst case we have shrinker
			 * to reclaim buffers quickier.
			 */
			TRACE_MEM("Rescheduling purge work for pool %p (delay "
				"%d HZ/%d sec)", pool, PURGE_INTERVAL,
				PURGE_INTERVAL/HZ);
			schedule_delayed_work(&pool->sgv_purge_work,
				PURGE_INTERVAL);
			pool->purge_work_scheduled = true;
			break;
		}
	}

	spin_unlock_bh(&pool->sgv_pool_lock);

	TRACE_MEM("Leaving purge work for pool %p", pool);

	TRACE_EXIT();
	return;
}

static int sgv_check_full_clustering(struct scatterlist *sg, int cur, int hint)
{
	int res = -1;
	int i = hint;
	unsigned long pfn_cur = page_to_pfn(sg_page(&sg[cur]));
	int len_cur = sg[cur].length;
	unsigned long pfn_cur_next = pfn_cur + (len_cur >> PAGE_SHIFT);
	int full_page_cur = (len_cur & (PAGE_SIZE - 1)) == 0;
	unsigned long pfn, pfn_next;
	bool full_page;

#if 0
	TRACE_MEM("pfn_cur %ld, pfn_cur_next %ld, len_cur %d, full_page_cur %d",
		pfn_cur, pfn_cur_next, len_cur, full_page_cur);
#endif

	/* check the hint first */
	if (i >= 0) {
		pfn = page_to_pfn(sg_page(&sg[i]));
		pfn_next = pfn + (sg[i].length >> PAGE_SHIFT);
		full_page = (sg[i].length & (PAGE_SIZE - 1)) == 0;

		if ((pfn == pfn_cur_next) && full_page_cur)
			goto out_head;

		if ((pfn_next == pfn_cur) && full_page)
			goto out_tail;
	}

	/* ToDo: implement more intelligent search */
	for (i = cur - 1; i >= 0; i--) {
		pfn = page_to_pfn(sg_page(&sg[i]));
		pfn_next = pfn + (sg[i].length >> PAGE_SHIFT);
		full_page = (sg[i].length & (PAGE_SIZE - 1)) == 0;

		if ((pfn == pfn_cur_next) && full_page_cur)
			goto out_head;

		if ((pfn_next == pfn_cur) && full_page)
			goto out_tail;
	}

out:
	return res;

out_tail:
	TRACE_MEM("SG segment %d will be tail merged with segment %d", cur, i);
	sg[i].length += len_cur;
	sg_clear(&sg[cur]);
	res = i;
	goto out;

out_head:
	TRACE_MEM("SG segment %d will be head merged with segment %d", cur, i);
	sg_assign_page(&sg[i], sg_page(&sg[cur]));
	sg[i].length += len_cur;
	sg_clear(&sg[cur]);
	res = i;
	goto out;
}

static int sgv_check_tail_clustering(struct scatterlist *sg, int cur, int hint)
{
	int res = -1;
	unsigned long pfn_cur = page_to_pfn(sg_page(&sg[cur]));
	int len_cur = sg[cur].length;
	int prev;
	unsigned long pfn_prev;
	bool full_page;

#ifdef SCST_HIGHMEM
	if (page >= highmem_start_page) {
		TRACE_MEM("%s", "HIGHMEM page allocated, no clustering")
		goto out;
	}
#endif

#if 0
	TRACE_MEM("pfn_cur %ld, pfn_cur_next %ld, len_cur %d, full_page_cur %d",
		pfn_cur, pfn_cur_next, len_cur, full_page_cur);
#endif

	if (cur == 0)
		goto out;

	prev = cur - 1;
	pfn_prev = page_to_pfn(sg_page(&sg[prev])) +
			(sg[prev].length >> PAGE_SHIFT);
	full_page = (sg[prev].length & (PAGE_SIZE - 1)) == 0;

	if ((pfn_prev == pfn_cur) && full_page) {
		TRACE_MEM("SG segment %d will be tail merged with segment %d",
			cur, prev);
		sg[prev].length += len_cur;
		sg_clear(&sg[cur]);
		res = prev;
	}

out:
	return res;
}

static void sgv_free_sys_sg_entries(struct scatterlist *sg, int sg_count,
	void *priv)
{
	int i;

	TRACE_MEM("sg=%p, sg_count=%d", sg, sg_count);

	for (i = 0; i < sg_count; i++) {
		struct page *p = sg_page(&sg[i]);
		int len = sg[i].length;
		int pages =
			(len >> PAGE_SHIFT) + ((len & ~PAGE_MASK) != 0);

		TRACE_MEM("page %lx, len %d, pages %d",
			(unsigned long)p, len, pages);

		while (pages > 0) {
			int order = 0;

/*
 * __free_pages() doesn't like freeing pages with not that order with
 * which they were allocated, so disable this small optimization.
 */
#if 0
			if (len > 0) {
				while (((1 << order) << PAGE_SHIFT) < len)
					order++;
				len = 0;
			}
#endif
			TRACE_MEM("free_pages(): order %d, page %lx",
				order, (unsigned long)p);

			__free_pages(p, order);

			pages -= 1 << order;
			p += 1 << order;
		}
	}
}

static struct page *sgv_alloc_sys_pages(struct scatterlist *sg,
	gfp_t gfp_mask, void *priv)
{
	struct page *page = alloc_pages(gfp_mask, 0);

	sg_set_page(sg, page, PAGE_SIZE, 0);
	TRACE_MEM("page=%p, sg=%p, priv=%p", page, sg, priv);
	if (page == NULL) {
		TRACE(TRACE_OUT_OF_MEM, "%s", "Allocation of "
			"sg page failed");
	}
	return page;
}

static int sgv_alloc_sg_entries(struct scatterlist *sg, int pages,
	gfp_t gfp_mask, enum sgv_clustering_types clustering_type,
	struct trans_tbl_ent *trans_tbl,
	const struct sgv_pool_alloc_fns *alloc_fns, void *priv)
{
	int sg_count = 0;
	int pg, i, j;
	int merged = -1;

	TRACE_MEM("pages=%d, clustering_type=%d", pages, clustering_type);

#if 0
	gfp_mask |= __GFP_COLD;
#endif
#ifdef CONFIG_SCST_STRICT_SECURITY
	gfp_mask |= __GFP_ZERO;
#endif

	for (pg = 0; pg < pages; pg++) {
		void *rc;
#ifdef CONFIG_SCST_DEBUG_OOM
		if (((gfp_mask & __GFP_NOFAIL) != __GFP_NOFAIL) &&
		    ((scst_random() % 10000) == 55))
			rc = NULL;
		else
#endif
			rc = alloc_fns->alloc_pages_fn(&sg[sg_count], gfp_mask,
				priv);
		if (rc == NULL)
			goto out_no_mem;

		/*
		 * This code allows compiler to see full body of the clustering
		 * functions and gives it a chance to generate better code.
		 * At least, the resulting code is smaller, comparing to
		 * calling them using a function pointer.
		 */
		if (clustering_type == sgv_full_clustering)
			merged = sgv_check_full_clustering(sg, sg_count, merged);
		else if (clustering_type == sgv_tail_clustering)
			merged = sgv_check_tail_clustering(sg, sg_count, merged);
		else
			merged = -1;

		if (merged == -1)
			sg_count++;

		TRACE_MEM("pg=%d, merged=%d, sg_count=%d", pg, merged,
			sg_count);
	}

	if ((clustering_type != sgv_no_clustering) && (trans_tbl != NULL)) {
		pg = 0;
		for (i = 0; i < pages; i++) {
			int n = (sg[i].length >> PAGE_SHIFT) +
				((sg[i].length & ~PAGE_MASK) != 0);
			trans_tbl[i].pg_count = pg;
			for (j = 0; j < n; j++)
				trans_tbl[pg++].sg_num = i+1;
			TRACE_MEM("i=%d, n=%d, pg_count=%d", i, n,
				trans_tbl[i].pg_count);
		}
	}

out:
	TRACE_MEM("sg_count=%d", sg_count);
	return sg_count;

out_no_mem:
	alloc_fns->free_pages_fn(sg, sg_count, priv);
	sg_count = 0;
	goto out;
}

static int sgv_alloc_arrays(struct sgv_pool_obj *obj,
	int pages_to_alloc, int order, gfp_t gfp_mask)
{
	int sz, tsz = 0;
	int res = 0;

	TRACE_ENTRY();

	sz = pages_to_alloc * sizeof(obj->sg_entries[0]);

	obj->sg_entries = kmalloc(sz, gfp_mask);
	if (unlikely(obj->sg_entries == NULL)) {
		TRACE(TRACE_OUT_OF_MEM, "Allocation of sgv_pool_obj "
			"SG vector failed (size %d)", sz);
		res = -ENOMEM;
		goto out;
	}

	sg_init_table(obj->sg_entries, pages_to_alloc);

	if (sgv_pool_clustered(obj->owner_pool)) {
		if (order <= sgv_max_trans_order) {
			obj->trans_tbl =
				(struct trans_tbl_ent *)obj->sg_entries_data;
			/*
			 * No need to clear trans_tbl, if needed, it will be
			 * fully rewritten in sgv_alloc_sg_entries()
			 */
		} else {
			tsz = pages_to_alloc * sizeof(obj->trans_tbl[0]);
			obj->trans_tbl = kzalloc(tsz, gfp_mask);
			if (unlikely(obj->trans_tbl == NULL)) {
				TRACE(TRACE_OUT_OF_MEM, "Allocation of "
					"trans_tbl failed (size %d)", tsz);
				res = -ENOMEM;
				goto out_free;
			}
		}
	}

	TRACE_MEM("pages_to_alloc %d, order %d, sz %d, tsz %d, obj %p, "
		"sg_entries %p, trans_tbl %p", pages_to_alloc, order,
		sz, tsz, obj, obj->sg_entries, obj->trans_tbl);

out:
	TRACE_EXIT_RES(res);
	return res;

out_free:
	kfree(obj->sg_entries);
	obj->sg_entries = NULL;
	goto out;
}

static struct sgv_pool_obj *sgv_get_obj(struct sgv_pool *pool, int order,
	gfp_t gfp_mask)
{
	struct sgv_pool_obj *obj;
	int pages = 1 << order;

	spin_lock_bh(&pool->sgv_pool_lock);
	if (likely(!list_empty(&pool->recycling_lists[order]))) {
		obj = list_entry(pool->recycling_lists[order].next,
			 struct sgv_pool_obj, recycling_list_entry);

		list_del(&obj->sorted_recycling_list_entry);
		list_del(&obj->recycling_list_entry);

		pool->inactive_cached_pages -= pages;

		spin_unlock_bh(&pool->sgv_pool_lock);

		EXTRACHECKS_BUG_ON(obj->order_or_pages != order);
		goto out;
	}

	if (pool->cached_entries == 0) {
		TRACE_MEM("Adding pool %p to the active list", pool);
		spin_lock_bh(&sgv_pools_lock);
		list_add_tail(&pool->sgv_active_pools_list_entry,
			&sgv_active_pools_list);
		spin_unlock_bh(&sgv_pools_lock);
	}

	pool->cached_entries++;
	pool->cached_pages += pages;

	spin_unlock_bh(&pool->sgv_pool_lock);

	TRACE_MEM("New cached entries %d (pool %p)", pool->cached_entries,
		pool);

	obj = kmem_cache_alloc(pool->caches[order],
		gfp_mask & ~(__GFP_HIGHMEM|GFP_DMA));
	if (likely(obj)) {
		memset(obj, 0, sizeof(*obj));
		obj->order_or_pages = order;
		obj->owner_pool = pool;
	} else {
		spin_lock_bh(&pool->sgv_pool_lock);
		sgv_dec_cached_entries(pool, pages);
		spin_unlock_bh(&pool->sgv_pool_lock);
	}

out:
	return obj;
}

static void sgv_put_obj(struct sgv_pool_obj *obj)
{
	struct sgv_pool *pool = obj->owner_pool;
	struct list_head *entry;
	struct list_head *list = &pool->recycling_lists[obj->order_or_pages];
	int pages = 1 << obj->order_or_pages;

	EXTRACHECKS_BUG_ON(obj->order_or_pages < 0);

	spin_lock_bh(&pool->sgv_pool_lock);

	TRACE_MEM("sgv %p, order %d, sg_count %d", obj, obj->order_or_pages,
		obj->sg_count);

	if (sgv_pool_clustered(pool)) {
		/* Make objects with less entries more preferred */
		__list_for_each(entry, list) {
			struct sgv_pool_obj *tmp = list_entry(entry,
				struct sgv_pool_obj, recycling_list_entry);

			TRACE_MEM("tmp %p, order %d, sg_count %d", tmp,
				tmp->order_or_pages, tmp->sg_count);

			if (obj->sg_count <= tmp->sg_count)
				break;
		}
		entry = entry->prev;
	} else
		entry = list;

	TRACE_MEM("Adding in %p (list %p)", entry, list);
	list_add(&obj->recycling_list_entry, entry);

	list_add_tail(&obj->sorted_recycling_list_entry,
		&pool->sorted_recycling_list);

	obj->time_stamp = jiffies;

	pool->inactive_cached_pages += pages;

	if (!pool->purge_work_scheduled) {
		TRACE_MEM("Scheduling purge work for pool %p", pool);
		pool->purge_work_scheduled = true;
		schedule_delayed_work(&pool->sgv_purge_work, PURGE_INTERVAL);
	}

	spin_unlock_bh(&pool->sgv_pool_lock);
	return;
}

/* No locks */
static int sgv_hiwmk_check(int pages_to_alloc)
{
	int res = 0;
	int pages = pages_to_alloc;

	pages += atomic_read(&sgv_pages_total);

	if (unlikely(pages > sgv_hi_wmk)) {
		pages -= sgv_hi_wmk;
		atomic_inc(&sgv_releases_on_hiwmk);

		pages = __sgv_shrink(pages, 0);
		if (pages > 0) {
			TRACE(TRACE_OUT_OF_MEM, "Requested amount of "
			    "memory (%d pages) for being executed "
			    "commands together with the already "
			    "allocated memory exceeds the allowed "
			    "maximum %d. Should you increase "
			    "scst_max_cmd_mem?", pages_to_alloc,
			   sgv_hi_wmk);
			atomic_inc(&sgv_releases_on_hiwmk_failed);
			res = -ENOMEM;
			goto out_unlock;
		}
	}

	atomic_add(pages_to_alloc, &sgv_pages_total);

out_unlock:
	TRACE_MEM("pages_to_alloc %d, new total %d", pages_to_alloc,
		atomic_read(&sgv_pages_total));

	return res;
}

/* No locks */
static void sgv_hiwmk_uncheck(int pages)
{
	atomic_sub(pages, &sgv_pages_total);
	TRACE_MEM("pages %d, new total %d", pages,
		atomic_read(&sgv_pages_total));
	return;
}

/* No locks */
static bool sgv_check_allowed_mem(struct scst_mem_lim *mem_lim, int pages)
{
	int alloced;
	bool res = true;

	alloced = atomic_add_return(pages, &mem_lim->alloced_pages);
	if (unlikely(alloced > mem_lim->max_allowed_pages)) {
		TRACE(TRACE_OUT_OF_MEM, "Requested amount of memory "
			"(%d pages) for being executed commands on a device "
			"together with the already allocated memory exceeds "
			"the allowed maximum %d. Should you increase "
			"scst_max_dev_cmd_mem?", pages,
			mem_lim->max_allowed_pages);
		atomic_sub(pages, &mem_lim->alloced_pages);
		res = false;
	}

	TRACE_MEM("mem_lim %p, pages %d, res %d, new alloced %d", mem_lim,
		pages, res, atomic_read(&mem_lim->alloced_pages));

	return res;
}

/* No locks */
static void sgv_uncheck_allowed_mem(struct scst_mem_lim *mem_lim, int pages)
{
	atomic_sub(pages, &mem_lim->alloced_pages);

	TRACE_MEM("mem_lim %p, pages %d, new alloced %d", mem_lim,
		pages, atomic_read(&mem_lim->alloced_pages));
	return;
}

struct scatterlist *sgv_pool_alloc(struct sgv_pool *pool, unsigned int size,
	gfp_t gfp_mask, int flags, int *count,
	struct sgv_pool_obj **sgv, struct scst_mem_lim *mem_lim, void *priv)
{
	struct sgv_pool_obj *obj;
	int order, pages, cnt;
	struct scatterlist *res = NULL;
	int pages_to_alloc;
	struct kmem_cache *cache;
	int no_cached = flags & SCST_POOL_ALLOC_NO_CACHED;
	bool allowed_mem_checked = false, hiwmk_checked = false;

	TRACE_ENTRY();

	if (unlikely(size == 0))
		goto out;

	sBUG_ON((gfp_mask & __GFP_NOFAIL) == __GFP_NOFAIL);

	pages = ((size + PAGE_SIZE - 1) >> PAGE_SHIFT);
	order = get_order(size);

	TRACE_MEM("size=%d, pages=%d, order=%d, flags=%x, *sgv %p", size, pages,
		order, flags, *sgv);

	if (*sgv != NULL) {
		obj = *sgv;
		pages_to_alloc = (1 << order);
		cache = pool->caches[obj->order_or_pages];

		TRACE_MEM("Supplied obj %p, sgv_order %d", obj,
			obj->order_or_pages);

		EXTRACHECKS_BUG_ON(obj->order_or_pages != order);
		EXTRACHECKS_BUG_ON(obj->sg_count != 0);

		if (unlikely(!sgv_check_allowed_mem(mem_lim, pages_to_alloc)))
			goto out_fail_free_sg_entries;
		allowed_mem_checked = true;

		if (unlikely(sgv_hiwmk_check(pages_to_alloc) != 0))
			goto out_fail_free_sg_entries;
		hiwmk_checked = true;
	} else if ((order < SGV_POOL_ELEMENTS) && !no_cached) {
		pages_to_alloc = (1 << order);
		cache = pool->caches[order];

		if (unlikely(!sgv_check_allowed_mem(mem_lim, pages_to_alloc)))
			goto out_fail;
		allowed_mem_checked = true;

		obj = sgv_get_obj(pool, order, gfp_mask);
		if (unlikely(obj == NULL)) {
			TRACE(TRACE_OUT_OF_MEM, "Allocation of "
				"sgv_pool_obj failed (size %d)", size);
			goto out_fail;
		}

		if (obj->sg_count != 0) {
			TRACE_MEM("Cached obj %p", obj);
			EXTRACHECKS_BUG_ON(obj->order_or_pages != order);
			atomic_inc(&pool->cache_acc[order].hit_alloc);
			goto success;
		}

		if (flags & SCST_POOL_NO_ALLOC_ON_CACHE_MISS) {
			if (!(flags & SCST_POOL_RETURN_OBJ_ON_ALLOC_FAIL))
				goto out_fail_free;
		}

		TRACE_MEM("Brand new obj %p", obj);

		if (order <= sgv_max_local_order) {
			obj->sg_entries = obj->sg_entries_data;
			sg_init_table(obj->sg_entries, pages_to_alloc);
			TRACE_MEM("sg_entries %p", obj->sg_entries);
			if (sgv_pool_clustered(pool)) {
				obj->trans_tbl = (struct trans_tbl_ent *)
					(obj->sg_entries + pages_to_alloc);
				TRACE_MEM("trans_tbl %p", obj->trans_tbl);
				/*
				 * No need to clear trans_tbl, if needed, it
				 * will be fully rewritten in
				 * sgv_alloc_sg_entries(),
				 */
			}
		} else {
			if (unlikely(sgv_alloc_arrays(obj, pages_to_alloc,
					order, gfp_mask) != 0))
				goto out_fail_free;
		}

		if ((flags & SCST_POOL_NO_ALLOC_ON_CACHE_MISS) &&
		    (flags & SCST_POOL_RETURN_OBJ_ON_ALLOC_FAIL))
			goto out_return;

		obj->allocator_priv = priv;

		if (unlikely(sgv_hiwmk_check(pages_to_alloc) != 0))
			goto out_fail_free_sg_entries;
		hiwmk_checked = true;
	} else {
		int sz;

		pages_to_alloc = pages;

		if (unlikely(!sgv_check_allowed_mem(mem_lim, pages_to_alloc)))
			goto out_fail;
		allowed_mem_checked = true;

		if (flags & SCST_POOL_NO_ALLOC_ON_CACHE_MISS)
			goto out_return2;

		cache = NULL;
		sz = sizeof(*obj) + pages*sizeof(obj->sg_entries[0]);

		obj = kmalloc(sz, gfp_mask);
		if (unlikely(obj == NULL)) {
			TRACE(TRACE_OUT_OF_MEM, "Allocation of "
				"sgv_pool_obj failed (size %d)", size);
			goto out_fail;
		}
		memset(obj, 0, sizeof(*obj));

		obj->owner_pool = pool;
		obj->order_or_pages = -pages_to_alloc;
		obj->allocator_priv = priv;

		obj->sg_entries = obj->sg_entries_data;
		sg_init_table(obj->sg_entries, pages);

		if (unlikely(sgv_hiwmk_check(pages_to_alloc) != 0))
			goto out_fail_free_sg_entries;
		hiwmk_checked = true;

		TRACE_MEM("Big or no_cached obj %p (size %d)", obj,	sz);
	}

	obj->sg_count = sgv_alloc_sg_entries(obj->sg_entries,
		pages_to_alloc, gfp_mask, pool->clustering_type,
		obj->trans_tbl, &pool->alloc_fns, priv);
	if (unlikely(obj->sg_count <= 0)) {
		obj->sg_count = 0;
		if ((flags & SCST_POOL_RETURN_OBJ_ON_ALLOC_FAIL) && cache)
			goto out_return1;
		else
			goto out_fail_free_sg_entries;
	}

	if (cache) {
		atomic_add(pages_to_alloc - obj->sg_count,
			&pool->cache_acc[order].merged);
	} else {
		if (no_cached) {
			atomic_add(pages_to_alloc,
				&pool->other_pages);
			atomic_add(pages_to_alloc - obj->sg_count,
				&pool->other_merged);
		} else {
			atomic_add(pages_to_alloc,
				&pool->big_pages);
			atomic_add(pages_to_alloc - obj->sg_count,
				&pool->big_merged);
		}
	}

success:
	if (cache) {
		int sg;
		atomic_inc(&pool->cache_acc[order].total_alloc);
		if (sgv_pool_clustered(pool))
			cnt = obj->trans_tbl[pages-1].sg_num;
		else
			cnt = pages;
		sg = cnt-1;
		obj->orig_sg = sg;
		obj->orig_length = obj->sg_entries[sg].length;
		if (sgv_pool_clustered(pool)) {
			obj->sg_entries[sg].length =
				(pages - obj->trans_tbl[sg].pg_count) << PAGE_SHIFT;
		}
	} else {
		cnt = obj->sg_count;
		if (no_cached)
			atomic_inc(&pool->other_alloc);
		else
			atomic_inc(&pool->big_alloc);
	}

	*count = cnt;
	res = obj->sg_entries;
	*sgv = obj;

	if (size & ~PAGE_MASK)
		obj->sg_entries[cnt-1].length -=
			PAGE_SIZE - (size & ~PAGE_MASK);

	TRACE_MEM("obj=%p, sg_entries %p (size=%d, pages=%d, sg_count=%d, "
		"count=%d, last_len=%d)", obj, obj->sg_entries, size, pages,
		obj->sg_count, *count, obj->sg_entries[obj->orig_sg].length);

out:
	TRACE_EXIT_HRES(res);
	return res;

out_return:
	obj->allocator_priv = priv;
	obj->owner_pool = pool;

out_return1:
	*sgv = obj;
	TRACE_MEM("Returning failed obj %p (count %d)", obj, *count);

out_return2:
	*count = pages_to_alloc;
	res = NULL;
	goto out_uncheck;

out_fail_free_sg_entries:
	if (obj->sg_entries != obj->sg_entries_data) {
		if (obj->trans_tbl !=
			(struct trans_tbl_ent *)obj->sg_entries_data) {
			/* kfree() handles NULL parameter */
			kfree(obj->trans_tbl);
			obj->trans_tbl = NULL;
		}
		kfree(obj->sg_entries);
		obj->sg_entries = NULL;
	}

out_fail_free:
	if (cache) {
		spin_lock_bh(&pool->sgv_pool_lock);
		sgv_dec_cached_entries(pool, pages_to_alloc);
		spin_unlock_bh(&pool->sgv_pool_lock);

		kmem_cache_free(pool->caches[obj->order_or_pages], obj);
	} else
		kfree(obj);

out_fail:
	res = NULL;
	*count = 0;
	*sgv = NULL;
	TRACE_MEM("%s", "Allocation failed");

out_uncheck:
	if (hiwmk_checked)
		sgv_hiwmk_uncheck(pages_to_alloc);
	if (allowed_mem_checked)
		sgv_uncheck_allowed_mem(mem_lim, pages_to_alloc);
	goto out;
}
EXPORT_SYMBOL(sgv_pool_alloc);

void *sgv_get_priv(struct sgv_pool_obj *obj)
{
	return obj->allocator_priv;
}
EXPORT_SYMBOL(sgv_get_priv);

void sgv_pool_free(struct sgv_pool_obj *obj, struct scst_mem_lim *mem_lim)
{
	int pages;

	TRACE_MEM("Freeing obj %p, order %d, sg_entries %p, "
		"sg_count %d, allocator_priv %p", obj, obj->order_or_pages,
		obj->sg_entries, obj->sg_count, obj->allocator_priv);

	if (obj->order_or_pages >= 0) {
		obj->sg_entries[obj->orig_sg].length = obj->orig_length;
		pages = (obj->sg_count != 0) ? 1 << obj->order_or_pages : 0;
		sgv_put_obj(obj);
	} else {
		obj->owner_pool->alloc_fns.free_pages_fn(obj->sg_entries,
			obj->sg_count, obj->allocator_priv);
		pages = (obj->sg_count != 0) ? -obj->order_or_pages : 0;
		kfree(obj);
		sgv_hiwmk_uncheck(pages);
	}

	sgv_uncheck_allowed_mem(mem_lim, pages);

	return;
}
EXPORT_SYMBOL(sgv_pool_free);

struct scatterlist *scst_alloc(int size, gfp_t gfp_mask, int *count)
{
	struct scatterlist *res;
	int pages = (size >> PAGE_SHIFT) + ((size & ~PAGE_MASK) != 0);
	struct sgv_pool_alloc_fns sys_alloc_fns = {
		sgv_alloc_sys_pages, sgv_free_sys_sg_entries };
	int no_fail = ((gfp_mask & __GFP_NOFAIL) == __GFP_NOFAIL);

	TRACE_ENTRY();

	atomic_inc(&sgv_other_total_alloc);

	if (unlikely(sgv_hiwmk_check(pages) != 0)) {
		if (!no_fail) {
			res = NULL;
			goto out;
		} else {
			/*
			 * Update active_pages_total since alloc can't fail.
			 * If it wasn't updated then the counter would cross 0
			 * on free again.
			 */
			sgv_hiwmk_uncheck(-pages);
		 }
	}

	res = kmalloc(pages*sizeof(*res), gfp_mask);
	if (res == NULL) {
		TRACE(TRACE_OUT_OF_MEM, "Unable to allocate sg for %d pages",
			pages);
		goto out_uncheck;
	}

	sg_init_table(res, pages);

	/*
	 * If we allow use clustering here, we will have troubles in
	 * scst_free() to figure out how many pages are in the SG vector.
	 * So, always don't use clustering.
	 */
	*count = sgv_alloc_sg_entries(res, pages, gfp_mask, sgv_no_clustering,
			NULL, &sys_alloc_fns, NULL);
	if (*count <= 0)
		goto out_free;

out:
	TRACE_MEM("Alloced sg %p (count %d) \"no fail\" %d", res, *count, no_fail);

	TRACE_EXIT_HRES(res);
	return res;

out_free:
	kfree(res);
	res = NULL;

out_uncheck:
	if (!no_fail)
		sgv_hiwmk_uncheck(pages);
	goto out;
}
EXPORT_SYMBOL(scst_alloc);

void scst_free(struct scatterlist *sg, int count)
{
	TRACE_MEM("Freeing sg=%p", sg);

	sgv_hiwmk_uncheck(count);

	sgv_free_sys_sg_entries(sg, count, NULL);
	kfree(sg);
	return;
}
EXPORT_SYMBOL(scst_free);

/* Must be called under sgv_pools_mutex */
int sgv_pool_init(struct sgv_pool *pool, const char *name,
	enum sgv_clustering_types clustering_type)
{
	int res = -ENOMEM;
	int i;
	struct sgv_pool_obj *obj;

	TRACE_ENTRY();

	memset(pool, 0, sizeof(*pool));

	atomic_set(&pool->big_alloc, 0);
	atomic_set(&pool->big_pages, 0);
	atomic_set(&pool->big_merged, 0);
	atomic_set(&pool->other_alloc, 0);
	atomic_set(&pool->other_pages, 0);
	atomic_set(&pool->other_merged, 0);

	pool->clustering_type = clustering_type;
	pool->alloc_fns.alloc_pages_fn = sgv_alloc_sys_pages;
	pool->alloc_fns.free_pages_fn = sgv_free_sys_sg_entries;

	TRACE_MEM("name %s, sizeof(*obj)=%zd, clustering_type=%d", name,
		sizeof(*obj), clustering_type);

	strncpy(pool->name, name, sizeof(pool->name)-1);
	pool->name[sizeof(pool->name)-1] = '\0';

	pool->owner_mm = current->mm;

	for (i = 0; i < SGV_POOL_ELEMENTS; i++) {
		int size;

		atomic_set(&pool->cache_acc[i].total_alloc, 0);
		atomic_set(&pool->cache_acc[i].hit_alloc, 0);
		atomic_set(&pool->cache_acc[i].merged, 0);

		if (i <= sgv_max_local_order) {
			size = sizeof(*obj) + (1 << i) *
				(sizeof(obj->sg_entries[0]) +
				 ((clustering_type != sgv_no_clustering) ?
					sizeof(obj->trans_tbl[0]) : 0));
		} else if (i <= sgv_max_trans_order) {
			/*
			 * sgv ie sg_entries is allocated outside object, but
			 * ttbl is still embedded.
			 */
			size = sizeof(*obj) + (1 << i) *
				(((clustering_type != sgv_no_clustering) ?
					sizeof(obj->trans_tbl[0]) : 0));
		} else {
			size = sizeof(*obj);
			/* both sgv and ttbl are kallocated() */
		}

		TRACE_MEM("pages=%d, size=%d", 1 << i, size);

		scnprintf(pool->cache_names[i], sizeof(pool->cache_names[i]),
			"%s-%luK", name, (PAGE_SIZE >> 10) << i);
		pool->caches[i] = kmem_cache_create(pool->cache_names[i],
			size, 0, SCST_SLAB_FLAGS, NULL
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 23))
			, NULL);
#else
			);
#endif
		if (pool->caches[i] == NULL) {
			TRACE(TRACE_OUT_OF_MEM, "Allocation of sgv_pool cache "
				"%s(%d) failed", name, i);
			goto out_free;
		}
	}

	atomic_set(&pool->sgv_pool_ref, 1);
	spin_lock_init(&pool->sgv_pool_lock);
	INIT_LIST_HEAD(&pool->sorted_recycling_list);
	for (i = 0; i < SGV_POOL_ELEMENTS; i++)
		INIT_LIST_HEAD(&pool->recycling_lists[i]);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 20))
	INIT_DELAYED_WORK(&pool->sgv_purge_work,
		(void (*)(struct work_struct *))sgv_purge_work_fn);
#else
	INIT_WORK(&pool->sgv_purge_work, sgv_purge_work_fn, pool);
#endif

	spin_lock_bh(&sgv_pools_lock);
	list_add_tail(&pool->sgv_pools_list_entry, &sgv_pools_list);
	spin_unlock_bh(&sgv_pools_lock);

	res = 0;

out:
	TRACE_EXIT_RES(res);
	return res;

out_free:
	for (i = 0; i < SGV_POOL_ELEMENTS; i++) {
		if (pool->caches[i]) {
			kmem_cache_destroy(pool->caches[i]);
			pool->caches[i] = NULL;
		} else
			break;
	}
	goto out;
}

static void sgv_evaluate_local_order(void)
{
	int space4sgv_ttbl = PAGE_SIZE - sizeof(struct sgv_pool_obj);

	sgv_max_local_order = get_order(
		(((space4sgv_ttbl /
		  (sizeof(struct trans_tbl_ent) + sizeof(struct scatterlist))) *
			PAGE_SIZE) & PAGE_MASK)) - 1;

	sgv_max_trans_order = get_order(
		(((space4sgv_ttbl / sizeof(struct trans_tbl_ent)) * PAGE_SIZE)
		 & PAGE_MASK)) - 1;

	TRACE_MEM("sgv_max_local_order %d, sgv_max_trans_order %d",
		sgv_max_local_order, sgv_max_trans_order);
	TRACE_MEM("max object size with embedded sgv & ttbl %zd",
		(1 << sgv_max_local_order) * (sizeof(struct trans_tbl_ent) +
						sizeof(struct scatterlist)) +
		sizeof(struct sgv_pool_obj));
	TRACE_MEM("max object size with embedded sgv (!clustered) %zd",
		(1 << sgv_max_local_order) * sizeof(struct scatterlist) +
		sizeof(struct sgv_pool_obj));
	TRACE_MEM("max object size with embedded ttbl %zd",
		(1 << sgv_max_trans_order) * sizeof(struct trans_tbl_ent)
		+ sizeof(struct sgv_pool_obj));
	return;
}

void sgv_pool_flush(struct sgv_pool *pool)
{
	int i;

	TRACE_ENTRY();

	for (i = 0; i < SGV_POOL_ELEMENTS; i++) {
		struct sgv_pool_obj *obj;

		spin_lock_bh(&pool->sgv_pool_lock);

		while (!list_empty(&pool->recycling_lists[i])) {
			obj = list_entry(pool->recycling_lists[i].next,
				struct sgv_pool_obj, recycling_list_entry);

			__sgv_purge_from_cache(obj);

			spin_unlock_bh(&pool->sgv_pool_lock);

			EXTRACHECKS_BUG_ON(obj->owner_pool != pool);
			sgv_dtor_and_free(obj);

			spin_lock_bh(&pool->sgv_pool_lock);
		}
		spin_unlock_bh(&pool->sgv_pool_lock);
	}

	TRACE_EXIT();
	return;
}
EXPORT_SYMBOL(sgv_pool_flush);

void sgv_pool_deinit(struct sgv_pool *pool)
{
	int i;

	TRACE_ENTRY();

	cancel_delayed_work_sync(&pool->sgv_purge_work);

	sgv_pool_flush(pool);

	mutex_lock(&sgv_pools_mutex);
	spin_lock_bh(&sgv_pools_lock);
	list_del(&pool->sgv_pools_list_entry);
	spin_unlock_bh(&sgv_pools_lock);
	mutex_unlock(&sgv_pools_mutex);

	for (i = 0; i < SGV_POOL_ELEMENTS; i++) {
		if (pool->caches[i])
			kmem_cache_destroy(pool->caches[i]);
		pool->caches[i] = NULL;
	}

	TRACE_EXIT();
	return;
}

void sgv_pool_set_allocator(struct sgv_pool *pool,
	struct page *(*alloc_pages_fn)(struct scatterlist *, gfp_t, void *),
	void (*free_pages_fn)(struct scatterlist *, int, void *))
{
	pool->alloc_fns.alloc_pages_fn = alloc_pages_fn;
	pool->alloc_fns.free_pages_fn = free_pages_fn;
	return;
}
EXPORT_SYMBOL(sgv_pool_set_allocator);

struct sgv_pool *sgv_pool_create(const char *name,
	enum sgv_clustering_types clustering_type, bool shared)
{
	struct sgv_pool *pool;
	int rc;

	TRACE_ENTRY();

	mutex_lock(&sgv_pools_mutex);
	list_for_each_entry(pool, &sgv_pools_list, sgv_pools_list_entry) {
		if (strcmp(pool->name, name) == 0) {
			if (shared) {
				if (pool->owner_mm != current->mm) {
					PRINT_ERROR("Attempt of a shared use "
						"of SGV pool %s with "
						"different MM", name);
					goto out_err_unlock;
				}
				sgv_pool_get(pool);
				goto out_unlock;
			} else {
				PRINT_ERROR("SGV pool %s already exists", name);
				goto out_err_unlock;
			}
		}
	}

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (pool == NULL) {
		TRACE(TRACE_OUT_OF_MEM, "%s", "Allocation of sgv_pool failed");
		goto out_unlock;
	}

	rc = sgv_pool_init(pool, name, clustering_type);
	if (rc != 0)
		goto out_free_unlock;

out_unlock:
	mutex_unlock(&sgv_pools_mutex);

	TRACE_EXIT_RES(pool != NULL);
	return pool;

out_free_unlock:
	kfree(pool);

out_err_unlock:
	pool = NULL;
	goto out_unlock;
}
EXPORT_SYMBOL(sgv_pool_create);

static void sgv_pool_destroy(struct sgv_pool *pool)
{
	TRACE_ENTRY();

	sgv_pool_deinit(pool);
	kfree(pool);

	TRACE_EXIT();
	return;
}

static void sgv_pool_get(struct sgv_pool *pool)
{
	atomic_inc(&pool->sgv_pool_ref);
	TRACE_MEM("Incrementing sgv pool %p ref (new value %d)",
		pool, atomic_read(&pool->sgv_pool_ref));
	return;
}

static void sgv_pool_put(struct sgv_pool *pool)
{
	TRACE_MEM("Decrementing sgv pool %p ref (new value %d)",
		pool, atomic_read(&pool->sgv_pool_ref)-1);
	if (atomic_dec_and_test(&pool->sgv_pool_ref))
		sgv_pool_destroy(pool);
	return;
}

void sgv_pool_del(struct sgv_pool *pool)
{
	TRACE_ENTRY();

	sgv_pool_put(pool);

	TRACE_EXIT();
	return;
}
EXPORT_SYMBOL(sgv_pool_del);

/* Both parameters in pages */
int scst_sgv_pools_init(unsigned long mem_hwmark, unsigned long mem_lwmark)
{
	int res;

	TRACE_ENTRY();

	sgv_hi_wmk = mem_hwmark;
	sgv_lo_wmk = mem_lwmark;

	sgv_evaluate_local_order();

	mutex_lock(&sgv_pools_mutex);

	res = sgv_pool_init(&sgv_norm_pool, "sgv", sgv_no_clustering);
	if (res != 0)
		goto out_unlock;

	res = sgv_pool_init(&sgv_norm_clust_pool, "sgv-clust",
		sgv_full_clustering);
	if (res != 0)
		goto out_free_norm;

	res = sgv_pool_init(&sgv_dma_pool, "sgv-dma", sgv_no_clustering);
	if (res != 0)
		goto out_free_clust;

	mutex_unlock(&sgv_pools_mutex);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 23))
	sgv_shrinker = set_shrinker(DEFAULT_SEEKS, sgv_shrink);
#else
	sgv_shrinker.shrink = sgv_shrink;
	sgv_shrinker.seeks = DEFAULT_SEEKS;
	register_shrinker(&sgv_shrinker);
#endif

out:
	TRACE_EXIT_RES(res);
	return res;

out_free_clust:
	sgv_pool_deinit(&sgv_norm_clust_pool);

out_free_norm:
	sgv_pool_deinit(&sgv_norm_pool);

out_unlock:
	mutex_unlock(&sgv_pools_mutex);
	goto out;
}

void scst_sgv_pools_deinit(void)
{
	TRACE_ENTRY();

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 23))
	remove_shrinker(sgv_shrinker);
#else
	unregister_shrinker(&sgv_shrinker);
#endif

	sgv_pool_deinit(&sgv_dma_pool);
	sgv_pool_deinit(&sgv_norm_pool);
	sgv_pool_deinit(&sgv_norm_clust_pool);

	flush_scheduled_work();

	TRACE_EXIT();
	return;
}

static void sgv_do_proc_read(struct seq_file *seq, const struct sgv_pool *pool)
{
	int i, total = 0, hit = 0, merged = 0, allocated = 0;
	int oa, om;

	for (i = 0; i < SGV_POOL_ELEMENTS; i++) {
		int t;

		hit += atomic_read(&pool->cache_acc[i].hit_alloc);
		total += atomic_read(&pool->cache_acc[i].total_alloc);

		t = atomic_read(&pool->cache_acc[i].total_alloc) -
			atomic_read(&pool->cache_acc[i].hit_alloc);
		allocated += t * (1 << i);
		merged += atomic_read(&pool->cache_acc[i].merged);
	}

	seq_printf(seq, "\n%-30s %-11d %-11d %-11d %d/%d/%d\n", pool->name,
		hit, total, (allocated != 0) ? merged*100/allocated : 0,
		pool->cached_pages, pool->inactive_cached_pages,
		pool->cached_entries);

	for (i = 0; i < SGV_POOL_ELEMENTS; i++) {
		int t = atomic_read(&pool->cache_acc[i].total_alloc) -
			atomic_read(&pool->cache_acc[i].hit_alloc);
		allocated = t * (1 << i);
		merged = atomic_read(&pool->cache_acc[i].merged);

		seq_printf(seq, "  %-28s %-11d %-11d %d\n",
			pool->cache_names[i],
			atomic_read(&pool->cache_acc[i].hit_alloc),
			atomic_read(&pool->cache_acc[i].total_alloc),
			(allocated != 0) ? merged*100/allocated : 0);
	}

	allocated = atomic_read(&pool->big_pages);
	merged = atomic_read(&pool->big_merged);
	oa = atomic_read(&pool->other_pages);
	om = atomic_read(&pool->other_merged);

	seq_printf(seq, "  %-40s %d/%-9d %d/%d\n", "big/other",
		atomic_read(&pool->big_alloc), atomic_read(&pool->other_alloc),
		(allocated != 0) ? merged*100/allocated : 0,
		(oa != 0) ? om/oa : 0);

	return;
}

int sgv_procinfo_show(struct seq_file *seq, void *v)
{
	struct sgv_pool *pool;
	int inactive_pages = 0;

	TRACE_ENTRY();

	spin_lock_bh(&sgv_pools_lock);
	list_for_each_entry(pool, &sgv_active_pools_list,
			sgv_active_pools_list_entry) {
		inactive_pages += pool->inactive_cached_pages;
	}
	spin_unlock_bh(&sgv_pools_lock);

	seq_printf(seq, "%-42s %d/%d\n%-42s %d/%d\n%-42s %d/%d\n\n",
		"Inactive/active pages", inactive_pages,
		atomic_read(&sgv_pages_total) - inactive_pages,
		"Hi/lo watermarks [pages]", sgv_hi_wmk, sgv_lo_wmk,
		"Hi watermark releases/failures",
		atomic_read(&sgv_releases_on_hiwmk),
		atomic_read(&sgv_releases_on_hiwmk_failed));

	seq_printf(seq, "%-30s %-11s %-11s %-11s %-11s", "Name", "Hit", "Total",
		"% merged", "Cached (P/I/O)");

	mutex_lock(&sgv_pools_mutex);
	list_for_each_entry(pool, &sgv_pools_list, sgv_pools_list_entry) {
		sgv_do_proc_read(seq, pool);
	}
	mutex_unlock(&sgv_pools_mutex);

	seq_printf(seq, "\n%-42s %-11d\n", "other",
		atomic_read(&sgv_other_total_alloc));

	TRACE_EXIT();
	return 0;
}
