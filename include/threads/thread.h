#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "filesys/file.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* States in a thread's life cycle. */
enum thread_status
{
	THREAD_RUNNING, /* Running thread. */
	THREAD_READY,	/* Not running but ready to run. */
	THREAD_BLOCKED, /* Waiting for an event to trigger. */
	THREAD_DYING	/* About to be destroyed. */
};

struct file_fd
{
	int fd;					  /* fd: 파일 식별자 */
	struct file *file;		  /* file */
	struct list_elem fd_elem; /* list 구조체의 구성원 */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t)-1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0	   /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63	   /* Highest priority. */

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 * 왜 grow downward?
 * => 중간에 있는 공간을 share 할 수 있게 초기 설계를 했기 때문에 관습적으로 이어짐
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread
{
	/* Owned by thread.c. */
	tid_t tid;				   /* Thread identifier. */
	enum thread_status status; /* Thread state. */
	char name[16];			   /* Name (for debugging purposes). */
	int priority;			   /* Priority. */

	int64_t wakeup_tick; /* 깨어나야할 시간을 저장해주는 변수 */
	/* Shared between thread.c and synch.c. */
	struct list_elem elem; /* List element. */
	// run queue의 원소로 사용되거나 semaphore wait의 원소로 사용됨
	// 동시에 두 가지 기능을 할 수 있는 이유는 두 기능이 Mutually exclusive이기 때문이다.
	// run queue에 들어가려면 ready state이어야 하고
	// semaphore wait lsit에 들어가려면 block state 이어야 한다.
	// 스레드가 동시에 두 가지 state를 가질 수 없으므로 elem을 통해 두 가지 작업을 수행해도 문제가 생기지 않는다.

	int init_priority;				/* 자기 자신의 초기 priority 저장*/
	struct lock *wait_on_lock;		/* 획득하고자 하는 lock의 주소를 저장*/
	struct list_elem donation_elem; /* donate할때 확장할 priority의 자료형 */
	struct list donations;			/* donate받은 priority 저장할 list */
	struct list fd_list;			/* fd를 저장할 리스트 */
	int fd_count;					/* fd를 확인하기 위한 count*/
	int exit_status;
	struct semaphore fork_sema; /* 자식 프로세스의 fork가 완료될 때까지 기다리도록 하기 위한 세마포어*/
	struct semaphore wait_sema;
	struct semaphore exit_sema;

	struct list child_list;		 /* 자식 스레드를 보관하는 리스트 */
	struct list_elem child_elem; /* 자식 리스트 element */
	struct file *now_file;

	bool check_child;

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4; /* Page map level 4 */
	struct file *running_executable; /* file struct address of executable file for this process*/

#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
	uint64_t rsp;
	uint64_t stack_btm;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf; /* Information for switching */
	unsigned magic;		  /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

void do_iret(struct intr_frame *tf);

/* 실행 중인 스레드를 슬립으로 만듬 */
void thread_sleep(int64_t ticks);

/* 슬립큐에서 깨워야할 스레드를 깨움 */
void thread_awake(int64_t ticks);

/* 인자로 주어진 스레드들의 우선순위를 비교 */
bool cmp_priority(const struct list_elem *a,
				  const struct list_elem *b, void *aux UNUSED);
bool cmp_dona_priority(const struct list_elem *a_,
					   const struct list_elem *b_, void *aux UNUSED);
void test_max_priority(void);
void donate_priority(void);
void remove_with_lock(struct lock *lock);
void refresh_priority(void);
#endif /* threads/thread.h */
