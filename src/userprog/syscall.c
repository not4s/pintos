#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

/* System call helper functions */
static void     syscall_halt     (void);
static void     syscall_exit     (int status);
static pid_t    syscall_exec     (const char *cmd_line);
static int      syscall_wait     (pid_t pid);
static bool     syscall_create   (const char *file, unsigned initial_size);
static bool     syscall_remove   (const char *file);
static int      syscall_open     (const char *file);
static int      syscall_filesize (int fd);
static int      syscall_read     (int fd, void *buffer, unsigned size);
static int      syscall_write    (int fd, const void *buffer, unsigned size);
static void     syscall_seek     (int fd, unsigned position);
static unsigned syscall_tell     (int fd);
static void     syscall_close    (int fd);

static void     syscall_handler (struct intr_frame *f);
static void     verify_ptr      (void *ptr);
static void     verify_args     (int argc, void *esp);
static uint32_t invoke_function (void *function_ptr, int argc, void *esp);

static struct     lock filesys_lock;

static struct function 
syscall_func_map[] = 
  {
    {&syscall_halt,     .argc = 0},  /* SYS_HALT */      
    {&syscall_exit,     .argc = 1},  /* SYS_EXIT */      
    {&syscall_exec,     .argc = 1},  /* SYS_EXEC */      
    {&syscall_wait,     .argc = 1},  /* SYS_WAIT */      
    {&syscall_create,   .argc = 2},  /* SYS_CREATE */    
    {&syscall_remove,   .argc = 1},  /* SYS_REMOVE */    
    {&syscall_open,     .argc = 1},  /* SYS_OPEN */      
    {&syscall_filesize, .argc = 1},  /* SYS_FILESIZE */  
    {&syscall_read,     .argc = 3},  /* SYS_READ */      
    {&syscall_write,    .argc = 3},  /* SYS_WRITE */     
    {&syscall_seek,     .argc = 2},  /* SYS_SEEK */      
    {&syscall_tell,     .argc = 1},  /* SYS_TELL */      
    {&syscall_close,    .argc = 1},  /* SYS_CLOSE */     
  };

static struct file *
filesys_fd_map[MAX_OPEN_FILES];

void
syscall_init (void) 
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Allows user processes to ask the kernel to execute operations
   that they don't have permission to execute themselves */
static void
syscall_handler (struct intr_frame *f) 
{
  /* Read the syscall number at the stack pointer (f->esp) */
  verify_ptr (f->esp);
  int syscall_no = *((int *) f->esp);

  /* Ensure our syscall_no refers to a defined system call */
  ASSERT (SYS_HALT <= syscall_no && syscall_no <= SYS_CLOSE);

  /* Read the argc and function_ptr values from our syscall_func_map */
  int   argc         = syscall_func_map[syscall_no].argc;
  void *function_ptr = syscall_func_map[syscall_no].function_ptr;

  /* Verify the necessary number of arguments before passing function
     invocation to invoke_function */
  verify_args (argc, f->esp);
  f->eax = invoke_function (function_ptr, argc, f->esp);
}

static void 
verify_args (int argc, void *esp) 
{
  switch (argc)
    {
      case 3: verify_ptr (esp + 3); __attribute__ ((fallthrough));
      case 2: verify_ptr (esp + 2); __attribute__ ((fallthrough));
      case 1: verify_ptr (esp + 1); __attribute__ ((fallthrough));
      default: break;
    }
}

static uint32_t 
invoke_function (void *function_ptr, int argc, void *esp) 
{
  switch (argc) 
    {
      case 0: 
        return ((function_0_args) function_ptr) (); 
      case 1: 
        return ((function_1_args) function_ptr) (((uint32_t *) esp)[1]); 
      case 2: 
        return ((function_2_args) function_ptr) (((uint32_t *) esp)[1], 
                                                 ((uint32_t *) esp)[2]); 
      case 3: 
        return ((function_3_args) function_ptr) (((uint32_t *) esp)[1], 
                                                 ((uint32_t *) esp)[2], 
                                                 ((uint32_t *) esp)[3]); 
      default: NOT_REACHED (); 
    }
}


static void
verify_ptr (void *ptr)
{
  if (ptr != NULL && 
      /* Verify address in user space */
      is_user_vaddr (ptr) && 
      /* Verify address in page directory*/
      pagedir_get_page (active_pd (), ptr) != NULL) 
    return;

  /* If this point is reached the pointer is not valid. Exit with -1 */
  syscall_exit (-1);
  NOT_REACHED ();
}

/* SYS_HALT */
static void
syscall_halt (void)
{
  /* TODO implementation */
}

/* SYS_EXIT 
 * Free current process resources and output process name and exit code.*/
static void
syscall_exit (int status UNUSED)
{
	// Retrieve name of process.
	char name[16];
	strlcpy (name, thread_current ()->name, sizeof name);
	process_exit ();
	
	// Generate ourput string.
	uint8_t buf_size = 32; //is size of 25 safe?
	char str[buf_size];
	ASSERT (snprintf (str, buf_size, "%s: exit(%d)\n", name, status) == 0);
	syscall_write (1, str, buf_size);
  thread_exit ();
}

/* SYS_EXEC */
static pid_t
syscall_exec (const char *cmd_line UNUSED)
{
  /* TODO implementation */
  return 0;
}

/* SYS_WAIT */
static int
syscall_wait (pid_t pid UNUSED)
{
  /* TODO implementation */
  return 0;
}

/* SYS_CREATE */
static bool
syscall_create (const char *file UNUSED, unsigned initial_size UNUSED)
{
  /* TODO implementation */
  return 0;
}

/* SYS_REMOVE */
static bool
syscall_remove (const char *file UNUSED)
{
  /* TODO implementation */
  return 0;
}

/* SYS_OPEN */
static int
syscall_open (const char *file UNUSED)
{
  /* TODO implementation */
  return 0;
}

/* SYS_FILESIZE */
static int
syscall_filesize (int fd UNUSED)
{
  /* TODO implementation */
  return 0;
}

/* SYS_READ */
static int
syscall_read (int fd UNUSED, void *buffer UNUSED, unsigned size UNUSED)
{
  /* TODO implementation */
  return 0;
}

/* SYS_WRITE */
static int
syscall_write (int fd, const void *buffer, unsigned size)
{
  ASSERT (fd >= 1);
  ASSERT (buffer != NULL);
  ASSERT ((unsigned long) fd <= 
          sizeof (filesys_fd_map) / sizeof(struct file *));

  lock_acquire (&filesys_lock);

  int bytes_written;

  /* fd 1 refers to the console, so output is sent to putbuf */
  if (fd == 1) 
    {
      uint32_t buffer_size = sizeof(buffer);
      uint32_t bytes_to_write;
      char temp_buffer[MAX_CONSOLE_BUFFER_SIZE];
      
      /* here we break the buffer into chunks of size MAX_CONSOLE_BUFFER_SIZE */
      for (uint32_t offset = 0; 
        offset <= size;
        offset += MAX_CONSOLE_BUFFER_SIZE) 
        {
          bytes_to_write = 
            (buffer_size - offset) > ((uint32_t) MAX_CONSOLE_BUFFER_SIZE) ?
            (buffer_size - offset) : ((uint32_t) MAX_CONSOLE_BUFFER_SIZE);
          memcpy(temp_buffer, &((char *) buffer)[offset / sizeof(char)], 
            bytes_to_write);
          putbuf(temp_buffer, bytes_to_write);
        }
      bytes_written = sizeof(buffer);
    }
  else 
    {
      /* if not fd 1, we find the corresponding file_ptr and then write to it */
      struct file *file_ptr = filesys_fd_map[fd];
      ASSERT (file_ptr != NULL); 
      bytes_written = file_write(file_ptr, buffer, size);
    }

  lock_release (&filesys_lock);
  return bytes_written;
}

/* SYS_SEEK */
static void
syscall_seek (int fd UNUSED, unsigned position UNUSED)
{
  /* TODO implementation */
}

/* SYS_TELL */
static unsigned
syscall_tell (int fd UNUSED)
{
  /* TODO implementation */
  return 0;
}

/* SYS_CLOSE */
static void
syscall_close (int fd UNUSED)
{
  /* TODO implementation */
}