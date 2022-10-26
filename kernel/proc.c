#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "procstat.h"
#include "param.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  uint xticks;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->priority = -1000000;
  p->next_burst_len = 0;
  p->batch_process = 0;
  p->cpu_usage = 0;
  p->prev_cpu_usage = 0;
  p->wait_time = 0;
  p->wait_st_time = -1;
  p->prev_burst_start = -1;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);

  p->ctime = xticks;
  p->stime = -1;
  p->endtime = -1;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  int cticks;
  if (!holding(&tickslock)) {
    acquire(&tickslock);
    cticks = ticks;
    release(&tickslock);
  }
  else cticks = ticks;
  p->wait_st_time = cticks;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;

  int cticks;
  if (!holding(&tickslock)) {
    acquire(&tickslock);
    cticks = ticks;
    release(&tickslock);
  }
  else cticks = ticks;
  np->wait_st_time = cticks;

  release(&np->lock);

  return pid;
}

int
forkp(int priority)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;
  np->priority = priority;
  np->batch_process = 1;
  mycpu()->nump += 1;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;

  int cticks;
  if (!holding(&tickslock)) {
    acquire(&tickslock);
    cticks = ticks;
    release(&tickslock);
  }
  else cticks = ticks;
  np->wait_st_time = cticks;

  release(&np->lock);

  return pid;
}

int
forkf(uint64 faddr)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;
  // Make child to jump to function
  np->trapframe->epc = faddr;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;

  int cticks;
  if (!holding(&tickslock)) {
    acquire(&tickslock);
    cticks = ticks;
    release(&tickslock);
  }
  else cticks = ticks;
  np->wait_st_time = cticks;

  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();
  int xticks;

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);

  p->endtime = xticks;

  struct cpu *c = mycpu();
  if(p->batch_process)
  {
    c->comp += 1;
    c->tatime += (p->endtime - p->ctime);
    c->wtime += p->wait_time;
    c->ctime += p->endtime;
    c->max_ctime = c->max_ctime < p->endtime ? p->endtime : c->max_ctime;
    c->min_ctime = c->min_ctime > p->endtime ? p->endtime : c->min_ctime;
    if(c->sched_policy == 1) {
      int t = p->endtime - p->prev_burst_start;
      int err = t - p->next_burst_len;
      err = err > 0 ? err : (0-err);
      if(t > 0) {
        c->nbursts += 1;
        c->max_blen = c->max_blen < t ? t : c->max_blen;
        c->tblen += t;
        c->min_blen = c->min_blen > t ? t : c->min_blen;
      }
      if(err != 0 && t > 0 && p->next_burst_len > 0) {
        c->ebursts += 1;
        c->tebursts += err;
      }
    }
  }

  if(c->comp && c->comp == c->nump) {
    printf("Batch execution time: %d\n", (xticks - c->stime));
    printf("Average turn-around time: %d\n", c->tatime / c->nump);
    printf("Average waiting time: %d\n", c->wtime / c->nump);
    printf("Completion time: avg: %d, max: %d, min: %d\n", c->ctime / c->nump, c->max_ctime, c->min_ctime);
    c->comp = 0;
    c->nump = 0;
    c->stime = -1;
    c->tatime = 0;
    c->wtime = 0;
    c->ctime = 0;
    c->max_ctime = 0;
    c->min_ctime = 1000000000;

    if(c->sched_policy == 1) {
      printf("CPU bursts: count: %d, avg: %d, max: %d, min: %d\n", c->nbursts, c->tblen / c->nbursts, c->max_blen, c->min_blen);
      printf("CPU burst estimates: count: %d, avg: %d, max: %d, min: %d\n", c->nebursts, c->teblen / c->nebursts, c->max_belen, c->min_belen);
      printf("CPU burst estimation error: count: %d, avg: %d\n", c->ebursts, c->tebursts / c->ebursts);
      c->nbursts = 0;
      c->tblen = 0;
      c->max_blen = 0;
      c->min_blen = 1000000000;
      c->nebursts = 0;
      c->teblen = 0;
      c->max_belen = 0;
      c->min_belen = 1000000000;
      c->ebursts = 0;
      c->tebursts = 0;
    }
  }

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

int
waitpid(int pid, uint64 addr)
{
  struct proc *np;
  struct proc *p = myproc();
  int found=0;

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for child with pid
    for(np = proc; np < &proc[NPROC]; np++){
      if((np->parent == p) && (np->pid == pid)){
	found = 1;
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        if(np->state == ZOMBIE){
           if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
             release(&np->lock);
             release(&wait_lock);
             return -1;
           }
           freeproc(np);
           release(&np->lock);
           release(&wait_lock);
           return pid;
	}

        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!found || p->killed){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    if(c->sched_policy == SCHED_NPREEMPT_FCFS || c->sched_policy == SCHED_PREEMPT_RR)
    {
      for(p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if(p->state == RUNNABLE) {
          // Switch to chosen process.  It is the process's job
          // to release its lock and then reacquire it
          // before jumping back to us.
          p->state = RUNNING;
          c->proc = p;
          int curr_ticks;
          if (!holding(&tickslock)) {
            acquire(&tickslock);
            curr_ticks = ticks;
            release(&tickslock);
          }
          else curr_ticks = ticks;

          p->wait_time += (curr_ticks - p->wait_st_time);
          p->wait_st_time = -1;
          swtch(&c->context, &p->context);

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
        }
        release(&p->lock);

        if(c->sched_policy != SCHED_NPREEMPT_FCFS && c->sched_policy != SCHED_PREEMPT_RR)
          break;
      }
    }

    else
    if (c->sched_policy == SCHED_NPREEMPT_SJF)
    {
      int min_burst_len = -1;
      struct proc *p_to_sched = 0;
      int flag = 0;
      int found = 0;

      for(p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if(p->state == RUNNABLE) {
          if(p->batch_process == 0){
            p->state = RUNNING;
            c->proc = p;
            swtch(&c->context, &p->context);

            c->proc = 0;
            release(&p->lock);
            flag = 1;
            break;
          }

          else {
            found = 1;
            if(min_burst_len == -1){
              min_burst_len = p->next_burst_len;
              p_to_sched = p;
            }
            else
            if(min_burst_len > p->next_burst_len) {
              min_burst_len = p->next_burst_len;
              p_to_sched = p;
            }
          }
        }
        release(&p->lock); // ye bhi check karna am I doing it correctly

        if(c->sched_policy != SCHED_NPREEMPT_SJF)
        {
          flag = 1;
          break;
        }
      }
      if(flag || found == 0)
        continue;

      acquire(&p_to_sched->lock);

      int curr_ticks;
      if (!holding(&tickslock)) {
        acquire(&tickslock);
        curr_ticks = ticks;
        release(&tickslock);
      }
      else curr_ticks = ticks;

      p_to_sched->wait_time += (curr_ticks - p_to_sched->wait_st_time);
      p_to_sched->wait_st_time = -1;

      p_to_sched->state = RUNNING;
      c->proc = p_to_sched;

      int sticks;
      if (!holding(&tickslock)) {
        acquire(&tickslock);
        sticks = ticks;
        release(&tickslock);
      }
      else sticks = ticks;

      p_to_sched->prev_burst_start = sticks;

      swtch(&c->context, &p_to_sched->context);

      int eticks;
      if (!holding(&tickslock)) {
        acquire(&tickslock);
        eticks = ticks;
        release(&tickslock);
      }
      else eticks = ticks;
      
      int t = eticks - sticks;
      int err = t - p_to_sched->next_burst_len;
      err = err > 0 ? err : (0-err);
      if(err > 0 && p_to_sched->state != ZOMBIE && t > 0 && p_to_sched->next_burst_len > 0) {
        c->tebursts += err;
        c->ebursts += 1;
      }

      int next_burst_len = t - (SCHED_PARAM_SJF_A_NUMER * t) / SCHED_PARAM_SJF_A_DENOM + 
                              (SCHED_PARAM_SJF_A_NUMER * p_to_sched->next_burst_len) / SCHED_PARAM_SJF_A_DENOM;
      p_to_sched->next_burst_len = next_burst_len;

      if(t != 0 && p_to_sched->state != ZOMBIE) {
        c->nbursts += 1;
        c->max_blen = c->max_blen < t ? t : c->max_blen;
        c->tblen += t;
        c->min_blen = c->min_blen > t ? t : c->min_blen;
      }

      if(p_to_sched->next_burst_len != 0 && p_to_sched->state != ZOMBIE) {
        t = p_to_sched->next_burst_len;
        c->nebursts += 1;
        c->max_belen = c->max_belen < t ? t : c->max_belen;
        c->teblen += t;
        c->min_belen = c->min_belen > t ? t : c->min_belen;
      }

      c->proc = 0;
      release(&p_to_sched->lock);
    }

    else
    if (c->sched_policy == SCHED_PREEMPT_UNIX) {
      int min_priority = -1;
      int min_wait_time = -1;
      struct proc *p_to_sched = 0;
      int flag = 0;
      int found = 0;

      int curr_ticks = 0;
      if (!holding(&tickslock)) {
        acquire(&tickslock);
        curr_ticks = ticks;
        release(&tickslock);
      }
      else curr_ticks = ticks;

      for(p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if(p->state == RUNNABLE) {
          if(p->batch_process == 0){
            p->state = RUNNING;
            c->proc = p;
            swtch(&c->context, &p->context);

            c->proc = 0;
            release(&p->lock);
            flag = 1;
            break;
          }

          else {
            found = 1;
            if(p->cpu_usage != p->prev_cpu_usage) {
              p->cpu_usage /= 2;
              p->prev_cpu_usage = p->cpu_usage;
            }

            int priority = p->priority + (p->cpu_usage / 2);
            int waiting_time = (curr_ticks - p->wait_st_time) + p->wait_time;
            if(min_priority == -1) {
              min_priority = priority;
              p_to_sched = p;
              min_wait_time = waiting_time;
            }
            else
            if(min_priority > priority)
            {
              min_priority = priority;
              p_to_sched = p;
              min_wait_time = waiting_time;
            }
            else
            if(min_priority == priority && min_wait_time < waiting_time){
              min_priority = priority;
              p_to_sched = p;
              min_wait_time = waiting_time;
            }
          }
        }
        release(&p->lock);

        if(c->sched_policy != SCHED_PREEMPT_UNIX)
        {
          flag = 1;
          break;
        }
      }

      if(flag || found == 0)
        continue;

      acquire(&p_to_sched->lock);

      if (!holding(&tickslock)) {
        acquire(&tickslock);
        curr_ticks = ticks;
        release(&tickslock);
      }
      else curr_ticks = ticks;

      p_to_sched->wait_time += (curr_ticks - p_to_sched->wait_st_time);
      p_to_sched->wait_st_time = -1;

      p_to_sched->state = RUNNING;
      c->proc = p_to_sched;
      swtch(&c->context, &p_to_sched->context);
      c->proc = 0;
      release(&p_to_sched->lock);
    }

    else {
      panic("Scheduling policy not found");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  p->cpu_usage += SCHED_PARAM_CPU_USAGE;

  int cticks;
  if (!holding(&tickslock)) {
    acquire(&tickslock);
    cticks = ticks;
    release(&tickslock);
  }
  else cticks = ticks;
  p->wait_st_time = cticks;

  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;
  uint xticks;
  int first_batch = 0;
  if(myproc()->batch_process)
  {
    if(mycpu()->stime == -1)
      first_batch = 1;
  }

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);

  myproc()->stime = xticks;
  if(first_batch) {
    mycpu()->stime = xticks;
  }

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  p->cpu_usage += SCHED_PARAM_CPU_USAGE / 2;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;

        int cticks;
        if (!holding(&tickslock)) {
          acquire(&tickslock);
          cticks = ticks;
          release(&tickslock);
        }
        else cticks = ticks;
        p->wait_st_time = cticks;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;

        int cticks;
        if (!holding(&tickslock)) {
          acquire(&tickslock);
          cticks = ticks;
          release(&tickslock);
        }
        else cticks = ticks;
        p->wait_st_time = cticks;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

// Print a process listing to console with proper locks held.
// Caution: don't invoke too often; can slow down the machine.
int
ps(void)
{
   static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep",
  [RUNNABLE]  "runble",
  [RUNNING]   "run",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;
  int ppid, pid;
  uint xticks;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == UNUSED) {
      release(&p->lock);
      continue;
    }
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

    pid = p->pid;
    release(&p->lock);
    acquire(&wait_lock);
    if (p->parent) {
       acquire(&p->parent->lock);
       ppid = p->parent->pid;
       release(&p->parent->lock);
    }
    else ppid = -1;
    release(&wait_lock);

    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);

    printf("pid=%d, ppid=%d, state=%s, cmd=%s, ctime=%d, stime=%d, etime=%d, size=%p", pid, ppid, state, p->name, p->ctime, p->stime, (p->endtime == -1) ? xticks-p->stime : p->endtime-p->stime, p->sz);
    printf("\n");
  }
  return 0;
}

int
pinfo(int pid, uint64 addr)
{
   struct procstat pstat;

   static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep",
  [RUNNABLE]  "runble",
  [RUNNING]   "run",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;
  uint xticks;
  int found=0;

  if (pid == -1) {
     p = myproc();
     acquire(&p->lock);
     found=1;
  }
  else {
     for(p = proc; p < &proc[NPROC]; p++){
       acquire(&p->lock);
       if((p->state == UNUSED) || (p->pid != pid)) {
         release(&p->lock);
         continue;
       }
       else {
         found=1;
         break;
       }
     }
  }
  if (found) {
     if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
         state = states[p->state];
     else
         state = "???";

     pstat.pid = p->pid;
     release(&p->lock);
     acquire(&wait_lock);
     if (p->parent) {
        acquire(&p->parent->lock);
        pstat.ppid = p->parent->pid;
        release(&p->parent->lock);
     }
     else pstat.ppid = -1;
     release(&wait_lock);

     acquire(&tickslock);
     xticks = ticks;
     release(&tickslock);

     safestrcpy(&pstat.state[0], state, strlen(state)+1);
     safestrcpy(&pstat.command[0], &p->name[0], sizeof(p->name));
     pstat.ctime = p->ctime;
     pstat.stime = p->stime;
     pstat.etime = (p->endtime == -1) ? xticks-p->stime : p->endtime-p->stime;
     pstat.size = p->sz;
     if(copyout(myproc()->pagetable, addr, (char *)&pstat, sizeof(pstat)) < 0) return -1;
     return 0;
  }
  else return -1;
}

int schedpolicy(int policy)
{
  struct cpu *c = mycpu();
  int prev_policy = c -> sched_policy;
  c -> sched_policy = policy;
  return prev_policy;
}
