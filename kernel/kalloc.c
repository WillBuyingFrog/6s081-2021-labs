// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

int cow_count[(PHYSTOP-KERNBASE)/PGSIZE];

int cow_init = 0;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
  cow_init = 1;
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  int cow_index = ((uint64)pa - KERNBASE) / PGSIZE;
  if(cow_init && cow_count[cow_index] <= 0)
    panic("kfree: cow_count");
  
  if(cow_count[cow_index] > 1){
    cow_count[cow_index] -= 1;
    return;
  }

  cow_count[cow_index] = 0;
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk
    int cow_index = ((uint64)r - KERNBASE) / PGSIZE;
    cow_count[cow_index] = 1;
  }
  
  return (void*)r;
}


void *
cow_kalloc(pagetable_t pagetable, uint64 va){
  // 将pagetable中原来映射虚拟地址va的页表项清除
  // 然后kalloc一个新的页，复制旧页的内容到新页上
  // 然后kfree掉旧页对应的物理页
  // 最后将va映射到新的页上
  pte_t *pte_old;
  int old_perm;
  uint64 old_pa, new_pa;

  if((pte_old = walk(pagetable, va, 0)) == 0)
    panic("cow_kalloc: old_pte should exist");
  
  old_perm = PTE_FLAGS(*pte_old);
  // 清除掉cow页标记
  old_perm &= ~(PTE_COW);

  old_pa = PTE2PA(*pte_old);

  printf("Ready to unmap old COW page, va %p, pa %p, perm(without PTE_COW) %p\n", va, old_pa, old_perm);

  uvmunmap(pagetable, va, 1, 1);

  new_pa = (uint64)kalloc();

  if(new_pa == 0){
    return 0;
  }
    
  
  memmove((char *)new_pa, (char *)old_pa, PGSIZE);

  // kfree((char*)old_pa);

  if(mappages(pagetable, va, PGSIZE, new_pa, old_perm | PTE_W) < 0){
    panic("cow_kalloc: new pte should be properly mapped");
  }
  return (void*)new_pa;
}