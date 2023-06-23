#include "types.h"
#include "stat.h"
#include "user.h"

#define MAX_ARGS 16
#define MAX_ARG_LEN 32

void parse_input(char *input, char **args, int *argc)
{
  int i = 0;
  int len = strlen(input);
  int arg_cnt = 0;
  int arg_len = 0;

  while (i < len && arg_cnt < MAX_ARGS - 1)
  {
    if (input[i] == ' ' || input[i] == '\r' || input[i] == '\n')
    {
      if (arg_len > 0)
      {
        args[arg_cnt] = (char *)malloc(arg_len + 1);
        memmove(args[arg_cnt], input + i - arg_len, arg_len);
        args[arg_cnt][arg_len] = '\0';
        arg_cnt++;
        arg_len = 0;
      }
    }
    else
    {
      arg_len++;
    }
    i++;
  }

  if (arg_len > 0)
  {
    args[arg_cnt] = (char *)malloc(arg_len + 1);
    memmove(args[arg_cnt], input + i - arg_len, arg_len);
    args[arg_cnt][arg_len] = '\0';
    arg_cnt++;
  }

  args[arg_cnt] = 0;
  *argc = arg_cnt;
}

int main(void)
{
  char input[1000];

  while (1)
  {
    printf(1, "$$$ pmanager cmd : "); // 쉘 프롬프트 출력

    memset(input, 0, sizeof(input)); // 입력 버퍼 초기화
    gets(input, sizeof(input));      // 사용자로부터 입력 받기

    // 입력을 인자로 구문 분석
    char *args[MAX_ARGS];
    int argc = 0;

    parse_input(input, args, &argc);

    if (strcmp(args[0], "list") == 0)
    {
      list_info();
    }
    else if (strcmp(args[0], "kill") == 0)
    {
      int pid = atoi(args[1]);
      int result = kill(pid); // kill 시스템 콜을 사용하여 프로세스 종료
      if (result == -1)
      {
        printf(1, "Failed to kill process with PID %d\n", pid);
      }
      else
      {
        printf(1, "Process with PID %d killed\n", pid);
      }
    }
    else if (strcmp(args[0], "execute") == 0)
    {
      char *path = args[1];
      int stack_size = atoi(args[2]);
      char *exec_args[2];
      exec_args[0] = path;
      exec_args[1] = 0;
      int pid = fork();
      if (pid == 0)
      {

        if (exec2(path, exec_args, stack_size) == -1)
        {
          printf(2, "Failed to execute program at %s\n", path);
          exit();
        }
        printf(1,"asdf\n");
      }
    }
    else if (strcmp(args[0], "memlim") == 0)
    {
      int pid = atoi(args[1]);
      int limit = atoi(args[2]);
      int result = setmemorylimit(pid, limit); // 사용자 정의 시스템 콜을 통해 프로세스의 메모리 제한 설정
      if (result == -1)
      {
        printf(1, "Failed to set memory limit for process with PID %d\n", pid);
      }
      else
      {
        printf(1, "Memory limit set for process with PID %d\n", pid);
      }
    }
    else if (strcmp(args[0], "exit") == 0)
    {
      exit(); // pmanager 종료
    }
    else
    {
      printf(1, "Invalid command: %s\n", args[0]);
    }
  }
}