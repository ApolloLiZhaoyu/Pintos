#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "filesys/file.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <threads/vaddr.h>
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"

typedef int pid_t;

void syscall_init (void);
void close_file(struct file *);
void exit_status(struct intr_frame *f, int status);
bool check_translate_user(const char *vaddr, bool write);

#ifdef VM
bool mmap_check_mmap_vaddr(struct thread *cur, const void *vaddr, int num_page);
bool mmap_install_page(struct thread *cur, struct mmap_handler *mh);
void mmap_read_file(struct mmap_handler* mh, void *upage, void *kpage);
void mmap_write_file(struct mmap_handler* mh, void *upage, void *kpage);
bool mmap_load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes, uint32_t zero_bytes, bool writable);
#endif


#endif /* userprog/syscall.h */
