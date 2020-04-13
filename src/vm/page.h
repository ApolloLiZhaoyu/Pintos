#ifndef SUPPLEMENTAL_PAGE_TABLE_MODULE
#define SUPPLEMENTAL_PAGE_TABLE_MODULE

#include <stdint.h>
#include <hash.h>
#include "threads/thread.h"

enum page_status {
	FRAME,
	SWAP,
	FILE
};

struct page_table_elem {
	void* key;
	void* value;
	void* origin;
	enum page_status status;
	bool writable;
	struct hash_elem elem;    
};

struct page_table_elem* page_find(struct hash* page_table, void* upage);
bool page_status_exp(struct thread* cur, void* upage, void* index, bool to_swap);
bool page_install_file(struct hash* page_table, struct mmap_handler* mh, void* key);
bool page_upage_accessable(struct hash* page_table, void* upage);
void page_init(void);
void page_destroy(struct hash* page_table);
bool page_fault_handler(const void* vaddr, bool to_write, void* esp);
bool page_set_frame(void* upage, void* kpage, bool wb);
bool page_unmap(struct hash* page_table, void* upage);
struct hash* page_create(void);
struct page_table_elem* page_find_lock(struct hash* page_table, void* upage);

#endif

