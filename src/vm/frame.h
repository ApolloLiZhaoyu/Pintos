#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdbool.h>
#include <list.h>
#include <hash.h>
#include "../threads/palloc.h"

struct frame_item {
    void* frame;
    void* upage;
    struct thread* t;
    bool swapable;
    struct hash_elem hash_elem;
    struct list_elem list_elem;
};

void frame_init(void);
void* frame_get(enum palloc_flags flag, void *upage);
void frame_free(void *frame);
bool frame_set_unswapable(void* frame);

#endif /* vm/frame.h */
