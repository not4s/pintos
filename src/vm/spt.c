#include <debug.h>
#include <hash.h>
#include "threads/malloc.h"
#include "vm/spt.h"
#include "vm/ft.h"


/* SPT hashmap helpers */
static unsigned spte_hash_func       (const struct hash_elem *e_ptr, 
                                      void *aux UNUSED);
static bool     spte_less_func       (const struct hash_elem *a_ptr,
                                      const struct hash_elem *b_ptr,
                                      void *aux UNUSED);
static void     spte_deallocate_func (struct hash_elem *e_ptr, 
                                      void *aux UNUSED);

static struct spte *spte_construct (void *uaddr,
                                    struct fte *fte_ptr,
                                    enum frame_type frame_type,
                                    struct inode *inode_ptr,
                                    off_t offset,
                                    int amount_occupied,
                                    bool writable);


/* Attempts to initialise the supplementary page table 
   Returns false if failed and true if succeeeded. */
bool 
spt_init (struct hash **spt_ptr_ptr)
{
  *spt_ptr_ptr = malloc (sizeof (struct hash));
  if (*spt_ptr_ptr == NULL) return false;

  if (!hash_init (*spt_ptr_ptr, &spte_hash_func, &spte_less_func, NULL)) 
    {
      free (*spt_ptr_ptr);
      return false;
    }

  return true;
}

void 
spt_destroy (struct hash *spt_ptr)
{
  /* TODO: Close associated files if necesary (and if not shared) */
  ASSERT (spt_ptr != NULL);
  hash_destroy (spt_ptr, &spte_deallocate_func);
  free (spt_ptr);
}

/* Constructs supplementary page table entry and inserts it to given 
   supplementary page table hashmap, returns NULL if spte memory allocation
   fail */
struct spte *
spt_add_entry (struct hash *spt_ptr,
               struct fte *fte_ptr,
               void *uaddr,
               enum frame_type frame_type,
               struct inode *inode_ptr,
               off_t offset,
               int amount_occupied,
               bool writable)
{
  struct spte *spte_ptr = spte_construct (uaddr, fte_ptr, frame_type, 
      inode_ptr, offset, amount_occupied, writable);

  if (spte_ptr == NULL) return NULL;

  hash_insert (spt_ptr, &spte_ptr->hash_elem);

  return spte_ptr;
}

/* Finds and removes a supplementary page table entry at uaddr, freeing 
   associated memory.
   Returns true on success, and fale if entry not found */
bool
spt_remove_entry (struct hash *spt_ptr, void *uaddr)
{
  struct spte *spte_ptr = spt_find_entry (spt_ptr, uaddr);

  if (spte_ptr == NULL || 
      hash_delete (spt_ptr, &spte_ptr->hash_elem) == NULL) return false;

  // TODO: check if entry is in ft, and if so retrieve it
  //       if entry in swap space check status and remove it
  //       remove swap space entry
  //       if entry not in swap space (i.e. in memory)
  //       then remove it from memory and save/delete it
  //       free ft entry

  return true;
}

/* removes the page at uaddr from the spt, removes
   its associated entry in the frame table,
   and removes it from memory/swap space */
bool 
spt_propagated_removal (struct hash *spt_ptr, 
                        struct hash *pt_ptr,
                        void *uaddr)
{
  struct spte *spte_ptr = spt_find_entry (spt_ptr, uaddr);

  if (spte_ptr == NULL || 
      hash_delete (spt_ptr, &spte_ptr->hash_elem) == NULL) return false;

  /* the frame is removed from memory/swap space inside of ft_remove_frame */
  ft_remove_frame (spte_ptr->fte_ptr);

  free (spte_ptr);
  
  return true;
}

/* Attempts to find supplementary page table entry, returns NULL if not
   present */
struct spte *
spt_find_entry (struct hash *spt_ptr, void *uaddr)
{
  /* Create fake supplementary page table entry to search by */
  struct spte spte;
  spte.uaddr = uaddr;

  /* Attempt to locate entry */
  struct hash_elem *hash_elem_ptr = hash_find (spt_ptr, &spte.hash_elem);

  /* Return NULL on failure or the surrounding struct spte on success */
  if (hash_elem_ptr == NULL) return NULL;
  return hash_entry (hash_elem_ptr, struct spte, hash_elem);
}

/* Constructs a supplmental page table entry, returns NULL
   if memory allocation fails */
static struct spte *
spte_construct (void *uaddr,
                struct fte *fte_ptr,
                enum frame_type frame_type,
                struct inode *inode_ptr,
                off_t offset,
                int amount_occupied,
                bool writable)
{
  struct spte *spte_ptr = malloc (sizeof (struct spte));
  if (spte_ptr == NULL) return NULL;

  spte_ptr->uaddr           = uaddr;
  spte_ptr->fte_ptr         = fte_ptr;
  spte_ptr->frame_type      = frame_type;
  spte_ptr->inode_ptr       = inode_ptr;
  spte_ptr->offset          = offset;
  spte_ptr->amount_occupied = amount_occupied;
  spte_ptr->writable        = writable;

  return spte_ptr;
}

static unsigned
spte_hash_func (const struct hash_elem *e_ptr, void *aux UNUSED)
{
  return (unsigned) hash_entry (e_ptr, struct spte, hash_elem)->uaddr;
}

static bool 
spte_less_func (const struct hash_elem *a_ptr,
                const struct hash_elem *b_ptr,
                void *aux UNUSED) 
{
  return hash_entry (a_ptr, struct spte, hash_elem)->uaddr <
         hash_entry (b_ptr, struct spte, hash_elem)->uaddr;
}

static void
spte_deallocate_func (struct hash_elem *e_ptr, void *aux UNUSED)
{
  free (hash_entry (e_ptr, struct spte, hash_elem));
}
