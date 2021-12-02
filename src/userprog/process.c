#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/parse.h"
#include "userprog/load_arguments.h"
#include "userprog/process.h"
#include "userprog/fd_table.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/ft.h"
#include "vm/spt.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static bool process_wait_for_load (tid_t child_tid);
static bool test_overflow (int argc, char **argv, void *esp);

static bool
file_exists (const char *file_name)
{
  return filesys_open (file_name) != NULL;
}

/* Returns true if the given argv and argc will overflow the page that esp is 
   in when they are loaded into the stack using load_arguments */
static bool 
test_overflow (int argc, char **argv, void *esp) 
{
  // pointer to argv + argc + null return address + max padding
  static int base_size = sizeof (char **) + sizeof (int) + sizeof (void *) + 3;
  int total_size = base_size;

  /* loop through elements in argv adding their sizes */
  for (int i = argc - 1; i >= 0; i--) {
    total_size += strlen (argv[i]) + 1;
  }

  total_size += sizeof (char *) * (argc + 1);   /* pointers to args (including 
                                                   null pointer) */

  // check if the page size behind esp is larger than the total stack size
  return pg_ofs (esp - 1) < total_size; 
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char *process_name;
  char *save_ptr;
  tid_t tid;


  process_name = palloc_get_page (0);
  if (process_name == NULL)
    return TID_ERROR;

  /* Guarantees page will not be overflowed by the copy operations */
  ASSERT (strlen (file_name) + 1 < MAX_CHARS);

  /* Copy a file_name into the page for use by strtok_r to extract 
     the name of the process for giving the thread the correct name */
  int file_name_length = strlen (file_name) + 1;
  strlcpy (process_name, file_name, file_name_length);
  process_name = strtok_r (process_name, " ", &save_ptr);
  
  /* Make another copy of FILE_NAME to be parsed and args loaded in start_process
     Otherwise there's a race between the caller and load(). */
  int process_name_length = strlen (process_name) + 1;
  fn_copy = process_name + process_name_length;
  strlcpy (fn_copy, file_name, file_name_length);

  if (!file_exists (process_name)) 
  {
    palloc_free_page (pg_round_down (process_name));
    return TID_ERROR; 
  }

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (process_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR || !process_wait_for_load (tid))
    {
      tid = TID_ERROR;
      palloc_free_page (pg_round_down (process_name));
    }
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  char *file_name = file_name_;
  struct intr_frame if_;
  bool success;

  int argc;
  char *argv[MAX_ARGS + 1];
  char argv_store[MAX_CHARS + 1];
  // Also side affects file_name to recieve program name 
  parse (file_name, &argc, argv, argv_store);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs     = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success    = load (file_name, &if_.eip, &if_.esp);
  void *initial_esp = pg_round_up (if_.esp); // store esp to check for overflow
  if (test_overflow (argc, argv, if_.esp)) 
    success = false;
  else
    load_arguments (argc, argv, &if_.esp);
  
  /* check that esp has not overflowed the initial page */
  success &= pg_round_up (initial_esp) == pg_round_up (if_.esp);

  palloc_free_page (pg_round_down (file_name));
  struct child *self_child_ptr = thread_current ()->self_child_ptr;

  self_child_ptr->load_successful = success;
  if (!success) 
    {
      self_child_ptr->tid = TID_ERROR;
      sema_up (&self_child_ptr->load_sema);
      thread_exit ();
    }
  else 
    {
      thread_current ()->executable = filesys_open (argv[0]);
      file_deny_write (thread_current ()->executable);
      sema_up (&self_child_ptr->load_sema);
    }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

static struct child *
find_child (struct thread *parent, tid_t child_tid)
{
  struct list_elem *e;
  /* Iterates the list of children to match the given tid. */
  for (e  = list_begin (&parent->children);
       e != list_end (&parent->children); 
       e  = list_next(e))
    {
      struct child *cp_check = list_entry (e, struct child, elem);
      if (cp_check->tid == child_tid) return cp_check;
    }
  return NULL;
}

/* Waits for thread TID to die and return its exit status. 
   If it was terminated by the kernel (i.e. killed due to an exception), 
   returns -1.  
   If the given TID is invalid, was not a child of the calling process, or if 
   process_wait() has already been successfully called for the given TID, 
   returns -1 immediately, without waiting. */
int
process_wait (tid_t child_tid) 
{
  /* Checks whether the given tid matches one of current thread's children */
  struct child *cp = find_child (thread_current (), child_tid);

	/* If a child thread with the given tid is found, waits until it finishes
     to run and to deallocates its corresponding child struct after
     returning the child struct's exit status. */
  if (cp != NULL)
    {
      sema_down (&cp->sema);

      /* At this point cp is unblocked. Store the exit_status and
         remove the cp from the parent's children list to free its memory.
         This ensures that the parent will not wait for the same child twice */ 
      int exit_status = cp->exit_status;
      list_remove (&cp->elem);
      free (cp);
			return exit_status;
    }
  else 
	    /* If child thread not found, then either the parent called wait
         on a child that returned already or the given tid is invalid. */
      return TID_ERROR;
}

/* Blocks the current thread's given child until it finishes loading */
static bool
process_wait_for_load (tid_t child_tid)
{
  ASSERT (child_tid != TID_ERROR);
  struct child *cp = find_child (thread_current (), child_tid);
  ASSERT (cp != NULL);
  sema_down (&cp->load_sema);
  /* At this point load will be finished for cp, and this function
     returns the bool value load_successful to check the load status */
  bool success = cp->load_successful;
  /* If child load unsuccessful remove the child process from it's child list */
  if (!success) list_remove (&cp->elem);
  return success;
}

/* Termination of a process.
   Frees the current process's resources in prior. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

	/* Sets thread pointer of child struct of current thread to null,
	   since the thread is about to be terminated. */
	if (cur->tid != TID_MAIN) cur->self_child_ptr->thread_ptr = NULL;

	/* List elem to iterate through the thread's children list */
	struct list_elem *e;

  /* Iterates through the list of children to frees the child structs. */
  for (e = list_begin (&cur->children);
        e != list_end (&cur->children); 
        e = list_next(e))
    {
      struct child *child_ptr = list_entry (e, struct child, elem);

      /* Acquires the lock to set the child thread's self_child_ptr to null.
         The lock is necessary since the thread's child threads may be running
         while the thread is terminating */
			struct thread* child_t = child_ptr->thread_ptr;
			if (child_t != NULL)
			{
				lock_acquire (&child_t->self_lock);
				child_t->self_child_ptr = NULL;
				lock_release (&child_t->self_lock);
			}
      
      /* Releases the child struct */
      free ((void *) child_ptr);
    }

    if (cur->tid != TID_MAIN) hash_destroy (cur->hash_fd_ptr, &fd_hash_free);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);

  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
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
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  file_close (file);
  return success;
}

/* load() helpers. */

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
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

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
      
      /* Check if virtual page already allocated */
      struct thread *t = thread_current ();
      struct spte *spte_ptr = spt_find_entry (t->spt_ptr, upage);

      enum frame_type frame_type;

      if (page_zero_bytes == PGSIZE) frame_type = ALL_ZERO;
      else if (writable)             frame_type = EXECUTABLE_DATA;
      else if (!writable)            frame_type = EXECUTABLE_CODE;
      else                           NOT_REACHED ();

      if (spte_ptr == NULL) 
        {
          spt_add_entry (t->spt_ptr,
                         0,           // 0 fid infers no frame exists yet
                         upage,
                         frame_type,
                         (frame_type == ALL_ZERO) ? NULL : file->inode,
                         (frame_type == ALL_ZERO) ? 0    : ofs,
                         page_read_bytes,
                         writable);
        }
      else
        {
          /* If either frame had type executable data, this one should too
             to make it writable */
          spte_ptr->writable       |= writable;
          spte_ptr->amount_occupied = page_read_bytes;
          spte_ptr->frame_type      = frame_type;
        }


      /* Advance. */
      ofs        += page_read_bytes;
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage      += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  bool success = false;
  struct thread *t_ptr = thread_current ();

  struct fte *fte_ptr = ft_get_frame (t_ptr->tid,
                                      ALL_ZERO,
                                      NULL,
                                      0,
                                      PGSIZE);
  

  if (fte_ptr != NULL) 
    {
      uint8_t *uaddr = ((uint8_t *) PHYS_BASE) - PGSIZE;
      success = install_page (uaddr, fte_ptr->frame_location, true);
      if (success)
        {
          *esp = PHYS_BASE;
          spt_add_entry (t_ptr->spt_ptr, fte_ptr->fid, uaddr, STACK, NULL, 0, 
              PGSIZE, true);
          fte_ptr->pinned = false;
        }
      else 
        ft_remove_frame (fte_ptr);
        
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
