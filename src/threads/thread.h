#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
typedef int mapid_t;
typedef int off_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */
#define NICE_MIN -20                    /* Lowest nice. */
#define NICE_DEFAULT 0                  /* Default nice. */
#define NICE_MAX 20                     /* Highest nice. */

struct thread;

struct mmap_handler{
    mapid_t mapid;
    struct file* mmap_file; 
    void* mmap_addr; 
    int num_page; 
    int last_page_size; 
    struct list_elem elem;
    bool writable;
    bool is_segment; 
    bool is_static_data; 
    int num_page_with_segment;
    off_t file_ofs;
};

struct child_info
{
  struct thread *child_thread;
  tid_t child_id;
  bool exited;
  bool terminated;
  bool load_failed;
  int ret_value;
  struct semaphore *sema_start;
  struct semaphore *sema_finish;
  struct list_elem elem;
  struct list_elem allelem;
};
struct file_info{
  int fd;
  struct file* opened_file;
  struct dir* opened_dir;
  struct thread* thread_num;

  struct list_elem elem;
};


/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    int priority;                       /* Priority. */
    struct list_elem allelem;           /* List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */

    /* My attempt and owned by thread.c. */
    int64_t wakeup_time;                /* Time to wake up after sleep. */
    int recent_cpu;                     /* Recent_CPU for priority. */
    int nice;                           /* Nice for priority. */

    int old_priority;                   /* Old priority. */
    struct list locks;                  /* Locks tat the thread is holding. */
    struct lock *lock_waiting;          /* The lock that the thread is waiting for. */

    int return_value;
    struct list child_list;
    struct semaphore sema_start;
    struct semaphore sema_finish;
    bool parent_die;
    struct child_info *message_to_parent;
#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
    struct file* exec_file;
#endif

#ifdef VM
    struct hash* page_table;
    void* esp;
    struct list mmap_file_list;
    mapid_t next_mapid;
#endif

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
    struct dir* cwd;
  };

struct file_handle{
    int fd;
    struct file* opened_file;
    struct thread* owned_thread;
    struct list_elem elem;
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_sleep (int64_t ticks);
void thread_wakeup (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);
void thread_cond_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void increase_recent_cpu (void);
void update_priority (struct thread *t, void *aux UNUSED);
void update_priority_for_each (void);

void update_load_avg (void);
void update_recent_cpu (struct thread *t, void *aux UNUSED);
void update_recent_cpu_for_each (void);

void thread_hold_the_lock (struct lock *lock);
void thread_donate_priority (struct thread *t);
void thread_remove_lock (struct lock *lock);

struct file_info* get_file_info(int fd);
struct child_info* get_child_info(tid_t tid);
void add_file_list(struct file_info *info);

#endif /* threads/thread.h */
