#include <debug.h>
#include <threads/pte.h>
#include <threads/malloc.h>
#include <hash.h>
#include "swap.h"

const int BLOCK_PER_PAGE = PGSIZE / BLOCK_SECTOR_SIZE;

static struct list swap_free_list;
struct block* swap_block;
index_t top_index = 0;

void swap_init(){
    swap_block = block_get_role(BLOCK_SWAP);
    ASSERT(swap_block != NULL);
    list_init(&swap_free_list);
}

index_t swap_store(void* kpage){
    ASSERT(is_kernel_vaddr(kpage));
    index_t index = (index_t)-1;
    if (list_empty(&swap_free_list)){
	if (top_index + BLOCK_PER_PAGE < block_size(swap_block)){
	    index = top_index;
	    top_index += BLOCK_PER_PAGE;
	}
    } else {
	struct swap_item* t = list_entry(list_front(&swap_free_list), struct swap_item, list_elem);
	list_remove(&t->list_elem);
	index = t->index;
	free(t);
    }
    if (index == (index_t)-1) return index;
    for (int i = 0; i < BLOCK_PER_PAGE; i++)
	block_write(swap_block, index + i, kpage + i * BLOCK_SECTOR_SIZE);
    return index;
}

void swap_free(index_t index){
    ASSERT(index % BLOCK_PER_PAGE == 0);
    if (top_index == index + BLOCK_PER_PAGE) top_index = index;
    else {
	struct swap_item* t = malloc(sizeof(struct swap_item));
	t->index = index;
	list_push_back(&swap_free_list, &t->list_elem);
    }
}

void swap_load(index_t index, void* kpage){
    ASSERT(index != (index_t)-1);
    ASSERT(is_kernel_vaddr(kpage));
    ASSERT(index % BLOCK_PER_PAGE == 0);
    for (int i = 0; i < BLOCK_PER_PAGE; i++)
	block_read(swap_block, index + i, kpage + i * BLOCK_SECTOR_SIZE);
    swap_free(index);
}


