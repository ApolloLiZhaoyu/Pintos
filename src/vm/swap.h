#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <kernel/list.h>
#include <devices/block.h>

typedef block_sector_t index_t;

struct swap_item {
    index_t index;
    struct list_elem list_elem;
};

void swap_init(void);
index_t swap_store(void* kpage);
void swap_free(index_t index);
void swap_load(index_t index, void* kpage);

#endif
