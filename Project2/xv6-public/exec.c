#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"

int exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint argc, sz, sp, ustack[3 + MAXARG + 1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();
  struct thread *t;
  uint idx = 0;
  begin_op();
  // path인수가 유효한 파일인지 확인 아니면 -1 리턴
  if ((ip = namei(path)) == 0)
  {
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // Check ELF header //파일이 ELF 실행 파일인지 확인 그렇지 않은 경우 -1 리턴
  if (readi(ip, (char *)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if (elf.magic != ELF_MAGIC)
    goto bad;

  if ((pgdir = setupkvm()) == 0)
    goto bad;
  // Load program into memory.
  // 파일이 ELF파일이 맞을 경우, 파일을 메모리에 로드 그리고 setupkvm()을 이용해서 새 프로세스를 위한 새 가상 주소 공간을
  // 만듬. 그리고 loaduvm()함수를 사용하여 파일을 새 가상 주소 공간에 로드함.
  sz = 0;
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
  {
    if (readi(ip, (char *)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if (curproc->limit == 0) // unlimied 상태
    {
      if ((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      {
        goto bad;
      }
    }
    else if (curproc->limit != 0)
    {
      if (curproc->limit >= ph.vaddr + ph.memsz) // 할당해도 되는지 검사
      {
        if ((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
        {
          goto bad;
        }
      }
      else
      {
        goto bad;
      }
    }
    if (ph.vaddr % PGSIZE != 0)
      goto bad;
    if (loaduvm(pgdir, (char *)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;
  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.
  // 새 프로세스의 스택을 설정 / 스택에 대해 두 페이지의 메모리를 할당. 첫번째 페이지는 ACCESS불가, 두번째 페이지는 스택사용
  sz = PGROUNDUP(sz);
  if (curproc->limit == 0) // unlimied 상태
  {
    if ((sz = allocuvm(pgdir, sz, sz + 2 * PGSIZE)) == 0)
    {
      goto bad;
    }
  }
  else if (curproc->limit != 0)
  {
    if (curproc->limit >= sz + 2 * PGSIZE) // 할당해도 되는지 검사
    {
      if ((sz = allocuvm(pgdir, sz, sz + 2 * PGSIZE)) == 0)
      {
        goto bad;
      }
    }
    else
    {
      goto bad;
    }
  }
  clearpteu(pgdir, (char *)(sz - 2 * PGSIZE));
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  // 새 프로세스에 대한 인수를 스택에 푸시. exec()함수는 copyout()함수를 사용하여 인수를 사용자 공간에서 커널 공간으로 복사
  for (argc = 0; argv[argc]; argc++)
  {
    if (argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if (copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3 + argc] = sp;
  }
  ustack[3 + argc] = 0;

  ustack[0] = 0xffffffff; // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc + 1) * 4; // argv pointer

  sp -= (3 + argc + 1) * 4;
  if (copyout(pgdir, sp, ustack, (3 + argc + 1) * 4) < 0)
    goto bad;

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Commit to the user image.
  // exec함수는 프로그램 카운터를 새 프로세스의 main함수의 주소로 설정.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;
  curproc->tf->eip = elf.entry; // main
  curproc->tf->esp = sp;
  switchuvm(curproc);
  if (curproc->curThrIdx != 0) // 현재 돌고있는 번호가 0번(메인 쓰레드)이 아닌 그냥 일반 쓰레드일 때
  {
    curproc->thrlist.thread[curproc->curThrIdx].execVisited = 1;                        // execVisited를 1로 설정
    curproc->thrlist.thread[0].chan = curproc->thrlist.thread[curproc->curThrIdx].chan; // chan, retval, thr_state 변경
    curproc->thrlist.thread[0].retval = curproc->thrlist.thread[curproc->curThrIdx].retval;
    curproc->thrlist.thread[0].thr_state = curproc->thrlist.thread[curproc->curThrIdx].thr_state;
  }
  // kill part
  for (idx = 0, t = curproc->thrlist.thread; t < &curproc->thrlist.thread[NTHR]; idx++, t++)
  {
    if (idx == curproc->curThrIdx || idx == 0) // idx가 0(메인쓰레드)거나 현재 쓰레드 번호일 때는 정보 남겨둔다.
      continue;
    if (t->kstack != 0)
    {
      kfree(t->kstack);
    }

    t->kstack = 0;
    t->context = 0;
    t->thr_state = THR_UNUSED;
    t->tid = 0;
    t->chan = 0;
    t->tf = 0;
  }
  freevm(oldpgdir);
  return 0;

bad:
  if (pgdir)
    freevm(pgdir);
  if (ip)
  {
    iunlockput(ip);
    end_op();
  }
  return -1;
}

int exec2(char *path, char **argv, int stacksize)
{
  char *s, *last;
  int i, off;
  uint argc, sz, sp, ustack[3 + MAXARG + 1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();

  begin_op();

  if ((ip = namei(path)) == 0)
  {
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  if (stacksize < 1 || stacksize > 100)
  { // 스택사이즈 1과 100사이로 한정
    cprintf("exec: fail invalid stacksize\n");
    return -1;
  }

  // Check ELF header
  if (readi(ip, (char *)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if (elf.magic != ELF_MAGIC)
    goto bad;

  if ((pgdir = setupkvm()) == 0)
    goto bad;

  // Load program into memory.
  sz = 0;
  for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
  {
    if (readi(ip, (char *)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz)
      goto bad;
    if (ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if (curproc->limit == 0) // unlimied 상태
    {
      if ((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      {
        goto bad;
      }
    }
    else if (curproc->limit != 0)
    {
      if (curproc->limit >= ph.vaddr + ph.memsz) // 할당해도 되는지 검사
      {
        if ((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
        {
          goto bad;
        }
      }
      else
      {
        goto bad;
      }
    }
    if (ph.vaddr % PGSIZE != 0)
      goto bad;
    if (loaduvm(pgdir, (char *)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible. Use the second as the user stack.
  sz = PGROUNDUP(sz);
  if (curproc->limit == 0) // unlimied 상태
  {
    if ((sz = allocuvm(pgdir, sz, sz + (stacksize + 1) * PGSIZE)) == 0)
    {
      goto bad;
    }
  }
  else if (curproc->limit != 0)
  {
    if (curproc->limit >= sz + (stacksize+1) * PGSIZE) // 할당해도 되는지 검사
    {
      if ((sz = allocuvm(pgdir, sz, sz + (stacksize + 1) * PGSIZE)) == 0)
      {
        goto bad;
      }
    }
    else
    {
      goto bad;
    }
  }
  clearpteu(pgdir, (char *)(sz - (stacksize + 1) * PGSIZE));
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  for (argc = 0; argv[argc]; argc++)
  {
    if (argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3; // 4의 배수로 정렬 ~3이 11111100임->&하면 마지막 2개 다 0
    if (copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3 + argc] = sp;
  }
  ustack[3 + argc] = 0;

  ustack[0] = 0xffffffff; // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc + 1) * 4; // argv pointer

  sp -= (3 + argc + 1) * 4;
  if (copyout(pgdir, sp, ustack, (3 + argc + 1) * 4) < 0)
    goto bad;

  // Save program name for debugging.
  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Commit to the user image.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;
  curproc->tf->eip = elf.entry; // main
  curproc->tf->esp = sp;
  switchuvm(curproc);
  freevm(oldpgdir);
  return 0;

bad:
  if (pgdir)
    freevm(pgdir);
  if (ip)
  {
    iunlockput(ip);
    end_op();
  }
  return -1;
}
