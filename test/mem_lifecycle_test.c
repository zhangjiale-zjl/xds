// SPDX-License-Identifier: GPL-2.0
/* Exercise the production mem.c with deterministic userspace HAL/kernel mocks. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef uint64_t u64;
#define GFP_KERNEL 0
#define HZ 1
#define WQ_UNBOUND 1
#define WQ_MEM_RECLAIM 2
#define THIS_MODULE NULL
#define P2P_GET_PAGE_VERSION 1
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define check_mul_overflow(a, b, out) __builtin_mul_overflow(a, b, out)
#define pr_err(...) fprintf(stderr, __VA_ARGS__)
#define pr_err_ratelimited(...) pr_err(__VA_ARGS__)

struct mutex { pthread_mutex_t native; };
struct completion {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool done;
};
struct work_struct { void (*fn)(struct work_struct *); };
struct delayed_work { struct work_struct work; };
struct workqueue_struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t thread;
	struct delayed_work *pending;
	bool enabled;
	bool stopping;
};
struct p2p_page_info { u64 pa; };
struct p2p_page_table {
	u64 version;
	u64 page_size;
	u64 page_num;
	struct p2p_page_info *pages_info;
};
struct mock_table {
	struct p2p_page_table table;
	void (*callback)(void *);
	void *data;
};

static atomic_int symbols, module_refs, put_calls, freed, put_failures, invalidations;
static bool fail_workqueue, fail_pa_alloc, invalidate_during_get, bad_table;
static bool fail_get, callback_on_put;
static atomic_bool hold_module_put, exit_done;
static struct completion module_put_entered, allow_module_put_return;
static struct completion exit_entered, destroy_entered;
static struct mock_table *last_table;

static void mutex_init(struct mutex *lock)
{
	assert(!pthread_mutex_init(&lock->native, NULL));
}

static void mutex_lock(struct mutex *lock)
{
	assert(!pthread_mutex_lock(&lock->native));
}

static void mutex_unlock(struct mutex *lock)
{
	assert(!pthread_mutex_unlock(&lock->native));
}

static void init_completion(struct completion *done)
{
	assert(!pthread_mutex_init(&done->lock, NULL));
	assert(!pthread_cond_init(&done->cond, NULL));
	done->done = false;
}

static void complete_all(struct completion *done)
{
	assert(!pthread_mutex_lock(&done->lock));
	done->done = true;
	assert(!pthread_cond_broadcast(&done->cond));
	assert(!pthread_mutex_unlock(&done->lock));
}

static void wait_for_completion(struct completion *done)
{
	assert(!pthread_mutex_lock(&done->lock));
	while (!done->done)
		assert(!pthread_cond_wait(&done->cond, &done->lock));
	assert(!pthread_mutex_unlock(&done->lock));
}

static void *kzalloc(size_t size, int flags)
{
	(void)flags;
	return calloc(1, size);
}

static void *kvmalloc_array(size_t count, size_t size, int flags)
{
	(void)flags;
	if (fail_pa_alloc || (size && count > SIZE_MAX / size))
		return NULL;
	return calloc(count, size);
}

#define kfree free
#define kvfree free
#define is_power_of_2(n) ((n) && !((n) & ((n) - 1)))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
#define INIT_DELAYED_WORK(p, cb) ((p)->work.fn = (cb))
#define symbol_get(sym) (atomic_fetch_add(&symbols, 1), &(sym))
#define symbol_put(sym) ((void)atomic_fetch_sub(&symbols, 1))
#define __module_get(mod) ((void)atomic_fetch_add(&module_refs, 1))

static void module_put(void *mod)
{
	(void)mod;
	assert(atomic_fetch_sub(&module_refs, 1) > 0);
	if (atomic_load(&hold_module_put)) {
		complete_all(&module_put_entered);
		wait_for_completion(&allow_module_put_return);
	}
}

static void *workqueue_thread(void *arg)
{
	struct workqueue_struct *wq = arg;
	struct work_struct *work;

	assert(!pthread_mutex_lock(&wq->lock));
	for (;;) {
		while ((!wq->pending || !wq->enabled) && !wq->stopping)
			assert(!pthread_cond_wait(&wq->cond, &wq->lock));
		if (!wq->pending && wq->stopping)
			break;
		work = &wq->pending->work;
		wq->pending = NULL;
		assert(!pthread_mutex_unlock(&wq->lock));
		work->fn(work);
		/* The callback may free its embedded work; only touch wq now. */
		assert(!pthread_mutex_lock(&wq->lock));
	}
	assert(!pthread_mutex_unlock(&wq->lock));
	return NULL;
}

static struct workqueue_struct *alloc_workqueue(const char *name, int flags, int max_active)
{
	struct workqueue_struct *wq;

	(void)name;
	(void)flags;
	(void)max_active;
	if (fail_workqueue)
		return NULL;
	wq = calloc(1, sizeof(*wq));
	assert(wq);
	assert(!pthread_mutex_init(&wq->lock, NULL));
	assert(!pthread_cond_init(&wq->cond, NULL));
	wq->enabled = true;
	assert(!pthread_create(&wq->thread, NULL, workqueue_thread, wq));
	return wq;
}

static bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *work, int delay)
{
	(void)delay;
	assert(!pthread_mutex_lock(&wq->lock));
	assert(!wq->pending);
	wq->pending = work;
	assert(!pthread_cond_broadcast(&wq->cond));
	assert(!pthread_mutex_unlock(&wq->lock));
	return true;
}

static void enable_workqueue(struct workqueue_struct *wq, bool enabled)
{
	assert(!pthread_mutex_lock(&wq->lock));
	wq->enabled = enabled;
	assert(!pthread_cond_broadcast(&wq->cond));
	assert(!pthread_mutex_unlock(&wq->lock));
}

static void destroy_workqueue(struct workqueue_struct *wq)
{
	complete_all(&destroy_entered);
	assert(!pthread_mutex_lock(&wq->lock));
	wq->stopping = true;
	assert(!pthread_cond_broadcast(&wq->cond));
	assert(!pthread_mutex_unlock(&wq->lock));
	assert(!pthread_join(wq->thread, NULL));
	assert(!pthread_cond_destroy(&wq->cond));
	assert(!pthread_mutex_destroy(&wq->lock));
	free(wq);
}

static int hal_kernel_p2p_get_pages(u64 addr, u64 size, void (*callback)(void *),
				  void *data, struct p2p_page_table **out)
{
	struct mock_table *table;

	(void)addr;
	if (fail_get)
		return -ENOMEM;
	table = calloc(1, sizeof(*table));
	assert(table);
	table->table.version = bad_table ? 99 : P2P_GET_PAGE_VERSION;
	table->table.page_size = 4096;
	table->table.page_num = size / 4096;
	table->table.pages_info = calloc(size / 4096, sizeof(struct p2p_page_info));
	assert(table->table.pages_info);
	table->table.pages_info[0].pa = 0x100000;
	table->callback = callback;
	table->data = data;
	last_table = table;
	*out = &table->table;
	if (invalidate_during_get)
		callback(data);
	return 0;
}

static int hal_kernel_p2p_put_pages(struct p2p_page_table *page_table)
{
	struct mock_table *table = container_of(page_table, struct mock_table, table);

	atomic_fetch_add(&put_calls, 1);
	if (atomic_load(&put_failures) > 0) {
		atomic_fetch_sub(&put_failures, 1);
		return -EAGAIN;
	}
	if (callback_on_put)
		table->callback(table->data);
	free(table->table.pages_info);
	free(table);
	atomic_fetch_add(&freed, 1);
	return 0;
}

#include "../mem.c"

struct owner {
	struct completion callback_started;
	struct completion io_done;
};

static void invalidate_owner(void *arg)
{
	struct owner *owner = arg;

	atomic_fetch_add(&invalidations, 1);
	complete_all(&owner->callback_started);
	wait_for_completion(&owner->io_done);
}

static void *run_callback(void *arg)
{
	struct mock_table *table = arg;

	table->callback(table->data);
	return NULL;
}

static void *run_put(void *arg)
{
	p2p_mem_put_pages(arg);
	return NULL;
}

static void *run_exit(void *arg)
{
	(void)arg;
	complete_all(&exit_entered);
	p2p_mem_exit();
	atomic_store(&exit_done, true);
	return NULL;
}

static void test_prepare(struct owner *owner, int expected)
{
	struct p2p_mem_pages *pages = NULL;

	assert(p2p_mem_get_pages(0, 4096, invalidate_owner, owner, &pages) == expected);
	assert(!pages);
	assert(!atomic_load(&invalidations));
	assert(atomic_load(&freed) == !fail_get);
	p2p_mem_exit();
}

static void test_active(struct owner *owner)
{
	struct p2p_mem_pages *pages = NULL;
	pthread_t callback_thread, put_thread;

	assert(!p2p_mem_get_pages(0, 4096, invalidate_owner, owner, &pages));
	assert(p2p_mem_page_pa(pages, 0) == 0x100000);
	assert(!pthread_create(&callback_thread, NULL, run_callback, last_table));
	wait_for_completion(&owner->callback_started);
	assert(!pthread_create(&put_thread, NULL, run_put, pages));
	assert(!atomic_load(&freed));
	complete_all(&owner->io_done);
	assert(!pthread_join(callback_thread, NULL));
	assert(!pthread_join(put_thread, NULL));
	assert(atomic_load(&invalidations) == 1);
	assert(atomic_load(&freed) == 1);
	p2p_mem_exit();
}

static void test_retry(struct owner *owner)
{
	struct p2p_mem_pages *pages = NULL;
	pthread_t exit_thread;

	assert(!p2p_mem_get_pages(0, 4096, invalidate_owner, owner, &pages));
	enable_workqueue(put_retry_wq, false);
	atomic_store(&put_failures, 2);
	atomic_store(&hold_module_put, true);
	p2p_mem_put_pages(pages);
	assert(atomic_load(&module_refs) == 1);
	assert(!atomic_load(&freed));
	/* Owner has detached; a late callback must not wait for owner->io_done. */
	last_table->callback(last_table->data);
	assert(!atomic_load(&invalidations));
	enable_workqueue(put_retry_wq, true);
	wait_for_completion(&module_put_entered);
	assert(atomic_load(&put_calls) == 3);
	assert(atomic_load(&freed) == 1);
	assert(!atomic_load(&module_refs));
	assert(!pthread_create(&exit_thread, NULL, run_exit, NULL));
	wait_for_completion(&exit_entered);
	wait_for_completion(&destroy_entered);
	assert(!atomic_load(&exit_done));
	assert(atomic_load(&symbols) == 2);
	complete_all(&allow_module_put_return);
	assert(!pthread_join(exit_thread, NULL));
	assert(atomic_load(&exit_done));
}

static void test_normal(struct owner *owner)
{
	struct p2p_mem_pages *pages = NULL;

	assert(!p2p_mem_get_pages(0, 4096, invalidate_owner, owner, &pages));
	assert(p2p_mem_page_size(pages) == 4096);
	assert(p2p_mem_page_count(pages) == 1);
	callback_on_put = true;
	p2p_mem_put_pages(pages);
	assert(atomic_load(&put_calls) == 1);
	assert(atomic_load(&freed) == 1);
	assert(!atomic_load(&invalidations));
	p2p_mem_exit();
}

int main(int argc, char **argv)
{
	struct owner owner;

	alarm(15);
	assert(argc == 2);
	init_completion(&owner.callback_started);
	init_completion(&owner.io_done);
	init_completion(&module_put_entered);
	init_completion(&allow_module_put_return);
	init_completion(&exit_entered);
	init_completion(&destroy_entered);
	fail_workqueue = !strcmp(argv[1], "init-failure");
	assert(p2p_mem_init() == (fail_workqueue ? -ENOMEM : 0));
	if (fail_workqueue)
		goto checked;
	invalidate_during_get = !strcmp(argv[1], "prepare-invalidation");
	bad_table = !strcmp(argv[1], "invalid-table");
	fail_pa_alloc = !strcmp(argv[1], "copy-allocation-failure");
	fail_get = !strcmp(argv[1], "hal-get-failure");
	if (invalidate_during_get || bad_table || fail_pa_alloc || fail_get)
		test_prepare(&owner, invalidate_during_get ? -ESTALE : bad_table ? -EINVAL : -ENOMEM);
	else if (!strcmp(argv[1], "active-invalidation"))
		test_active(&owner);
	else if (!strcmp(argv[1], "retry-unload"))
		test_retry(&owner);
	else {
		assert(!strcmp(argv[1], "normal"));
		test_normal(&owner);
	}
checked:
	assert(!atomic_load(&symbols));
	assert(!atomic_load(&module_refs));
	printf("PASS %s\n", argv[1]);
	return 0;
}
