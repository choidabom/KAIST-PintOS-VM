#include "userprog/process.h"
#include "userprog/syscall.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "intrinsic.h"
#include "lib/string.h"
#include "lib/stdio.h"
#ifdef VM
#include "vm/vm.h"
#endif

#define DELIM_CHARS " "
static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(struct fork_data *aux);
struct thread *get_child(int tid);
struct fork_data
{
	struct thread *parent;
	struct intr_frame *user_level_f;
};
/* General process initializer for initd and other process. */
static void
process_init(void)
{
	struct thread *current = thread_current();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char *file_name)
{
	char *fn_copy;
	tid_t tid;
	char *process_name;
	char *next_ptr;
	/* 커맨드 라인의 첫 번째 토큰을 thread_create() 함수의 첫 인자로 전달되도록 프로그램을 수정
		현재는 커맨드 라인 전체가 thread_create()에 전달되고 있음 */
	// process_name = strtok_r(file_name, DELIM_CHARS, &next_ptr);

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	/*fn_copy => file_name을 fn_copy에 복사하기 위해 메모리 할당 */
	fn_copy = palloc_get_page(0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy(fn_copy, file_name, PGSIZE);

	char *save;
	strtok_r(file_name, " ", &save);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create(file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page(fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd(void *f_name)
{
#ifdef VM
	supplemental_page_table_init(&thread_current()->spt);
#endif

	process_init();
	if (process_exec(f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
/*
인자로 넣은 pid에 해당하는 자식 스레드의 구조체를 얻은 후 fork_sema 값이
1이 될 때(== 자식 스레드 load가 완료될 때)까지 기다렸다가 pid를 반환
*/
struct thread *get_child(int tid)
{
	struct thread *curr = thread_current();
	struct list_elem *start;
	struct list *child_list = &curr->child_list;
	for (start = list_begin(child_list); start != list_end(child_list); start = list_next(start))
	{
		struct thread *child_t = list_entry(start, struct thread, child_elem);
		if (child_t->tid == tid)
		{
			return child_t;
		}
	}
	return NULL;
}

/* 인터럽트 프레임은 인터럽트가 호출됐을 때 이전에 레지스터에 작업하던 context 정보를 스택에 담는 구조체이다.
 부모 프로세스가 갖고 있던 레지스터 정보를 담아 고대로 복사해야하기 때문이다.*/
tid_t process_fork(const char *name, struct intr_frame *if_)
{
	/* struct에 넣어야 할 정보
	1. 부모 프로세스
	2. 유저 레벨의 intr_frame
	3. __do_fork가 끝난 다음에 부모 프로세스가 실행 될 수 있게 하기 위한 lock */
	struct fork_data my_data;
	my_data.parent = thread_current();
	my_data.user_level_f = if_;

	// curr->tf = *if_; => 이렇게 하면 커널 레벨의 인터럽트 프레임에 유저 레벨의 인터럽트 정보를 넣기 때문에 "틀린" 과정을 도출하게 됨.
	/* curr->tf는 커널레벨의 인터럽트 프레임이기 때문에 context switch가 발생할 경우 값이 변경 되기 때문 */

	/* Clone current thread to new thread.*/
	tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, &my_data);

	if (tid == TID_ERROR)
	{
		return TID_ERROR;
	}
	// 자식의 스레드를 가져온다. get_child 사용.
	struct thread *child = get_child(tid);

	sema_down(&child->fork_sema);
	if (!thread_current()->check_child)
	{
		return -1;
	}
	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte(uint64_t *pte, void *va, void *aux)
{
	struct thread *current = thread_current();
	struct thread *parent = (struct thread *)aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately.
		만약 va가 kernel address라면 => true를 리턴  */
	if (is_kernel_vaddr(va))
	{
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4.
		부모 스레드 내 멤버인 pml4를 이용해 부모 페이지를 불러온다. */
	parent_page = pml4_get_page(parent->pml4, va);
	if (parent_page == NULL)
	{
		return false;
	}

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to NEWPAGE.
		새로운 PAL_USER 페이지를 할당하고 newpage에 저장한다. */
	newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL)
	{
		return false;
	}

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result).
	 * 	부모 페이지를 복사해 새로 할당받은 페이지에 넣어준다. 이 때 부모 페이지가 writable인지 아닌지 확인하기 위해 is_writable() 함수를 이용한다.  */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page(current->pml4, va, newpage, writable))
	{
		/* 6. TODO: if fail to insert page, do error handling.
			페이지 생성에 실패하면 에러 핸들링이 동작하도록 false를 리턴한다.  */
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork(struct fork_data *aux) // aux 인자로 struct fork_data 가 들어옴
{
	struct intr_frame if_;
	struct thread *parent = aux->parent;
	struct thread *current = thread_current();
	struct intr_frame *parent_if = aux->user_level_f;
	bool succ = true;

	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */

	/* 1. Read the cpu context to local stack. */
	memcpy(&if_, parent_if, sizeof(struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate(current); /* 해당 스레드의 페이지 테이블 활성화 (흠 .. 매핑 활성화하는 친구)*/
	current->now_file = file_duplicate(parent->now_file);

#ifdef VM
	supplemental_page_table_init(&current->spt);
	if (!supplemental_page_table_copy(&current->spt, &parent->spt))
		goto error;
#else
	/* pml4_for_each()를 활용해서 user memory space 전체와 그에 대응하는 pagetable 구조체를 복사한다.*/
	if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	struct list_elem *start;
	struct list *parent_list = &parent->fd_list;
	if (!list_empty(parent_list))
	{
		for (start = list_begin(parent_list); start != list_end(parent_list); start = list_next(start))
		{
			struct file_fd *parent_fd = list_entry(start, struct file_fd, fd_elem);
			if (parent_fd->file != NULL)
			{
				struct file_fd *child_fd = malloc(sizeof(struct file_fd));
				child_fd->file = file_duplicate(parent_fd->file);
				child_fd->fd = parent_fd->fd;
				list_push_back(&current->fd_list, &child_fd->fd_elem);
			}
			current->fd_count = parent->fd_count;
		}
		current->fd_count = parent->fd_count;
	}
	else
		current->fd_count = parent->fd_count;

	if_.R.rax = 0;
	process_init();
	sema_up(&current->fork_sema);
	/* Finally, switch to the newly created process. */
	/* do_iret을 통해서 interrupt frame에 있는 정보를 register로 보내준다.
	duplicate_pte와 file_duplicate 등을 통해서 interrupt frame의 모든 정보를 자식 process에 복사해주었기 때문이다. 그래서 그냥 register에 올려주기만 하면 된다.  */
	if (succ)
	{
		parent->check_child = true;
		do_iret(&if_);
	}
error:
	// sema_up(&current->fork_sema);
	parent->check_child = false;
	exit_handler(TID_ERROR);
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void *f_name)
{
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;
	/* We first kill the current context */
	process_cleanup();

	/* And then load the binary */
	success = load(f_name, &_if);

	// hex_dump(_if.rsp, _if.rsp, USER_STACK - _if.rsp, true);
	// 왜 void인데 char*인 file_name이 됨?
	palloc_free_page(file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret(&_if);
	NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int process_wait(tid_t child_tid)
{
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	struct thread *child = get_child(child_tid);
	if (child == NULL)
	{
		return -1;
	}
	sema_down(&child->wait_sema);
	int exit_status = child->exit_status;
	list_remove(&child->child_elem);
	sema_up(&child->exit_sema);
	return exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{
	struct thread *curr = thread_current();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	if (curr->pml4 != NULL)
	{
		printf("%s: exit(%d)\n", curr->name, curr->exit_status);
		file_close(curr->now_file);
		curr->now_file = NULL;
	}
	struct list *exit_list = &curr->fd_list;
	struct list_elem *start = list_begin(exit_list);
	while (!list_empty(exit_list))
	{
		struct file_fd *exit_fd = list_entry(start, struct file_fd, fd_elem);
		file_close(exit_fd->file);
		start = list_remove(&exit_fd->fd_elem);
		free(exit_fd);
	}

	process_cleanup();
	sema_up(&curr->wait_sema);
	sema_up(&curr->fork_sema);
	sema_down(&curr->exit_sema);
}

/* Free the current process's resources. */
static void
process_cleanup(void)
{
	struct thread *curr = thread_current();

#ifdef VM
	supplemental_page_table_kill(&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL)
	{
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate(NULL);
		pml4_destroy(pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next)
{
	/* Activate thread's page tables. */
	pml4_activate(next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0			/* Ignore. */
#define PT_LOAD 1			/* Loadable segment. */
#define PT_DYNAMIC 2		/* Dynamic linking info. */
#define PT_INTERP 3			/* Name of dynamic loader. */
#define PT_NOTE 4			/* Auxiliary info. */
#define PT_SHLIB 5			/* Reserved. */
#define PT_PHDR 6			/* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr
{
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage,
						 uint32_t read_bytes, uint32_t zero_bytes,
						 bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load(const char *file_name, struct intr_frame *if_)
{
	struct thread *t = thread_current();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;

	int i, j;
	char *next_ptr;
	char *process_name;
	char *token;
	uintptr_t stack_ptr;
	char *argv[LOADER_ARGS_LEN / 2 + 1];
	char *token_argv[LOADER_ARGS_LEN / 2 + 1];
	int argc = 0;

	/* 로드가 호출되기 전에 실행하고 있는 파일이 있으면, 파일을 close 해줘야함. */
	if(t->now_file){
		file_close(t->now_file);
		t->now_file = NULL;
	}

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create();
	if (t->pml4 == NULL)
		goto done;
	process_activate(thread_current());

	process_name = strtok_r(file_name, DELIM_CHARS, &next_ptr);
	token_argv[argc] = process_name;

	while (token != NULL)
	{
		// 인자(token) 분리해주는 과정 & argc 구하기
		argc++;
		token = strtok_r(NULL, DELIM_CHARS, &next_ptr);
		token_argv[argc] = token;
	}

	/* Open executable file. */
	file = filesys_open(process_name);
	if (file == NULL)
	{
		printf("load: %s: open failed\n", process_name);
		goto done;
	}

	/* Read and verify executable header. */
	if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr || memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 0x3E // amd64
		|| ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024)
	{
		printf("load: %s: error loading executable\n", process_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++)
	{
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length(file))
			goto done;
		file_seek(file, file_ofs);

		if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type)
		{
		case PT_NULL:
		case PT_NOTE:
		case PT_PHDR:
		case PT_STACK:
		default:
			/* Ignore this segment. */
			break;
		case PT_DYNAMIC:
		case PT_INTERP:
		case PT_SHLIB:
			goto done;
		case PT_LOAD:
			if (validate_segment(&phdr, file))
			{
				bool writable = (phdr.p_flags & PF_W) != 0;
				uint64_t file_page = phdr.p_offset & ~PGMASK;
				uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
				uint64_t page_offset = phdr.p_vaddr & PGMASK;
				uint32_t read_bytes, zero_bytes;
				if (phdr.p_filesz > 0)
				{
					/* Normal segment.
					 * Read initial part from disk and zero the rest. */
					read_bytes = page_offset + phdr.p_filesz;
					zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
				}
				else
				{
					/* Entirely zero.
					 * Don't read anything from disk. */
					read_bytes = 0;
					zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
				}
				if (!load_segment(file, file_page, (void *)mem_page,
								  read_bytes, zero_bytes, writable))
				{
					printf("load_segment 터짐\n");
					goto done;
				}
			}
			else
				goto done;
			break;
		}
	}

	/* Set up stack. */
	if (!setup_stack(if_))
	{
		printf("setup_stack에서 터짐\n");
		goto done;
	}

	/* Start address. */
	if_->rip = ehdr.e_entry;

	stack_ptr = if_->rsp;

	// process_name도 token_argv에 넣어서 스택에 넣어주기 위한 과정

	for (i = argc - 1; i > -1; i--)
	{
		stack_ptr -= (strlen(token_argv[i]) + 1);
		memcpy(stack_ptr, token_argv[i], strlen(token_argv[i]) + 1);
		argv[i] = stack_ptr;
	}

	// word-align - 8의 배수로 맞춘다(padding)
	// x86-64의 stack alignmet 규칙에 따르면 return address를 제외하고 직전까지의 상황에서 %rsprk 16
	if ((if_->rsp - stack_ptr) % 8)
	{
		int p_size = (8 - ((if_->rsp - stack_ptr) % 8));
		stack_ptr -= (8 - ((if_->rsp - stack_ptr) % 8));
		memset(stack_ptr, 0, p_size);
	}

	// argv의 마지막을 null로 한다 -> 인자을 끝을 알린다.
	stack_ptr -= 8;
	memset(stack_ptr, 0, 8);

	// 인자를 저장한 주소를 스택에 저장한다.
	stack_ptr -= (argc * (sizeof(argv[0]) / sizeof(char)));
	memcpy(stack_ptr, argv, argc * (sizeof(argv[0]) / sizeof(char)));

	// Push Fake Return Address
	stack_ptr -= 8;
	memset(stack_ptr, 0, 8);

	if_->rsp = stack_ptr;

	// rsi가 argv의 주소(argv[0]의 주소를 가리키게 하고, rdi를 argc로 설정합니다.
	if_->R.rsi = stack_ptr + 8;
	if_->R.rdi = argc;
	t->now_file = file; /* 현재 프로세스가 실행 중인 파일을 저장 */
	file_deny_write(file);
	success = true;
done:
	/* We arrive here whether the load is successful or not. */

	// file_close(file);
	return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment(const struct Phdr *phdr, struct file *file)
{
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t)file_length(file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr((void *)phdr->p_vaddr))
		return false;
	if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);

	file_seek(file, ofs);
	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page(PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes)
		{
			palloc_free_page(kpage);
			return false;
		}
		memset(kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page(upage, kpage, writable))
		{
			printf("fail\n");
			palloc_free_page(kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack(struct intr_frame *if_)
{
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (kpage != NULL)
	{
		success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page(kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page(void *upage, void *kpage, bool writable)
{
	struct thread *t = thread_current();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment(struct page *page, struct aux *aux)
{
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */

	/* Load aux data */
	struct file *file = aux->file;
	off_t ofs = aux->ofs;
	size_t page_read_bytes = aux->page_read_bytes;
	size_t page_zero_bytes = aux->page_zero_bytes;

	file_seek(file, ofs);
	/* Load this page. */
	if (file_read(file, page->va, page_read_bytes) != (int)page_read_bytes)
	{
		return false;
	}

	memset(page->va + page_read_bytes, 0, page_zero_bytes);

	// free(aux);

	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment(struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT(pg_ofs(upage) == 0);
	ASSERT(ofs % PGSIZE == 0);
	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;
		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		struct aux *aux = calloc(1, sizeof(struct aux));
		aux->file = file;
		aux->ofs = ofs;
		aux->page_read_bytes = page_read_bytes;
		aux->page_zero_bytes = page_zero_bytes;
		if (!vm_alloc_page_with_initializer(VM_ANON, upage,
											writable, lazy_load_segment, aux))
			return false;
		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack(struct intr_frame *if_)
{
	bool success = false;
	void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */
	if (vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true))
	{
		if_->rsp = USER_STACK;
		success = true;
	}

	return success;
}
#endif /* VM */
