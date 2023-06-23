#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "queue.h"
#include "spinlock.h"

struct
{
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);
int pick_in_mlfq();
void priority_boosting();
int isLocked = 0;           // SCHEDULER의 LOCK 유무
int pickedProcessIndex = 0; // queue에서 뽑은 process의 인덱스 또는 현재 도는 proc_index
int noSuchPid = 1;          // 그런 PID 또 없습니다~
extern uint ticks;
void printProcessInfo(struct proc *p);
void pinit(void)
{
  initlock(&ptable.lock, "ptable");
  qinit();
}

// Must be called with interrupts disabled
int cpuid()
{
  return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *
mycpu(void)
{
  int apicid, i;

  if (readeflags() & FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i)
  {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc *
myproc(void)
{
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

// PAGEBREAK: 32
//  Look in the process table for an UNUSED proc.
//  If found, change state to EMBRYO and initialize
//  state required to run in the kernel.
//  Otherwise return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;
  char *sp;
  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->priority = 3;
  p->timequantum = 0;
  p->level = 0;
  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0)
  {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint *)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
  enqueue(0, p - ptable.proc);//L0로 넣어줍니다.
  release(&ptable.lock);
  return p;
}

// PAGEBREAK: 32
//  Set up first user process.
void userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  p = allocproc();
  initproc = p;
  if ((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0; // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if (n > 0)
  {
    if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  else if (n < 0)
  {
    if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    cprintf("error");
    return -1;
  }

  // Copy process state from proc.
  if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0)
  {
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for (fd = 0; fd < NOFILE; fd++)
  {
    if (curproc->ofile[fd])
    {
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->parent == curproc)
    {
      p->parent = initproc;
      if (p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE)
      {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); // DOC: wait-sleep
  }
}

// PAGEBREAK: 42
//  Per-CPU process scheduler.
//  Each CPU calls scheduler() after setting itself up.
//  Scheduler never returns.  It loops, doing:
//   - choose a process to run
//   - swtch to start running that process
//   - eventually that process transfers control
//       via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  for (;;)
  {
    // Enable interrupts on this processor.
    sti();
    acquire(&ptable.lock);
    if (isLocked == 0) // 잠기지 않았다면
    {
      pickedProcessIndex = pick_in_mlfq();                   // 정상적인 pick_in_mlfq()호출
      if (ptable.proc[pickedProcessIndex].state != RUNNABLE) // 다 자고있다면...
      {
        release(&ptable.lock); // 다시 for문 돌아와야 하니까 release걸면됨.
        continue;
      }
    }
    // schedulerLock이 걸려있다면 여기서부터 실행
    p = &ptable.proc[pickedProcessIndex];
    if (ptable.proc[pickedProcessIndex].state != RUNNABLE)
    {
      release(&ptable.lock);
      continue;
    }
    p->timequantum++; // tq 증가
    if (p->timequantum < (p->level * 2 + 4)) //tq가 2n+4보다 작다면
    {
      enqueue(p->level, pickedProcessIndex); // 원래 레벨로 뽑힌 인덱스 그대로 다시 넣어줌
    }
    else if (p->timequantum >= (p->level * 2 + 4)) // 2n+4보다 크다면
    {
      p->timequantum = 0; // 큐레벨에 상관없이 timequantum = 0 초기화
      if (p->level != 2)  // L0나 L1이라면
      {
        enqueue(p->level + 1, pickedProcessIndex); // 큐 레벨 하나 내려보내야함.
        p->level++; //level도 하나 증가
      }
      else if (p->level == 2) // L2레벨이라면
      {
        enqueue(2, pickedProcessIndex); //L2에 넣어준다.
        p->priority--;       // PRIORITY감소시키고
        if (p->priority < 0) // 0보다 작다면
        {
          p->priority = 0; // PRIORITY는 0으로 유지
        }
      }
    }
    //   Switch to chosen process.  It is the process's job
    //   to release ptable.lock and then reacquire it
    //   before jumping back to us.
    c->proc = p;
    switchuvm(p); // Switch TSS and h/w page table to correspond to process p.
    p->state = RUNNING;
    swtch(&(c->scheduler), p->context);
    switchkvm(); //// Switch h/w page table register to the kernel-only page table,
    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();
  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (readeflags() & FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first)
  {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock)
  {                        // DOC: sleeplock0
    acquire(&ptable.lock); // DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock)
  { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// PAGEBREAK!
//  Wake up all processes sleeping on chan.
//  The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// PAGEBREAK: 36
//  Print a process listing to console.  For debugging.
//  Runs when user types ^P on console.
//  No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [EMBRYO] "embryo",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING)
    {
      getcallerpcs((uint *)p->context->ebp + 2, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int getLevel(void)
{
  return myproc()->level;
}

void setPriority(int pid, int priority)
{
  struct proc *p;
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->priority = priority;
    }
  }
  release(&ptable.lock);
}

void schedulerLock(int password)
{
  struct proc *p = myproc();
  acquire(&ptable.lock);
  if (myproc()->state != RUNNING || isLocked == 1) // state가 RUNNING이 아니거나, 먼저 걸려있었거나
  {
    release(&ptable.lock);
    return;
  }
  if (password == 2019073181) // 비밀번호도 걸려있어야 하고 락도 안걸려있나 확인해야함.
  {
    if (cpuid() == 0)
    {
      acquire(&tickslock);
      release(&ptable.lock);
      ticks = 0;
      wakeup(&ticks);
      release(&tickslock);
      acquire(&ptable.lock);
    }
    isLocked = 1;
    release(&ptable.lock);
    return;
  }
  else
  {
    cprintf("password error cur process's pid is %d, timequantum is %d, Queue level is %d\n", p->pid, p->timequantum, p->level);
    release(&ptable.lock);
    //kill(myproc()->pid);
    exit();
  }
}

void schedulerUnlock(int password)
{
  struct proc *p = myproc();
  acquire(&ptable.lock);
  if (myproc()->state != RUNNING ||isLocked == 0)
  {
    release(&ptable.lock);
    return;
  }

  if (password == 2019073181) // 비밀번호도 맞아야 하고 락도 걸려있어야 한다.
  {
    enqueue(0, p - ptable.proc);
    p->level = 0;
    p->timequantum = 0;
    p->priority = 3;
    isLocked = 0; // scheduler 풀어줘야함.
    release(&ptable.lock);
    return;
  }
  else
  {
    cprintf("password error cur process's pid is %d, timequantum is %d, Queue level is %d\n", p->pid, p->timequantum, p->level);
    release(&ptable.lock);
    // kill(myproc()->pid); 
    exit();
  }
}

int pick_in_mlfq()
{ // 큐에서 MLFQ 를 통해 process의 !INDEX!를 뽑는다.
  // L0 큐
  int proc_index = -1; // 뽑힌 index
  int sz = mlfq[0].size; 
  for (int i = 0; i < sz; i++)//L0의 사이즈만큼
  {
    proc_index = dequeue(0); // dequeue의 파라미터는 level 즉 dequeue(0)은 L0큐에서 뽑겠다는 소리.
    if (ptable.proc[proc_index].state == RUNNABLE) //상태가 runnable이면
    {
      ptable.proc[proc_index].level = 0; //뽑힐테니까 level을 0로 설정해주고
      return proc_index; //그 인덱스 리턴
    }
    enqueue(0, proc_index); //runnable이 아니면 다시 큐에 넣어준다.
  }

  // L1 큐
  sz = mlfq[1].size;
  for (int i = 0; i < sz; i++) //L1의 사이즈만큼
  {
    proc_index = dequeue(1); // dequeue의 파라미터는 level 즉 dequeue(0)은 L0큐에서 뽑겠다는 소리.
    if (ptable.proc[proc_index].state == RUNNABLE)//runnable이면
    {
      ptable.proc[proc_index].level = 1; //레벨 다시 1로 세팅
      return proc_index;//인덱스 리턴
    }
    enqueue(1, proc_index); //runnable이 아니면 다시 L1에넣는다.
  }
  // L2 큐
  sz = mlfq[2].size;//L2의 사이즈만큼
  int minPriority = 4;//가장 작은 priority를 뽑을 예정. max값인 3보다 큰 4로 세팅
  for (int i = 0; i < mlfq[2].size; i++) // 현재 L2큐에서 RUNNABLE하면서 priority가 가장 낮은 애를 찾는 과정
  {
    proc_index = dequeue(2);//뽑고
    if (ptable.proc[proc_index].state == RUNNABLE && ptable.proc[proc_index].priority < minPriority)
    {                                // RUNNABLE하고 현재 minspriority더 낮은 애들
      minPriority = ptable.proc[proc_index].priority; // 현재 L2에서 가장 낮은 priority
    }
    enqueue(2, proc_index); //아니면 다시 넣는다. 
  }
  for (int i = 0; i < sz; i++) //다시 L2의 사이즈만큼
  { 
    proc_index = dequeue(2); //뽑고
    if (ptable.proc[proc_index].state == RUNNABLE) //runnable인 process중에서
    {
      if (ptable.proc[proc_index].priority == minPriority) //아까 뽑은 minPriority로 처음 나오는 process를 
      {
        ptable.proc[proc_index].level = 2; //레벨 2로 다시 세팅하고
        return proc_index; //proc_index 리턴
      }
    }
    enqueue(2, proc_index); //
  }
  return proc_index;
}

void priority_boosting() // PRIORITY BOOSTING
{
  struct proc *p = myproc();
  int index;
  int sz = mlfq[1].size;
  for (int i = 0; i < sz; i++) // L1 큐 사이즈만큼
  {
    index = dequeue(1); // L1에서 빼서
    enqueue(0, index);  // L0에 넣는다.
  }
  sz = mlfq[2].size;
  for (int i = 0; i < sz; i++) // L2 큐 사이즈만큼
  {
    index = dequeue(2); // L2에서 빼서
    enqueue(0, index);  // L0에 넣는다.
  }
  sz = mlfq[0].size; //L0의 사이즈만큼
  for (int i = 0; i < sz; i++)
  {
    index = dequeue(0); //빼서 인덱스를 찾고
    p = &ptable.proc[index]; //process할당하고
    p->timequantum = 0; // timequnatum 0으로 할당하고
    p->level = 0; //level도 다시 0으로 넣고
    p->priority = 3; // priority도 다시 3으로 세팅
    enqueue(0, index); // 모든 과정 거친 후 L0에 넣는다.
  }
  return;
}

void printProcessInfo(struct proc *p)
{
  // cprintf("proc name is %s, pid is %d, ptable num is %d p->level is %d\n", p->name, p->pid,pickedProcessIndex, p->level);
}