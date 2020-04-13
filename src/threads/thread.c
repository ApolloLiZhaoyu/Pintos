#include "thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/fixed_point.h"
#ifdef USERPROG
#include "userprog/process.h"
#include "userprog/syscall.h"
#endif
#ifdef VM
#include "vm/page.h"
#endif
#ifdef FILESYS
#include "filesys/directory.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of processes in THREAD_BLOCK state. */
static struct list sleep_list;


/* List of file in this thread */
static struct list file_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* List of all children. */
static struct list child_list;


/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */
static int load_avg;            /* # of load_avg in all threads. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* For Priority queue of threads. */
static bool wakeup_time_less (const struct list_elem *lhs, const struct list_elem *rhs, void *aux UNUSED);
static bool thread_priority_more (const struct list_elem *lhs, const struct list_elem *rhs, void *aux UNUSED);
static bool lock_priority_more (const struct list_elem *lhs, const struct list_elem *rhs, void *aux UNUSED);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&sleep_list);
  list_init (&file_list);
  list_init (&all_list);
  list_init (&child_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

  load_avg = 0;
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();

  /* Check the block threads whether to wake up. */
  thread_wakeup();  
}

/* Make the thread sleep. */
void
thread_sleep (int64_t ticks)
{
  struct thread* current_thread = thread_current();
  ASSERT(current_thread ->status == THREAD_RUNNING);
  current_thread ->wakeup_time = timer_ticks() + ticks;

  enum intr_level old_level = intr_disable();
  list_insert_ordered (&sleep_list, &current_thread->elem, wakeup_time_less, NULL);
  thread_block ();
  intr_set_level(old_level);
}

/* Check the block thread. */
void
thread_wakeup (void)
{  
  if (list_empty (&sleep_list)) return;
  struct list_elem *cur = list_begin (&sleep_list);
  struct list_elem *next;
  
  while (cur != list_end (&sleep_list))
    {
      next = list_next (cur);
      struct thread *t = list_entry (cur, struct thread, elem);
      if (t->wakeup_time > timer_ticks()) break;
      t->wakeup_time = 0;

      enum intr_level old_level = intr_disable();
      list_remove (cur);
      thread_unblock (t);
      intr_set_level (old_level);
      
      cur = next;
    }
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

struct child_info* get_child_info(tid_t tid) {
  struct list_elem *e;
  struct child_info *ret;
  for(e = list_begin(&child_list); e != list_end(&child_list); e = list_next(e)) {
    ret = list_entry(e, struct child_info, allelem);
    if(ret->child_id == tid)
      return ret;
  }
  return NULL;
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
//  printf("before create have %d thread in ready_list\n", list_size(&ready_list));
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  struct child_info *info = palloc_get_page(PAL_ZERO);
  info->child_id = tid;
  info->child_thread = t;
  info->exited = false;
  info->terminated = false;
  info->load_failed = false;
  info->sema_start = &t->sema_start;
  info->sema_finish = &t->sema_finish;
  info->ret_value = 0;
  list_push_back(&child_list, &info->allelem);
  t->message_to_parent = info;

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

#ifdef FILESYS
  if(thread_current()->cwd)
    t->cwd = dir_reopen(thread_current()->cwd);
  else
    t->cwd = NULL;
#endif

  /* Add to run queue. */
  thread_unblock (t);

  if (thread_mlfqs)
  {
    update_recent_cpu (t, NULL);
    update_priority (t, NULL);
    update_recent_cpu (thread_current(), NULL);
    update_priority (thread_current(), NULL);
  }

  /*if(t->priority > thread_current ()->priority){
    thread_yield ();
  }*/
  thread_cond_yield ();

//  printf("current have %d thread in ready_list\n", list_size(&ready_list));

  return tid;
}


/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered (&ready_list, &t->elem, thread_priority_more, NULL);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit();

  sema_up(&thread_current()->sema_finish);

  struct thread *cur = thread_current();
  /* Close all the file current open and belong to this thread. */



  if (!list_empty(&file_list)) {
    struct list_elem *e;
    for (e = list_begin(&file_list); e != list_end(&file_list); e = list_next(e)) {
      struct file_info *fd;
      fd = list_entry(e, struct file_info, elem);
      if (fd->thread_num == cur) {
        close_file(fd->opened_file);
        e = list_prev(e);
        list_remove(&(fd->elem));
        free(fd);
      }
    }
  }
  if(cur->exec_file != NULL) {
    file_allow_write(cur->exec_file);
    close_file(cur->exec_file);
  }
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_insert_ordered (&ready_list, &cur->elem, thread_priority_more, NULL);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}
void
thread_cond_yield (void)
{
  if (thread_current () != idle_thread &&
      !list_empty (&ready_list) &&
      thread_current ()->priority < list_entry (list_begin (&ready_list), struct thread, elem)->priority)

    thread_yield ();

}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  if (new_priority > PRI_MAX || new_priority < PRI_MIN) return;
  if (thread_mlfqs) return;
  enum intr_level old_level = intr_disable();
  struct thread *t = thread_current();
  int old_priority = t->priority;
  t->old_priority = new_priority;
  if (list_empty (&t->locks) || new_priority > old_priority)
  {
    t->priority = new_priority;
    thread_yield ();
  }
  intr_set_level (old_level);
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Update the priority of the thread. */
void
update_priority (struct thread *t, void *aux UNUSED)
{
  if (t == idle_thread) return;
  t->priority = PRI_MAX - CONVERT_TO_INT_ROUND (DIV_INT (t->recent_cpu, 4)) - t->nice * 2;
  t->priority = t->priority < PRI_MIN ? PRI_MIN : t->priority;
  t->priority = t->priority > PRI_MAX ? PRI_MAX : t->priority;
}

/* Update the priority of thread for each. */
void
update_priority_for_each (void)
{
  thread_foreach (update_priority, NULL);
  list_sort (&ready_list, thread_priority_more, NULL);
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  if(nice > NICE_MAX || nice < NICE_MIN) return;
  struct thread *t = thread_current();
  if (t == idle_thread) return;
  t->nice = nice;
  update_priority(t, NULL);
  if (t->status == THREAD_READY)
    list_sort (&ready_list, thread_priority_more, NULL);
  else if (t->status == THREAD_RUNNING)
  {
    /*int max_priority = list_entry (list_begin (&ready_list), struct thread, elem)->priority;
    if (max_priority > t->priority) thread_yield ();*/
    thread_cond_yield ();
  }
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current ()-> nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return CONVERT_TO_INT_ROUND (MULT_INT (load_avg, 100));
}

/* Update the load_avg. */
void
update_load_avg (void)
{
  ASSERT(thread_mlfqs);

  int ready_threads = (thread_current () != idle_thread) ? list_size (&ready_list) + 1 : list_size (&ready_list);
  load_avg = MULT (DIV_INT (CONVERT_TO_FP (59), 60), load_avg) + MULT_INT (DIV_INT (CONVERT_TO_FP (1), 60), ready_threads);
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return CONVERT_TO_INT_ROUND (MULT_INT (thread_current ()->recent_cpu, 100));
}

/* Update the recent_cpu of thread for each. */
void
update_recent_cpu_for_each (void)
{
  ASSERT(thread_mlfqs);

  thread_foreach (update_recent_cpu, NULL);
}

/* Update the recent_cpu of thread. */
void
update_recent_cpu (struct thread *t, void *aux UNUSED)
{
  if (t == idle_thread) return;
  int coefficient = DIV (MULT_INT (load_avg, 2), ADD_INT (MULT_INT (load_avg, 2), 1));
  t->recent_cpu = ADD_INT (MULT (coefficient, t->recent_cpu), t->nice);
}

/* Increase the recent_cpu of the current thread by one. */
void
increase_recent_cpu (void)
{
  ASSERT(thread_mlfqs);

  struct thread *t = thread_current ();
  if (t == idle_thread) return;
  if (t->status == THREAD_RUNNING)
  {
    t->recent_cpu = ADD_INT (t->recent_cpu, 1);
  }
}

/* Let the thread hold the lock. */
void
thread_hold_the_lock (struct lock *lock)
{
  list_insert_ordered (&thread_current ()->locks, &lock->elem, lock_priority_more, NULL);
}

/* Let the current thread donate its priority to thread t. */
void 
thread_donate_priority (struct thread *t)
{
  enum intr_level old_level = intr_disable();
  t->priority = thread_current ()->priority;
  if (t->status == THREAD_READY)
  {
    list_remove (&t->elem);
    list_insert_ordered (&ready_list, &t->elem, thread_priority_more, NULL);
  } 
  else if (t->status == THREAD_RUNNING)
  {
    thread_cond_yield ();
  }

  intr_set_level (old_level);
}

/* Remove the lock from the current thread. */
void
thread_remove_lock (struct lock *lock)
{
  enum intr_level old_level = intr_disable ();
  list_remove (&lock->elem);
  struct thread *cur = thread_current ();
  if (list_empty (&cur->locks))
  {
    cur->priority = cur->old_priority;
  } 
  else
  {
    int lock_priority = list_entry (list_front (&cur->locks), struct lock, elem)->max_priority;
    if(lock_priority > cur->old_priority)
      cur->priority = lock_priority;
  }
  intr_set_level (old_level);
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;
  t->wakeup_time = 0;
  if (t == initial_thread)
  {
    t->nice = 0;
    t->recent_cpu = 0;
  } else
  {
    t->nice = thread_get_nice ();
    t->recent_cpu = thread_get_recent_cpu ();
  }
  /*t->nice = 0;
  t->recent_cpu = 0;*/

  t->old_priority = priority;
  list_init (&t->locks);
  t->lock_waiting = NULL;

  t->return_value = 0;
  t->parent_die = false;
  list_init(&t->child_list);
  sema_init(&t->sema_start, 0);
  sema_init(&t->sema_finish, 0);

  t->cwd = NULL;

#ifdef VM
  list_init(&t->mmap_file_list);
  t->next_mapid = 1;
#endif

  old_level = intr_disable ();
  list_insert_ordered (&all_list, &t->allelem, thread_priority_more, NULL);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void)
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

static bool
wakeup_time_less (const struct list_elem *lhs, const struct list_elem *rhs, void *aux UNUSED)
{
  struct thread *a, *b;
  
  ASSERT (lhs != NULL && rhs != NULL);
  
  a = list_entry (lhs, struct thread, elem);
  b = list_entry (rhs, struct thread, elem);
  
  return (a->wakeup_time < b->wakeup_time);
}

static bool
thread_priority_more (const struct list_elem *lhs, const struct list_elem *rhs, void *aux UNUSED)
{
  struct thread *a, *b;
  
  ASSERT (lhs != NULL && rhs != NULL);
  
  a = list_entry (lhs, struct thread, elem);
  b = list_entry (rhs, struct thread, elem);
  
  return (a->priority > b->priority);
}

static bool
lock_priority_more (const struct list_elem *lhs, const struct list_elem *rhs, void *aux UNUSED)
{
  struct lock *a, *b;
  
  ASSERT (lhs != NULL && rhs != NULL);
  
  a = list_entry (lhs, struct lock, elem);
  b = list_entry (rhs, struct lock, elem);
  
  return (a->max_priority > b->max_priority);
}

struct file_info* get_file_info (int fd) {
  struct thread *cur = thread_current();
  struct list_elem *i;
  for (i = list_begin(&file_list); i != list_end(&file_list); i = list_next(i)) {
    struct file_info *l;
    l = list_entry(i, struct file_info, elem);
    if (l->fd == fd) {
      if (l->thread_num == cur)
        return l;
      return NULL;
    }
  }
  return NULL;
}

void
add_file_list (struct file_info *info) {
  list_push_back(&file_list, &(info->elem));
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

