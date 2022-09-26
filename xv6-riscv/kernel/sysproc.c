#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "getpa.h"
// #include "user/user.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_forkf(void)
{
  uint64 f;
  if(argaddr(0,&f)<0)
    return -1;
  return forkf(((uint64)f));
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_getppid(void)
{
  struct proc *p = myproc();
  if(p->parent != 0){
    return p->parent->pid;
  }
  return -1;
}

uint64
sys_yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
  return 0;
}

uint64 
sys_getpa(void)
{
  struct proc *p = myproc();
  uint64 virtual_addr = p->trapframe->a0;
  return walkaddr_pa(p->pagetable, virtual_addr) + (virtual_addr & (PGSIZE - 1)); 
}

uint64 
sys_waitpid(void)
{
  int x;
  uint64 p;
  if(argint(0,&x)<0)
    return -1;
  if(argaddr(1,&p)<0)
    return -1;
  if(x==-1){
    wait(p);
  }
  else{
    waitpid(x,p);
  }
  return -1;
}