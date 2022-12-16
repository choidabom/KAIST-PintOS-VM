#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

/* Lock used by allocated_tid() */
struct lock filesys_lock;

void syscall_init(void);
void exit_handler(int status);

#endif /* userprog/syscall.h */
