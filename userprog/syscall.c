#include "userprog/syscall.h"
#include <stdio.h>
#include <stdbool.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "lib/kernel/stdio.h"
#include "userprog/process.h"
#include "include/threads/palloc.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
bool check_address(bool write, void *addr);
void exit_handler(int status);
tid_t fork_handler(const char *thread_name, struct intr_frame *f);
int exec_handler(const char *file_name);
int wait_handler(tid_t tid);
bool create_handler(const char *file, unsigned initial_size);
bool remove_handler(const char *file);
int open_handler(const char *file);
int filesize_handler(int fd);
int read_handler(int fd, void *buffer, unsigned size);
int write_handler(int fd, const void *buffer, unsigned size);
void seek_handler(int fd, unsigned position);
unsigned tell_handler(int fd);
void close_handler(int fd);
int process_add_file(struct file *f); /* 파일 객체에 대한 파일 디스크립터 생성 */

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
							((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			  FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
	lock_init(&filesys_lock); /* filesys_lock 초기화 코드 */
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f)
{
	// TODO: Your implementation goes here.
	int syscall_num = f->R.rax;
	uint64_t a1 = f->R.rdi;
	uint64_t a2 = f->R.rsi;
	uint64_t a3 = f->R.rdx;
	uint64_t a4 = f->R.r10;
	uint64_t a5 = f->R.r8;
	uint64_t a6 = f->R.r9;

	switch (syscall_num)
	{
	case SYS_HALT:
		halt_handler();
	case SYS_EXIT:
		exit_handler(a1);
		break;
	case SYS_FORK:
		f->R.rax = fork_handler(a1, f);
		break;
	case SYS_EXEC:
		f->R.rax = exec_handler(a1);
		break;
	case SYS_WAIT:
		f->R.rax = wait_handler(a1);
		break;
	case SYS_CREATE:
		f->R.rax = create_handler(a1, a2);
		break;
	case SYS_REMOVE:
		f->R.rax = remove_handler(a1);
		break;
	case SYS_OPEN:
		f->R.rax = open_handler(a1);
		break;
	case SYS_FILESIZE:
		f->R.rax = filesize_handler(a1);
		break;
	case SYS_READ:
		f->R.rax = read_handler(a1, a2, a3);
		break;
	case SYS_WRITE:
		f->R.rax = write_handler(a1, a2, a3);
		break;
	case SYS_SEEK:
		seek_handler(a1, a2);
		break;
	case SYS_TELL:
		f->R.rax = tell_handler(a1);
		break;
	case SYS_CLOSE:
		close_handler(a1);
		break;
	default:
		break;
	}
}

void halt_handler()
{
	power_off();
}

/* 현재 프로세스를 종료시키는 시스템 콜 */
/* staus: 프로그램이 정상적으로 종료됐는지 확인, 정상적 종료 시 status = 0*/
void exit_handler(int status)
{
	struct thread *cur = thread_current();
	cur->exit_status = status;
	thread_exit();
}
/* thread_name이라는 이름을 가진 (현재 프로세스의 복제본인) 새 프로세스를 만들어주는 시스템 콜 */
/* 자식 프로세스의 tid를 반환해야 함. 유효한 tid가 아닌 경우 0을 반환해야함. */
tid_t fork_handler(const char *thread_name, struct intr_frame *f)
{

	return process_fork(thread_name, f);

	// fork() 의 반환 값이 0보다 작으면 오류.->TID_ERROR를 반환한다.fork() 의 반환 값이 0이면 자식 프로세스.fork() 의 반환 값이 0보다 크면 부모 프로세스.thread_create를 해서 자식프로세스(스레드) 를 생성한다.
}

/* 현재 프로세스를 인자로 주어진 이름을 갖는 실행 파일로 변경한다.  */
int exec_handler(const char *file_name)
{
	check_address(0, file_name);
	char *file = palloc_get_page(PAL_ZERO);
	strlcpy(file, file_name, strlen(file_name) + 1);

	if (process_exec(file) == -1)
	{
		return -1;
	}
}

int wait_handler(tid_t tid)
{
	return process_wait(tid);
}

/* 파일을 생성하는 시스템 콜 */
bool create_handler(const char *file, unsigned initial_size)
{
	check_address(0, file);
	if (file == NULL)
	{
		exit_handler(-1);
	}
	return filesys_create(file, initial_size);
}

/* 파일을 삭제하는 시스템 콜 */
/* file: 제거할 파일의 이름 및 경로 정보 */
bool remove_handler(const char *file)
{
	check_address(0, file);
	return filesys_remove(file);
}

int open_handler(const char *file)
{
	check_address(0, file);
	if (thread_current()->fd_count > 20)
	{
		return -1;
	}
	if (*file == NULL)
		return -1;

	struct file *open_file = filesys_open(file);
	if (open_file == NULL)
		return -1;
	int fd = process_add_file(open_file);

	return fd;
}

/* fd로 열려있는 파일의 사이즈를 리턴해주는 시스템 콜 */
int filesize_handler(int fd)
{
	struct thread *curr = thread_current();
	struct list_elem *start;

	for (start = list_begin(&curr->fd_list); start != list_end(&curr->fd_list); start = list_next(start))
	{
		struct file_fd *now_fd = list_entry(start, struct file_fd, fd_elem);
		if (now_fd->fd == fd)
		{
			return file_length(now_fd->file);
		}
	}
}

// bad-fd는 page-fault를 일으키기 때문에 page-fault를 처리하는 함수에서 확인
int read_handler(int fd, void *buffer, unsigned size)
{
	check_address(1, buffer);
	struct thread *curr = thread_current();
	struct list_elem *start;
	off_t buff_size;

	if (fd == 0)
	{
		return input_getc();
	}
	else if (fd < 0 || fd == NULL || fd == 1)
	{
		exit_handler(-1);
	}
	// bad-fd는 page-fault를 일으키기 때문에 page-fault를 처리하는 함수에서 확인
	else
	{
		for (start = list_begin(&curr->fd_list); start != list_end(&curr->fd_list); start = list_next(start))
		{
			struct file_fd *read_fd = list_entry(start, struct file_fd, fd_elem);
			if (read_fd->fd == fd)
			{
				lock_acquire(&filesys_lock);
				buff_size = file_read(read_fd->file, buffer, size);
				lock_release(&filesys_lock);
			}
		}
	}
	return buff_size;
}

int write_handler(int fd, const void *buffer, unsigned size)
{
	struct thread *curr = thread_current();
	struct list_elem *start;
	if (fd == 1)
	{
		putbuf(buffer, size);
		return size;
		// fd == 1이라는 의미는 표준 출력을 의미함. 따라서 화면에 입력된 데이터를 출력하는 함수 pufbuf를 호출.
		// putbuf 함수는 buffer에 입력된 데이터를 size만큼 화면에 출력하는 함수.
		// 이후 버퍼의 크기 -> size를 반환한다.
	}
	else if (fd < 0 || fd == NULL || !check_address(0, buffer))
	{
		exit_handler(-1);
	}
	for (start = list_begin(&curr->fd_list); start != list_end(&curr->fd_list); start = list_next(start))
	{
		struct file_fd *write_fd = list_entry(start, struct file_fd, fd_elem);
		// if (write_fd->file == NULL){
		// 	exit_handler(-1);
		// }
		if (write_fd->fd == fd)
		{
			lock_acquire(&filesys_lock);
			off_t write_size = file_write(write_fd->file, buffer, size);
			// fd == 0 과 fd == 1은 표준 입출력을 의미하는 파일 식별자이기 때문에 해당되는 파일이 존재하지 않는다.
			// 따라서 정상적인 write가 이루어지지 않는다. fd == 1이면 write 함수의 반환값은 0임.
			lock_release(&filesys_lock);
			return write_size;
		}
	}
}

/* fd로 열려있는 파일의 (읽고 쓸 위치를 알려주는)포인터의 위치를 변경해주는 시스템 콜 */
void seek_handler(int fd, unsigned position)
{
	struct thread *curr = thread_current();
	struct list_elem *start;

	for (start = list_begin(&curr->fd_list); start != list_end(&curr->fd_list); start = list_next(start))
	{
		struct file_fd *seek_fd = list_entry(start, struct file_fd, fd_elem);
		if (seek_fd->fd == fd)
		{
			file_seek(seek_fd->file, position);
		}
	}
}

/* fd에서 읽히거나 써질 다음 바이트의 위치를 반환 */
unsigned
tell_handler(int fd)
{
	struct thread *curr = thread_current();
	struct list_elem *start;

	for (start = list_begin(&curr->fd_list); start != list_end(&curr->fd_list); start = list_next(start))
	{
		struct file_fd *tell_fd = list_entry(start, struct file_fd, fd_elem);
		if (tell_fd->fd == fd)
		{
			return file_tell(tell_fd->file);
		}
	}
}

/* fd를 닫는 시스템 콜 */
void close_handler(int fd)
{
	struct thread *curr = thread_current();
	struct list_elem *start;
	for (start = list_begin(&curr->fd_list); start != list_end(&curr->fd_list); start = list_next(start))
	{
		struct file_fd *close_fd = list_entry(start, struct file_fd, fd_elem);
		if (close_fd->fd == fd)
		{
			file_close(close_fd->file);
			list_remove(&close_fd->fd_elem);
			// close_fd->fd = NULL;
			// free(close_fd);
		}
	}
	return;
}

bool check_address(bool write, void *addr)
{
	struct thread *curr = thread_current();
	if (addr == NULL)
		return false;
	struct page *p = spt_find_page(&curr->spt, pg_round_down(addr));
	if (p == NULL)
		return false;
	else{
		if (write && !p->writable){
			return false;
		}
	}
	return true;

}

// int process_add_file(struct file *f)
// {
// 	struct thread *curr = thread_current();
// 	struct file_fd *new_fd = malloc(sizeof(struct file_fd));

// 	// curr에 있는 fd_list의 fd를 확인하기 위한 작업
// 	// list_begin 했을 경우 fd = 0 출력되고, list_back 했을 경우 fd = 1 출력됨
// 	struct list_elem *curr_elem = list_back(&curr->fd_list);
// 	struct file_fd *curr_fd = list_entry(curr_elem, struct file_fd, fd_elem);
// 	printf("%d\n", curr_fd->fd);

// 	curr_fd->fd += 1;
// 	new_fd->fd = curr_fd->fd; => 이 부분에 확신이 없음
// 	new_fd->file = f;
// 	list_push_back(&curr->fd_list, &new_fd->fd_elem);

// 	return new_fd->fd;
// }

int process_add_file(struct file *f)
{
	struct thread *curr = thread_current();
	struct file_fd *new_fd = malloc(sizeof(struct file_fd));

	// curr에 있는 fd_list의 fd를 확인하기 위한 작업
	// list_begin 했을 경우 fd = 0 출력되고, list_back 했을 경우 fd = 1 출력됨
	// struct list_elem *check = list_begin(&curr->fd_list);
	// struct file_fd *check_fd = list_entry(check, struct file_fd, fd_elem);
	// printf("%d\n", check_fd->fd);
	// 악 대박 fd 나옴 ~!~!

	curr->fd_count += 1;
	new_fd->fd = curr->fd_count;
	new_fd->file = f;
	// printf(" new fd : %d \n", new_fd->fd);
	list_push_back(&curr->fd_list, &new_fd->fd_elem);

	return new_fd->fd;
}