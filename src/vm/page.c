#include <stdio.h>
#include <debug.h>
#include <stddef.h>
#include <hash.h>
#include "page.h"
#include "frame.h"
#include "swap.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"

#define PAGE_PAL_FLAG			0
#define PAGE_INST_MARGIN		32
#define PAGE_STACK_SIZE			0x800000
#define PAGE_STACK_UNDERLINE	(PHYS_BASE - PAGE_STACK_SIZE)

static struct lock page_lock;

bool page_hash_less(const struct hash_elem* lhs, const struct hash_elem* rhs, void *aux UNUSED) {
    return hash_entry(lhs, struct page_table_elem, elem)->key < hash_entry(rhs, struct page_table_elem, elem)->key;
}

unsigned page_hash(const struct hash_elem* e, void* aux UNUSED){
    struct page_table_elem *t = hash_entry(e, struct page_table_elem, elem);
    return hash_bytes(&(t->key), sizeof(t->key));
}

struct page_table_elem* page_find(struct hash* page_table, void* upage) {
    struct hash_elem *e;
    struct page_table_elem tmp;
    ASSERT(page_table != NULL);
    tmp.key = upage;
    e = hash_find(page_table, &(tmp.elem));
    if(e != NULL) return hash_entry(e, struct page_table_elem, elem);
    else return NULL;
}

bool page_upage_accessable(struct hash *page_table, void* upage) {
    return upage < PAGE_STACK_UNDERLINE && page_find(page_table, upage) == NULL;
}

bool page_status_exp(struct thread *cur, void* upage, void* index, bool to_swap) {
    struct page_table_elem* t = page_find(cur->page_table, upage);
    bool success = true;
    if(t != NULL && t->status == FRAME) {
	if(to_swap) {
	    t->value = index;
	    t->status = SWAP;
	} else {
	    ASSERT(t->origin != NULL);
	    t->value = t->origin;
	    t->status = FILE;
	}
	pagedir_clear_page(cur->pagedir, upage);
    } else {
	if (t != NULL) printf("%s\n", t->status == FILE ? "flie" : "swap");
	else puts("NULL");
	success = false;
    }
    return success;
}

bool page_install_file(struct hash *page_table, struct mmap_handler *mh, void *key) {
    bool success = true;
    lock_acquire(&page_lock);
    if(page_upage_accessable(page_table, key)) {
	struct page_table_elem *e = malloc(sizeof(*e));
	e->key = key;
	e->value = mh;
	e->status = FILE;
	e->writable = mh->writable;
	e->origin = mh;
	hash_insert(page_table, &e->elem);
    } else success = false;
    lock_release(&page_lock);
    return success;
}

void page_init() {
    lock_init(&page_lock);
}

void page_destroy_std(struct hash_elem* e, void* aux UNUSED) {
    struct page_table_elem* t = hash_entry(e, struct page_table_elem, elem);
    if(t->status == FRAME) {
	struct thread* cur = thread_current();
	pagedir_clear_page(cur->pagedir, t->key);
	frame_free(t->value);
    } else if(t->status == SWAP) swap_free((off_t) t->value);
    free(t);
}

void page_destroy(struct hash* page_table) {
    lock_acquire(&page_lock);
    hash_destroy(page_table, page_destroy_std);
    lock_release(&page_lock);
}

bool page_fault_handler(const void* vaddr, bool to_write, void *esp) {
    struct thread *cur = thread_current();
    struct hash *page_table = cur->page_table;
    uint32_t *pagedir = cur->pagedir;
    void* upage = pg_round_down(vaddr);
    bool success = true;
    lock_acquire(&page_lock);
    struct page_table_elem *t = page_find(page_table, upage);
    void *dest = NULL;
    ASSERT(is_user_vaddr(vaddr));
    ASSERT(!(t != NULL && t->status == FRAME));
    if(to_write == true && t != NULL && t->writable == false) return false;
    if(upage >= PAGE_STACK_UNDERLINE) {
	if(vaddr >= esp - PAGE_INST_MARGIN) {
	    if(t == NULL) {
		dest = frame_get(PAGE_PAL_FLAG, upage);
		if(dest == NULL) {
		    success = false;
		} else {
		    t = malloc(sizeof(*t));
		    t->key = upage;
		    t->value = dest;
		    t->status = FRAME;
		    t->writable = true;
		    t->origin = NULL;
		    hash_insert(page_table, &t->elem);
		}
	    } else {
		switch(t->status) {
		    case SWAP:
			dest = frame_get(PAGE_PAL_FLAG, upage);
			if(dest == NULL) {
			    success = false;
			    break;
			}
			swap_load((index_t) t->value, dest);
			t->value = dest;
			t->status = FRAME;
			break;
		    default:
			success = false;
		}
	    }
	} else success = false;
    } else {
	if(t == NULL) success = false;
	else {
	    switch(t->status) {
		case SWAP:
		    dest = frame_get(PAGE_PAL_FLAG, upage);
		    if(dest == NULL) {
			success = false;
			break;
		    }
		    swap_load((index_t)t->value, dest);
		    t->value = dest;
		    t->status = FRAME;
		    break;
		case FILE:
		    dest = frame_get(PAGE_PAL_FLAG, upage);
		    if(dest == NULL) {
			success = false;
			break;
		    }
		    mmap_read_file(t->value, upage, dest);
		    t->value = dest;
		    t->status = FRAME;
		    break;
		default:
		    success = false;
	    }
	}
    }
    frame_set_unswapable(dest);
    lock_release(&page_lock);
    if(success) ASSERT(pagedir_set_page(pagedir, t->key, t->value, t->writable));
    return success;
}

bool page_set_frame(void* upage, void* kpage, bool wb) {
    struct thread* cur = thread_current();
    struct hash* page_table = cur->page_table;
    uint32_t *pagedir = cur->pagedir;
    bool success = true;
    lock_acquire(&page_lock);
    struct page_table_elem *t = page_find(page_table, upage);
    if(t == NULL) {
	t = malloc(sizeof(struct page_table_elem));
	t->key = upage;
	t->value = kpage;
	t->status = FRAME;
	t->origin = NULL;
	t->writable = wb;
	hash_insert(page_table, &t->elem);
    } else success = false;
    lock_release(&page_lock);
    if(success) ASSERT(pagedir_set_page(pagedir, t->key, t->value, t->writable));
    return success;
}

bool page_accessible_upage(struct hash* page_table, void* upage) {
    return upage < PAGE_STACK_UNDERLINE && page_find(page_table, upage) != NULL;
}

bool page_unmap(struct hash* page_table, void* upage) {
    struct thread *cur = thread_current();
    bool success = true;
    lock_acquire(&page_lock);
    if(page_accessible_upage(page_table, upage)) {
	struct page_table_elem *t = page_find(page_table, upage);
	ASSERT( t != NULL );
	switch(t->status) {
	    case FILE:
		hash_delete(page_table, &(t->elem));
		free(t);
		break;
	    case FRAME:
		if(pagedir_is_dirty(cur->pagedir, t->key)) {
		    mmap_write_file(t->origin, t->key, t->value);
		}
		pagedir_clear_page(cur->pagedir, t->key);
		hash_delete(page_table, &(t->elem));
		frame_free(t->value);
		free(t);
		break;
	    default:
		success = false;
	}

    } else success = false;
    lock_release(&page_lock);
    return success;
}

struct hash* page_create(void) {
    struct hash* t = malloc(sizeof(struct hash));
    if(t != NULL) {
	if (!hash_init(t, page_hash, page_hash_less, NULL)) {
	    free(t);
	    return NULL;
	} else return t;
    } else return NULL;
}

struct page_table_elem* page_find_lock(struct hash* page_table, void* upage) {
    lock_acquire(&page_lock);
    struct page_table_elem* tmp = page_find(page_table, upage);
    lock_release(&page_lock);
    return tmp;
}
