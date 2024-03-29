#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "wmap.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "fcntl.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void tvinit(void)
{
  int i;

  for (i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE << 3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE << 3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void idtinit(void)
{
  lidt(idt, sizeof(idt));
}

// PAGEBREAK: 41
void trap(struct trapframe *tf)
{
  if (tf->trapno == T_SYSCALL)
  {
    if (myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if (myproc()->killed)
      exit();
    return;
  }

  switch (tf->trapno)
  {
  case T_PGFLT:
  {
    uint faulting_address = rcr2();
    struct proc *curproc = myproc();
    struct wmap_region *wmap_region;
    int i;
    for (i = 0; i < 16; i++)
    {
      wmap_region = curproc->wmap_regions[i];
      if (wmap_region != 0 &&
          faulting_address >= wmap_region->addr &&
          faulting_address < wmap_region->addr + wmap_region->length)
      {
        // Calculate the number of pages in the mapped region
        uint num_pages = PGROUNDUP(wmap_region->length) / PGSIZE;

        // Loop over each page in the mapped region
        for (uint j = 0; j < num_pages; j++)
        {
          // Calculate the faulting address for this page
          uint faulting_address_page = PGROUNDDOWN(faulting_address) + j * PGSIZE;

          char *mem = kalloc();
          if (mem == 0)
          {
            cprintf("out of memory\n");
            kill(curproc->pid);
            return;
          }
          memset(mem, 0, PGSIZE);

          // If the mapping is file-backed, read the data from the file
          if (!(wmap_region->flags & MAP_ANONYMOUS))
          {
            // Check the file descriptor
            struct file *f = curproc->ofile[wmap_region->fd];
            if (f == 0 || !(f->readable))
            {
              cprintf("invalid file descriptor\n");
              kill(curproc->pid);
              return;
            }

            // Calculate the offset in the file
            uint offset = faulting_address_page - wmap_region->addr;

            // Check the offset
            if (offset >= f->ip->size)
            {
              cprintf("invalid offset\n");
              kill(curproc->pid);
              return;
            }

            // Calculate the number of bytes to read
            uint remaining_bytes_in_file = f->ip->size - offset;
            uint n = min(remaining_bytes_in_file, (uint)PGSIZE);

            // Read the data from the file
            ilock(f->ip);
            int bytesRead = readi(f->ip, mem, offset, n);
            iunlock(f->ip);

            // Check the number of bytes read
            if (bytesRead != n)
            {
              cprintf("failed to read data from file\n");
              kill(curproc->pid);
              return;
            }
          }

          if (mappages(curproc->pgdir, (char *)faulting_address_page, PGSIZE, V2P(mem), PTE_W | PTE_U) < 0)
          {
            cprintf("out of memory (2)\n");
            kfree(mem); // Free the allocated page
            kill(curproc->pid);
            return;
          }
          break;
        }
        return;
      }
    }
    // If the faulting address is not within a file-backed memory mapping, segfault
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
    return;
  }
  case T_IRQ0 + IRQ_TIMER:
    if (cpuid() == 0)
    {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE + 1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  // PAGEBREAK: 13
  default:
    if (myproc() == 0 || (tf->cs & 3) == 0)
    {
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if (myproc() && myproc()->state == RUNNING &&
      tf->trapno == T_IRQ0 + IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();
}
