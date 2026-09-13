// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "p2p: " fmt

#include <linux/errno.h>
#include <linux/completion.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "mem_abi.h"
#include "mem.h"

#define P2P_HAL_MIN_PAGE_SIZE (4U << 10)

struct p2p_mem_pages {
	struct mutex lock;
	struct completion callback_done;
	struct p2p_page_table *page_table;
	u64 *pa_list;
	u64 page_size;
	u64 page_num;
	void (*invalidate)(void *data);
	void *invalidate_data;
	bool callback_running;
	bool revoked;
};

static typeof(hal_kernel_p2p_get_pages) *get_pages;
static typeof(hal_kernel_p2p_put_pages) *put_pages;

static void p2p_mem_free_callback(void *data)
{
	struct p2p_mem_pages *pages = data;
	void (*invalidate)(void *data);
	void *invalidate_data;

	mutex_lock(&pages->lock);
	pages->revoked = true;
	pages->callback_running = true;
	pages->page_table = NULL;
	invalidate = pages->invalidate;
	invalidate_data = pages->invalidate_data;
	mutex_unlock(&pages->lock);

	if (invalidate)
		invalidate(invalidate_data);

	complete(&pages->callback_done);
}

int p2p_mem_init(void)
{
	get_pages = symbol_get(hal_kernel_p2p_get_pages);
	if (!get_pages) {
		pr_err("required symbol hal_kernel_p2p_get_pages is unavailable\n");
		return -ENODEV;
	}

	put_pages = symbol_get(hal_kernel_p2p_put_pages);
	if (!put_pages) {
		pr_err("required symbol hal_kernel_p2p_put_pages is unavailable\n");
		symbol_put(hal_kernel_p2p_get_pages);
		return -ENODEV;
	}

	return 0;
}

int p2p_mem_get_pages(u64 addr, u64 size,
		      void (*invalidate)(void *data), void *data,
		      struct p2p_mem_pages **pages_out)
{
	struct p2p_mem_pages *pages;
	struct p2p_page_table *page_table;
	u64 expected_size;
	u64 i;
	int err;

	if (!size || !pages_out ||
	    ((addr | size) & (P2P_HAL_MIN_PAGE_SIZE - 1)))
		return -EINVAL;

	pages = kzalloc(sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;
	mutex_init(&pages->lock);
	init_completion(&pages->callback_done);
	pages->invalidate = invalidate;
	pages->invalidate_data = data;

	err = get_pages(addr, size, p2p_mem_free_callback, pages,
			&pages->page_table);
	if (err)
		goto free_pages;

	mutex_lock(&pages->lock);
	if (pages->revoked) {
		err = -ESTALE;
		goto unlock;
	}
	page_table = pages->page_table;
	if (!page_table || page_table->version != P2P_GET_PAGE_VERSION ||
	    page_table->page_size < P2P_HAL_MIN_PAGE_SIZE ||
	    !is_power_of_2(page_table->page_size) ||
	    !page_table->page_num || !page_table->pages_info ||
	    check_mul_overflow(page_table->page_size, page_table->page_num,
			       &expected_size) || expected_size != size) {
		err = -EINVAL;
		goto unlock;
	}

	pages->pa_list = kvmalloc_array(page_table->page_num,
					 sizeof(*pages->pa_list), GFP_KERNEL);
	if (!pages->pa_list) {
		err = -ENOMEM;
		goto unlock;
	}
	for (i = 0; i < page_table->page_num; i++)
		pages->pa_list[i] = page_table->pages_info[i].pa;
	pages->page_size = page_table->page_size;
	pages->page_num = page_table->page_num;
	mutex_unlock(&pages->lock);

	*pages_out = pages;
	return 0;

unlock:
	page_table = pages->page_table;
	pages->page_table = NULL;
	mutex_unlock(&pages->lock);
	if (page_table)
		put_pages(page_table);
free_pages:
	if (pages->callback_running)
		wait_for_completion(&pages->callback_done);
	kvfree(pages->pa_list);
	kfree(pages);
	return err;
}

void p2p_mem_put_pages(struct p2p_mem_pages *pages)
{
	struct p2p_page_table *page_table;
	bool callback_running;
	int err;

	if (!pages)
		return;

	mutex_lock(&pages->lock);
	page_table = pages->page_table;
	pages->page_table = NULL;
	callback_running = pages->callback_running;
	mutex_unlock(&pages->lock);

	if (page_table) {
		err = put_pages(page_table);
		if (err)
			pr_err("hal_kernel_p2p_put_pages failed: %d\n", err);
	} else if (callback_running) {
		wait_for_completion(&pages->callback_done);
	}

	kvfree(pages->pa_list);
	kfree(pages);
}

u64 p2p_mem_page_size(const struct p2p_mem_pages *pages)
{
	return pages->page_size;
}

u64 p2p_mem_page_count(const struct p2p_mem_pages *pages)
{
	return pages->page_num;
}

u64 p2p_mem_page_pa(const struct p2p_mem_pages *pages, u64 index)
{
	if (index >= pages->page_num)
		return 0;
	return pages->pa_list[index];
}

void p2p_mem_exit(void)
{
	symbol_put(hal_kernel_p2p_put_pages);
	symbol_put(hal_kernel_p2p_get_pages);
}
