#ifndef P2P_MEM_H_
#define P2P_MEM_H_

#include <linux/types.h>

struct p2p_mem_pages;

int p2p_mem_get_pages(u64 addr, u64 size,
		      void (*invalidate)(void *data), void *data,
		      struct p2p_mem_pages **pages_out);
void p2p_mem_put_pages(struct p2p_mem_pages *pages);
u64 p2p_mem_page_size(const struct p2p_mem_pages *pages);
u64 p2p_mem_page_count(const struct p2p_mem_pages *pages);
u64 p2p_mem_page_pa(const struct p2p_mem_pages *pages, u64 index);

int p2p_mem_init(void);
void p2p_mem_exit(void);

#endif
