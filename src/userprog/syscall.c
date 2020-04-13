#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <threads/vaddr.h>
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "process.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "pagedir.h"
#ifdef VM
#include "vm/page.h"
#endif
#ifdef FILESYS
#include "filesys/inode.h"
#endif

static void syscall_handler (struct intr_frame *);

static void sys_halt(struct intr_frame *f);
static void sys_exit(struct intr_frame *f, int status);
static void sys_exec(struct intr_frame *f, const char *cmd_line);
static void sys_wait(struct intr_frame *f, pid_t pid);
static void sys_create(struct intr_frame *f, const char *name, unsigned initial_size);
static void sys_remove(struct intr_frame *f, const char *file);
static void sys_open(struct intr_frame *f, const char *file);
static void sys_filesize(struct intr_frame *f, int fd);
static void sys_read(struct intr_frame *f, int fd, const void *buffer, unsigned size);
static void sys_write(struct intr_frame *f, int fd, const void *buffer, unsigned size);
static void sys_seek(struct intr_frame *f, int fd, unsigned position);
static void sys_tell(struct intr_frame *f, int fd);
static void sys_close(struct intr_frame *f, int fd);

static void syscall_mmap(struct intr_frame *f, int fd, const void *obj_vaddr);
static void syscall_munmap(struct intr_frame *f, mapid_t mapid);

static void sys_chdir(struct intr_frame *f, const char *name);
static void sys_mkdir(struct intr_frame *f, const char *name);
static void sys_readdir(struct intr_frame *f, int fd, char *name);
static void sys_isdir(struct intr_frame *f, int fd);
static void sys_inumber(struct intr_frame *f, int fd);

static struct lock filesys_lock;

void
syscall_init (void)  {
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

void
exit_status(struct intr_frame *f, int status) {
  struct thread *cur = thread_current();
  cur->return_value = status;
  f->eax = status;
  thread_exit();
}

bool
check_translate_user(const char *vaddr, bool write) {
  if(vaddr == NULL || !is_user_vaddr(vaddr))
    return false;
#ifdef VM
  struct page_table_elem* base = page_find_lock(thread_current()->page_table, pg_round_down(vaddr));
  if (base == NULL) return page_fault_handler(vaddr, write, thread_current()->esp);
  else return !(write && !(base->writable));
#else
  return pagedir_get_page(thread_current() -> pagedir, vaddr) != NULL;
#endif
}

bool
check_user(const char *vaddr, int size, bool write) {
  if(!check_translate_user(vaddr + size - 1, write))
    return false;
  size >>= 12;
  do {
    if(!check_translate_user(vaddr, write))
      return false;
    vaddr += (1<<12);
  } while(size--);
  return true;
}



static void
syscall_handler (struct intr_frame *f /*UNUSED*/)  {

#ifdef VM
  thread_current ()->esp = f->esp;
#endif

  if(!check_user(f->esp, 4, false))
    exit_status(f, -1);
  int syscall_num = *((int*)f->esp);
  void *arg1 = f->esp + 4, *arg2 = f->esp + 8, *arg3 = f->esp + 12;

  switch (syscall_num) {
    case SYS_EXIT: case SYS_EXEC: case SYS_WAIT: case SYS_TELL:  case SYS_REMOVE: case SYS_FILESIZE: case SYS_OPEN: case SYS_CLOSE:
#ifdef VM
    case SYS_MUNMAP:
#endif
#ifdef FILESYS
    case SYS_MKDIR: case SYS_CHDIR: case SYS_ISDIR: case SYS_INUMBER:
#endif
      if(!check_user(arg1, 4, false))
        exit_status(f, -1);
      break;
    case SYS_SEEK: case SYS_CREATE:
#ifdef VM
    case SYS_MMAP:
#endif
#ifdef FILESYS
    case SYS_READDIR:
#endif
      if(!check_user(arg1, 8, false))
        exit_status(f, -1);
      break;
    case SYS_READ: case SYS_WRITE:
      if(!check_user(arg1, 12, false))
        exit_status(f, -1);
      break;
    default:;
  }
  switch (syscall_num) {
    case SYS_HALT:
      sys_halt(f); break;
    case SYS_EXIT:
      sys_exit(f, *((int *)arg1)); break;
    case SYS_EXEC:
      sys_exec(f, *((void **) arg1)); break;
    case SYS_WAIT:
      sys_wait(f, *((int *)arg1)); break;
    case SYS_CREATE:
      sys_create(f, *((void **) arg1), *((unsigned *) arg2)); break;
    case SYS_REMOVE:
      sys_remove(f, *((void **) arg1)); break;
    case SYS_OPEN:
      sys_open(f, *((void **) arg1)); break;
    case SYS_FILESIZE:
      sys_filesize(f, *((int *)arg1)); break;
    case SYS_READ:
      sys_read(f, *((int *)arg1), *((void **) arg2), *((unsigned *) arg3)); break;
    case SYS_WRITE:
      sys_write(f, *((int *)arg1), *((void **) arg2), *((unsigned *) arg3)); break;
    case SYS_SEEK:
      sys_seek(f, *((int *)arg1), *((unsigned *) arg2)); break;
    case SYS_TELL:
      sys_tell(f, *((int *)arg1)); break;
    case SYS_CLOSE:
      sys_close(f, *((int *)arg1)); break;
#ifdef VM
    case SYS_MUNMAP:
      syscall_munmap(f, *((mapid_t *) arg1)); break;
    case SYS_MMAP:
      syscall_mmap(f, *((int *) arg1), *((void **) arg2)); break;
#endif
#ifdef FILESYS
    case SYS_CHDIR:
      sys_chdir(f, *((void **) arg1)); break;
    case SYS_MKDIR:
      sys_mkdir(f, *((void **) arg1)); break;
    case SYS_READDIR:
      sys_readdir(f, *((int *)arg1), *((void **) arg2)); break;
    case SYS_ISDIR:
      sys_isdir(f, *((int *)arg1));
    case SYS_INUMBER:
      sys_inumber(f, *((int *)arg1));
#endif
  }

}

static void
sys_halt(struct intr_frame *f) {
  shutdown_power_off();
}

static void
sys_wait(struct intr_frame *f, pid_t pid) {
//  printf("in wait");
  f->eax = (uint32_t)process_wait(pid);
}

static void
sys_exit(struct intr_frame *f, int status) {
  struct thread *cur = thread_current();
  if (!cur->parent_die) {
    cur->message_to_parent->exited = true;
    cur->message_to_parent->ret_value = status;
  }
  exit_status(f, status);
}

static void
sys_write(struct intr_frame *f, int fd, const void *buffer, unsigned size) {
  if (!check_user(buffer, size, false)) {
//    printf("wrong right.");
    exit_status(f, -1);
  }
  if (fd == STDIN_FILENO) {
//    printf("wrong std.");
    exit_status(f, -1);
  }
  else if (fd == STDOUT_FILENO) {
//    printf("stdout!!!");
    putbuf(buffer, size);
  } else {
    struct file_info *info = get_file_info(fd);
    if(info != NULL && info->opened_dir == NULL) {
      lock_acquire(&filesys_lock);
      f->eax = (uint32_t)file_write(info->opened_file, buffer, size);
      lock_release(&filesys_lock);
    } else {
//      printf("not open");
      exit_status(f, -1);
    }
  }
}

static void
sys_read(struct intr_frame *f, int fd, const void *buffer, unsigned size) {
  if(!check_user(buffer, size, true))
    exit_status(f, -1);
  if(fd == STDOUT_FILENO)
    exit_status(f, -1);
  uint8_t *str = buffer;
  if(fd == STDIN_FILENO) {
    while(size--) {
      *(char *)str++ = input_getc();
    }
  } else {
    struct file_info *info = get_file_info(fd);
    if(info != NULL) {
      lock_acquire(&filesys_lock);
      f->eax = (uint32_t)file_read(info->opened_file, (void *)buffer, size);
      lock_release(&filesys_lock);
    } else {
      exit_status(f, -1);
    }
  }
}

void close_file(struct file *file1) {
  lock_acquire(&filesys_lock);
  file_close(file1);
  lock_release(&filesys_lock);
}

bool
check_string(const char *str) {
  if(!check_translate_user(str, false))
    return false;
  int cnt = 0;
  while(*(str + cnt) != 0) {
    if(cnt == 4095)
      return false;
    cnt++;
    if(((int)(str + cnt) & PGMASK) == 0) {
      if (!check_translate_user(str + cnt, false))
        return false;
    }
  }
  return true;
}

static void
sys_exec(struct intr_frame *f, const char *cmd_line) {
  if(!check_string(cmd_line)) {
    exit_status(f, -1);
  }
  lock_acquire(&filesys_lock);
  f->eax = (uint32_t)process_execute(cmd_line);
  lock_release(&filesys_lock);
  struct list_elem *e;
  struct thread *cur = thread_current();
  struct child_info *l;
  for(e = list_begin(&cur->child_list); e != list_end(&cur->child_list); e = list_next(e)) {
    l = list_entry(e, struct child_info, elem);
    if(l->child_id == f->eax) {
      sema_down(l->sema_start);
      if(l->load_failed)
        f->eax = (uint32_t)-1;
      return;
    }
  }
}

static void
sys_open(struct intr_frame *f, const char *name) {
  if(!check_string(name))
    exit_status(f, -1);
  lock_acquire(&filesys_lock);
  struct file *tmp = filesys_open(name);
  lock_release(&filesys_lock);
  if(tmp == NULL) {
    f->eax = (uint32_t)-1;
    return ;
  }
  static uint32_t fd_next = 2;
  struct file_info *info = malloc(sizeof(struct file_info));
  info->opened_file = tmp;
  info->thread_num = thread_current();
  info->fd = fd_next++;
  lock_acquire(&filesys_lock);
  struct inode *inode = file_get_inode(info->opened_file);
  if(inode != NULL && inode_is_dir(inode)) {
    info->opened_dir = dir_open( inode_reopen(inode) );
  }
  else
    info->opened_dir = NULL;

  lock_release(&filesys_lock);
  add_file_list(info);
  f->eax = (uint32_t)info->fd;
}

static void
sys_create(struct intr_frame *f, const char *name, unsigned initial_size) {
  if(!check_string(name))
    exit_status(f, -1);
  lock_acquire(&filesys_lock);
  f->eax = (uint32_t)filesys_create(name, initial_size, false);
  lock_release(&filesys_lock);
}

static void
sys_remove(struct intr_frame *f, const char *name) {
  if(!check_string(name))
    exit_status(f, -1);
  lock_acquire(&filesys_lock);
  f->eax = (uint32_t)filesys_remove(name);
  lock_release(&filesys_lock);
}

static void
sys_filesize(struct intr_frame *f, int fd) {
  struct file_info *info = get_file_info(fd);
  if(info != NULL) {
    lock_acquire(&filesys_lock);
    f->eax = (uint32_t)file_length(info->opened_file);
    lock_release(&filesys_lock);
  } else {
    exit_status(f, -1);
  }
}

static void
sys_close(struct intr_frame *f, int fd) {
  struct file_info *info = get_file_info(fd);
  if(info != NULL) {
    lock_acquire(&filesys_lock);
    file_close(info->opened_file);
    if(info->opened_dir != NULL)
      dir_close(info->opened_dir);
    lock_release(&filesys_lock);
    list_remove(&info->elem);
    free(info);
  } else {
    exit_status(f, -1);
  }
}

static void
sys_tell(struct intr_frame *f, int fd) {
  struct file_info *info = get_file_info(fd);
  if(info != NULL) {//--------------------need change after filesys finished -------
    lock_acquire(&filesys_lock);
    f->eax = (uint32_t)file_tell(info->opened_file);
    lock_release(&filesys_lock);
  } else {
    exit_status(f, -1);
  }
}

static void
sys_seek(struct intr_frame *f, int fd, unsigned position) {
  struct file_info *info = get_file_info(fd);
  if(info != NULL) {
    lock_acquire(&filesys_lock);
    file_seek(info->opened_file, position);
    lock_release(&filesys_lock);
  } else {
    exit_status(f, -1);
  }
}

#ifdef VM
bool mmap_check_mmap_vaddr(struct thread *cur, const void *vaddr, int num_page) {
    bool res = true;
    for (int i = 0; i < num_page; i++)
	if (!page_upage_accessable(cur->page_table, i * PGSIZE + vaddr))
	    res = false;
    return res;
}

bool mmap_install_page(struct thread *cur, struct mmap_handler *mh) {
    bool res = true;
    for (int i = 0; i < mh->num_page; i++)
	if (!page_install_file(cur->page_table, mh, mh->mmap_addr + i * PGSIZE))
	    res = false;
    if (mh->is_segment)
	for (int i = mh->num_page; i < mh->num_page_with_segment; i++)
	    if (!page_install_file(cur->page_table, mh, mh->mmap_addr + i * PGSIZE))
		res = false;
    return res;
}

void mmap_read_file(struct mmap_handler* mh, void *upage, void *kpage) {
    if (mh->is_segment) {
	void* addr = mh->mmap_addr + mh->num_page * PGSIZE + mh->last_page_size;
	if (mh->last_page_size != 0)
	    addr -= PGSIZE;
	if (addr > upage) {
	    if (addr - upage < PGSIZE) {
		file_read_at(mh->mmap_file, kpage, mh->last_page_size, upage - mh->mmap_addr + mh->file_ofs);
		memset(kpage + mh->last_page_size, 0, PGSIZE - mh->last_page_size);
	    } else file_read_at(mh->mmap_file, kpage, PGSIZE, upage - mh->mmap_addr + mh->file_ofs);
	} else memset(kpage, 0, PGSIZE);
    } else {
	if (mh->mmap_addr + file_length(mh->mmap_file) - upage < PGSIZE) {
	    file_read_at(mh->mmap_file, kpage, mh->last_page_size, upage - mh->mmap_addr + mh->file_ofs);
	    memset(kpage + mh->last_page_size, 0, PGSIZE - mh->last_page_size);
	} else file_read_at(mh->mmap_file, kpage, PGSIZE, upage - mh->mmap_addr + mh->file_ofs);
    }
}

void mmap_write_file(struct mmap_handler* mh, void* upage, void *kpage) {
    if (mh->writable) {
	if (mh->is_segment) {
	    void* addr = mh->mmap_addr + mh->num_page * PGSIZE + mh->last_page_size;
	    if (addr > upage) {
		if (addr - upage < PGSIZE) file_write_at(mh->mmap_file, kpage, mh->last_page_size, upage - mh->mmap_addr + mh->file_ofs);
		else file_write_at(mh->mmap_file, kpage, PGSIZE, upage - mh->mmap_addr + mh->file_ofs);
	    }
	} else {
	    if (mh->mmap_addr + file_length(mh->mmap_file) - upage < PGSIZE)
		file_write_at(mh->mmap_file, kpage, mh->last_page_size, upage - mh->mmap_addr + mh->file_ofs);
	    else
		file_write_at(mh->mmap_file, kpage, PGSIZE, upage - mh->mmap_addr + mh->file_ofs);
	}
    }
}

bool mmap_load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
    ASSERT(!((read_bytes + zero_bytes) & PGMASK)) struct thread* cur = thread_current();
    mapid_t mapid = cur->next_mapid++;
    struct mmap_handler* mh = malloc(sizeof(struct mmap_handler));
    mh->mapid = mapid;
    mh->mmap_file = file;
    mh->writable = writable;
    mh->is_static_data = writable;
    int num_page = read_bytes / PGSIZE;
    int total_num_page = ((read_bytes + zero_bytes) / PGSIZE);
    int last_page_used = read_bytes & PGMASK;
    if (last_page_used != 0) num_page++;
    if (!mmap_check_mmap_vaddr(cur, upage, total_num_page)) return false;
    mh->mmap_addr = upage;
    mh->num_page = num_page;
    mh->last_page_size = last_page_used;
    mh->num_page_with_segment = total_num_page;
    mh->is_segment = true;
    mh ->file_ofs = ofs;
    list_push_back(&(cur->mmap_file_list), &(mh->elem));
    return mmap_install_page(cur, mh);
}


static void syscall_mmap(struct intr_frame* f, int fd, const void* obj_vaddr) {
    if (fd == 0 || fd == 1) {
	f->eax = -1;
	return;
    }
    if (obj_vaddr == NULL || ((uint32_t) obj_vaddr % (uint32_t)PGSIZE != 0)) {
	f->eax = -1;
	return;
    }
    struct thread* cur = thread_current();
    struct file_info* fh = get_file_info(fd);
    if (fh != NULL) {
	mapid_t mapid = cur->next_mapid++;
	struct mmap_handler *mh = malloc(sizeof(struct mmap_handler));
	mh->mapid = mapid;
	mh->mmap_file = file_reopen(fh->opened_file);
	mh->writable = true;
	mh->is_segment = false;
	mh->is_static_data = false;
	mh->file_ofs = 0;
	off_t file_size = file_length(mh->mmap_file);
	int num_page = file_size / PGSIZE;
	int last_page_used = file_size % PGSIZE;
	if (last_page_used != 0) num_page++;
	if (!mmap_check_mmap_vaddr(cur, obj_vaddr, num_page)) {
	    f->eax = -1;
	    return;
	}
	mh->mmap_addr = obj_vaddr;
	mh->num_page = num_page;
	mh->num_page_with_segment = num_page;
	mh->last_page_size = last_page_used;
	list_push_back(&(cur->mmap_file_list), &(mh->elem));
	if(!mmap_install_page(cur, mh)) {
	    f->eax = -1;
	    return;
	}
	f->eax = (uint32_t) mapid;
    } else {
	f->eax = -1;
	return;
    }
}

static void syscall_munmap(struct intr_frame *f, mapid_t mapid) {
    struct thread* cur = thread_current();
    if (list_empty(&cur->mmap_file_list)) {
	f->eax = -1;
	return;
    }
    struct mmap_handler* mh = syscall_get_mmap_handle(mapid);
    if (mh == NULL) {
	f->eax = -1;
	return;
    }
    for (int i = 0; i < mh->num_page; i++) {
	if (!page_unmap(cur->page_table, mh->mmap_addr + i * PGSIZE)) {
	    delete_mmap_handle(mh);
	    f->eax = -1;
	    return;
	}
    }
    if (!delete_mmap_handle(mh)) {
	f->eax = -1;
	return;
    }
}

#endif
#ifdef FILESYS

static void
sys_chdir(struct intr_frame *f, const char *name)
{
  if(!check_string(name))
    exit_status(f, -1);
//  bool return_code;
//  check_user((const uint8_t*) filename);

  lock_acquire (&filesys_lock);
  f->eax = filesys_chdir(name);
  lock_release (&filesys_lock);

//  return return_code;
}

static void
sys_mkdir(struct intr_frame *f, const char *name)
{
  if(!check_string(name))
    exit_status(f, -1);
//  bool return_code;
//  check_user((const uint8_t*) filename);

  lock_acquire (&filesys_lock);
  f->eax = filesys_create(name, 0, true);
  lock_release (&filesys_lock);

//  return return_code;
}

static void
sys_readdir(struct intr_frame *f, int fd, char *name)
{
  if(!check_string(name))
    exit_status(f, -1);

//  struct file_desc* file_d;
//  bool ret = false;
  f->eax = 0;

  lock_acquire (&filesys_lock);
  //file_d = find_file_desc(thread_current(), fd, FD_DIRECTORY);
  struct file_info *info = get_file_info(fd);
  if (info == NULL) goto done;
  if (info->opened_dir == NULL)
    goto done;
//  if (file_d == NULL) goto done;

  struct inode *inode;
  inode = file_get_inode(info->opened_file); // file descriptor -> inode
  if(inode == NULL) goto done;

  // check whether it is a valid directory
  if(! inode_is_dir(inode)) goto done;

//  ASSERT (file_d->dir != NULL); // see sys_open()
  f->eax = dir_readdir (info->opened_dir, name);

  done:
  lock_release (&filesys_lock);
//  return ret;
}

static void
sys_isdir(struct intr_frame *f, int fd)
{
  lock_acquire (&filesys_lock);

  struct file_info *info = get_file_info(fd);
  if (info == NULL) {
    f->eax = 0;
    return ;
  }
  f->eax = inode_is_dir (file_get_inode(info->opened_file));

  lock_release (&filesys_lock);
//  return ret;
}

static void
sys_inumber(struct intr_frame *f, int fd)
{
  lock_acquire (&filesys_lock);

//  struct file_desc* file_d = find_file_desc(thread_current(), fd, FD_FILE | FD_DIRECTORY);
  struct file_info *info = get_file_info(fd);
  if (info == NULL) {
    f->eax = 0;
    return ;
  }
  f->eax = (int) inode_get_inumber (file_get_inode(info->opened_file));

  lock_release (&filesys_lock);
//  return ret;
}

#endif
