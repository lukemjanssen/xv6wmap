#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"
#include "spinlock.h"
#include "wmap.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "fcntl.h"

extern char data[]; // defined by kernel.ld
pde_t *kpgdir;      // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if (*pde & PTE_P)
  {
    pgtab = (pte_t *)P2V(PTE_ADDR(*pde));
  }
  else
  {
    if (!alloc || (pgtab = (pte_t *)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
int mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char *)PGROUNDDOWN((uint)va);
  last = (char *)PGROUNDDOWN(((uint)va) + size - 1);
  for (;;)
  {
    if ((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if (*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap
{
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
    {(void *)KERNBASE, 0, EXTMEM, PTE_W},            // I/O space
    {(void *)KERNLINK, V2P(KERNLINK), V2P(data), 0}, // kern text+rodata
    {(void *)data, V2P(data), PHYSTOP, PTE_W},       // kern data+memory
    {(void *)DEVSPACE, DEVSPACE, 0, PTE_W},          // more devices
};

// Set up kernel part of a page table.
pde_t *
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if ((pgdir = (pde_t *)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void *)DEVSPACE)
    panic("PHYSTOP too high");
  for (k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if (mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                 (uint)k->phys_start, k->perm) < 0)
    {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void switchkvm(void)
{
  lcr3(V2P(kpgdir)); // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void switchuvm(struct proc *p)
{
  if (p == 0)
    panic("switchuvm: no process");
  if (p->kstack == 0)
    panic("switchuvm: no kstack");
  if (p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts) - 1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort)0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir)); // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if (sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W | PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if ((uint)addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walkpgdir(pgdir, addr + i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if (readi(ip, P2V(pa), offset + i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if (newsz >= KERNBASE)
    return 0;
  if (newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for (; a < newsz; a += PGSIZE)
  {
    mem = kalloc();
    if (mem == 0)
    {
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pgdir, (char *)a, PGSIZE, V2P(mem), PTE_W | PTE_U) < 0)
    {
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if (newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for (; a < oldsz; a += PGSIZE)
  {
    pte = walkpgdir(pgdir, (char *)a, 0);
    if (!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if ((*pte & PTE_P) != 0)
    {
      pa = PTE_ADDR(*pte);
      if (pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void freevm(pde_t *pgdir)
{
  uint i;

  if (pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for (i = 0; i < NPDENTRIES; i++)
  {
    if (pgdir[i] & PTE_P)
    {
      char *v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char *)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if (pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t *
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if ((d = setupkvm()) == 0)
    return 0;
  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walkpgdir(pgdir, (void *)i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if (!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);

    // If this is a shared memory region, just map the same physical page to the new page table
    if (flags & MAP_SHARED)
    {
      if (mappages(d, (void *)i, PGSIZE, pa, flags) < 0)
      {
        freevm(d);
        return 0;
      }
    }
    else
    {
      // Otherwise, create a new copy of the memory
      if ((mem = kalloc()) == 0)
      {
        freevm(d);
        return 0;
      }
      memmove(mem, (char *)P2V(pa), PGSIZE);
      if (mappages(d, (void *)i, PGSIZE, V2P(mem), flags) < 0)
      {
        kfree(mem);
        freevm(d);
        return 0;
      }
    }
  }
  return d;
}

// PAGEBREAK!
//  Map user virtual address to kernel address.
char *
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if ((*pte & PTE_P) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  return (char *)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char *)p;
  while (len > 0)
  {
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char *)va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if (n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

// Check if the region [addr, addr+length) overlaps with any existing memory mappings
int region_overlaps(struct proc *curproc, uint addr, int length)
{
  struct wmap_region *wmap_region;
  int i;
  uint end_addr = addr + length;
  for (i = 0; i < 16; i++)
  {
    wmap_region = curproc->wmap_regions[i];
    if (wmap_region != 0)
    {
      uint wmap_start = wmap_region->addr;
      uint wmap_end = wmap_start + wmap_region->length;
      if ((addr >= wmap_start && addr < wmap_end) ||
          (end_addr > wmap_start && end_addr <= wmap_end) ||
          (addr <= wmap_start && end_addr >= wmap_end))
      {
        return 1; // The region overlaps
      }
    }
  }
  return 0; // The region does not overlap
}

// Memory map system call
uint wmap(uint addr, int length, int flags, int fd)
{
  struct proc *curproc = myproc();

  // Check if at least one of the MAP_ANONYMOUS, MAP_SHARED, or MAP_PRIVATE flags is set
  if (!(flags & MAP_ANONYMOUS) && !(flags & MAP_SHARED) && !(flags & MAP_PRIVATE))
  {
    cprintf("wmap: invalid flags\n");
    return -1; // Invalid flags
  }

  // If MAP_FIXED is set, check if the specified address is valid
  if (flags & MAP_FIXED)
  {
    if (addr % PGSIZE != 0 || addr < 0x60000000 || addr >= 0x80000000 || region_overlaps(curproc, addr, length))
    {
      return -1; // Invalid address
    }
  }
  else
  {
    // If MAP_FIXED is not set, find an available region in the virtual address space
    for (addr = 0x60000000; addr < 0x80000000; addr += PGSIZE)
    {
      if (!region_overlaps(curproc, addr, length))
      {
        break;
      }
    }
    if (addr >= 0x80000000)
    {
      return -1; // No available region found
    }
  }

  struct wmap_region *wmap_region;
  int i;
  for (i = 0; i < 16; i++)
  {
    if (curproc->wmap_regions[i] == 0)
    {
      wmap_region = (struct wmap_region *)kalloc();
      if (wmap_region == 0)
      {
        cprintf("wmap: failed to allocate memory for wmap_region\n");
        return -1;
      }

      wmap_region->addr = addr;
      wmap_region->length = length;
      wmap_region->flags = flags;
      wmap_region->fd = fd;
      wmap_region->ref_count = 1;

      // Handle flags
      if (flags & MAP_ANONYMOUS)
      {
        curproc->wmap_regions[i] = wmap_region;
        return addr; // Return the address of the memory region
      }
      else
      {
        struct file *f;
        if (fd < 0 || fd >= NOFILE || (f = curproc->ofile[fd]) == 0 || !(f->readable))
        {
          return -1;
        }
        if (flags & MAP_SHARED || flags & MAP_PRIVATE)
        {
          curproc->wmap_regions[i] = wmap_region;
          return addr; // Return the address of the memory region
        }
      }
    }
  }
  return -1;
}

// Memory unmap system call
int wunmap(uint addr)
{
  struct proc *curproc = myproc();

  // Check if the address is page aligned
  if (addr % PGSIZE != 0)
  {
    return -1; // Invalid address
  }

  struct wmap_region *wmap_region;
  int i;
  for (i = 0; i < 16; i++)
  {
    wmap_region = curproc->wmap_regions[i];
    if (wmap_region != 0 && wmap_region->addr == addr)
    {
      // If it's a file-backed mapping with MAP_SHARED, write the memory data back to the file
      if (!(wmap_region->flags & MAP_ANONYMOUS) && (wmap_region->flags & MAP_SHARED))
      {
        struct file *f = curproc->ofile[wmap_region->fd];
        if (f == 0)
        {
          return -1;
        }
        begin_op();
        ilock(f->ip);
        writei(f->ip, (char *)wmap_region->addr, 0, wmap_region->length); // fix this line
        iunlock(f->ip);
        end_op();
      }

      // Decrement the reference count
      wmap_region->ref_count--;
      if (wmap_region->ref_count == 0)
      {
        // Free the physical memory and remove the wmap_region
        kfree((char *)wmap_region);
        curproc->wmap_regions[i] = 0;
      }
      return 0;
    }
  }

  return -1; // No mapping found
}

// wremap system call
uint wremap(uint oldaddr, int oldsize, int newsize, int flags)
{
  struct proc *curproc = myproc();

  // Check if the old address is page aligned
  if (oldaddr % PGSIZE != 0)
  {
    return -1; // Invalid address
  }

  // Check if the old size is a multiple of PGSIZE
  if (oldsize % PGSIZE != 0)
  {
    return -1; // Invalid size
  }

  // Check if the new size is a multiple of PGSIZE
  if (newsize % PGSIZE != 0)
  {
    return -1; // Invalid size
  }

  // Check if the old address is within the user address space
  if (oldaddr < 0x60000000 || oldaddr >= 0x80000000)
  {
    return -1; // Invalid address
  }

  // Check if the new size is within the user address space
  if (oldaddr + newsize < 0x60000000 || oldaddr + newsize >= 0x80000000)
  {
    return -1; // Invalid address
  }

  // Check if the old address is within a valid memory mapping
  struct wmap_region *wmap_region;
  int i;
  for (i = 0; i < 16; i++)
  {
    wmap_region = curproc->wmap_regions[i];
    if (wmap_region != 0)
    {
      uint wmap_start = wmap_region->addr;
      uint wmap_end = wmap_start + wmap_region->length;
      if (oldaddr >= wmap_start && oldaddr < wmap_end)
      {
        break;
      }
    }
  }
  if (i >= 16)
  {
    return -1; // No valid memory mapping found
  }

  // Check if the new size overlaps with any existing memory mappings
  if (region_overlaps(curproc, oldaddr, newsize))
  {
    return -1; // Overlapping memory mappings
  }

  // If the new size is smaller than the old size, unmap the extra pages
  if (newsize < oldsize)
  {
    for (i = oldaddr + newsize; i < oldaddr + oldsize; i += PGSIZE)
    {
      if (wunmap(i) < 0)
      {
        return -1;
      }
    }
  }
  // If the new size is larger than the old size, map new pages
  else if (newsize > oldsize)
  {
    for (i = oldaddr + oldsize; i < oldaddr + newsize; i += PGSIZE)
    {
      if (wmap(i, PGSIZE, flags, -1) < 0)
      {
        return -1;
      }
    }
  }

  return 0;
}

// Count the number of loaded pages in the region [addr, addr+length)
int count_pages(uint addr, int length)
{
  int count = 0;
  uint a;
  pte_t *pte;

  for (a = addr; a < addr + length; a += PGSIZE)
  {
    if ((pte = walkpgdir(myproc()->pgdir, (void *)a, 0)) != 0 && (*pte & PTE_P))
    {
      count++;
    }
  }

  return count;
}

// Get memory mapping information system call
int getwmapinfo(struct wmapinfo *wminfo)
{
  if (wminfo == 0)
  {
    return -1; // Invalid pointer
  }

  struct proc *curproc = myproc();

  int count = 0;
  for (int i = 0; i < 16; i++)
  {
    struct wmap_region *wmap_region = curproc->wmap_regions[i];
    if (wmap_region != 0)
    {
      wminfo->addr[count] = wmap_region->addr;
      wminfo->length[count] = wmap_region->length;
      wminfo->n_loaded_pages[count] = count_pages(wmap_region->addr, wmap_region->length);
      count++;
    }
  }

  wminfo->total_mmaps = count;

  return 0;
}

int getpgdirinfo(struct pgdirinfo *pdinfo)
{
  struct proc *curproc = myproc();
  pde_t *pgdir;
  pte_t *pte;
  uint pa, i, j;

  if (pdinfo == 0)
  {
    return -1; // Invalid pointer
  }

  if (curproc == 0)
  {
    return -1; // No current process
  }

  pgdir = curproc->pgdir;
  if (pgdir == 0)
  {
    return -1; // No page directory for current process
  }

  pdinfo->n_upages = 0;

  for (i = 0; i < NPDENTRIES; i++)
  {
    if (pgdir[i] & PTE_P)
    {
      pte = (pte_t *)P2V(PTE_ADDR(pgdir[i]));
      for (j = 0; j < NPTENTRIES; j++)
      {
        if (pte[j] & PTE_P && pte[j] & PTE_U)
        {
          pa = PTE_ADDR(pte[j]);
          pdinfo->va[pdinfo->n_upages] = PGADDR(i, j, 0);
          pdinfo->pa[pdinfo->n_upages] = pa;
          pdinfo->n_upages++;
          if (pdinfo->n_upages >= MAX_UPAGE_INFO)
          {
            return 0; // Reached maximum number of pages we can store info about
          }
        }
      }
    }
  }

  return 0;
}
