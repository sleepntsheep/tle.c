#include "common.h"
#include <sys/wait.h>
#include <signal.h>

static void
close_all(int fd[2][2])
{
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j)
      close(fd[i][j]);
}

static void
kill_if_running(pid_t pid)
{
  if (pid > 0)
    kill(pid, SIGKILL);
}

int
main(int argc, char **argv)
{
  /* work-dir manager time-limit-ms memory-limit-kb */
  if (argc != 5)
  {
    fprintf(stderr, "usage: %s WORK_DIR MANAGER TIME_LIMIT_MS MEMORY_LIMIT_KB\n", argv[0]);
    return 2;
  }

  char bind_arg[1400], time_arg[64], wall_arg[64], memory_arg[64];
  int pipes[2][2];
  pid_t manager, contestant;
  int manager_status = 1, contestant_status = 1;
  unsigned long time_ms = strtoul(argv[3], NULL, 10);
  unsigned long memory_kb = strtoul(argv[4], NULL, 10);
  struct timespec start, now;
  long long elapsed_ms;

  if (time_ms == 0 || memory_kb == 0 ||
      strlen(argv[1]) >= sizeof bind_arg || strlen(argv[2]) >= PATH_MAX)
    return 2;
  if (pipe(pipes[0]) != 0 || pipe(pipes[1]) != 0)
    return 2;

  snprintf(bind_arg, sizeof bind_arg, "--dir=/work=%s:rw", argv[1]);
  snprintf(time_arg, sizeof time_arg, "--time=%.3f", time_ms / 1000.0);
  snprintf(wall_arg, sizeof wall_arg, "--wall-time=%.3f", (time_ms + 2000) / 1000.0);
  snprintf(memory_arg, sizeof memory_arg, "--mem=%lu", memory_kb);

  manager = fork();
  if (manager < 0)
  {
    close_all(pipes);
    return 2;
  }
  if (manager == 0)
  {
    dup2(pipes[1][0], STDIN_FILENO);
    dup2(pipes[0][1], STDOUT_FILENO);
    close_all(pipes);
    execl(argv[2], argv[2], NULL);
    _exit(127);
  }

  contestant = fork();
  if (contestant < 0)
  {
    kill_if_running(manager);
    close_all(pipes);
    waitpid(manager, NULL, 0);
    return 2;
  }
  if (contestant == 0)
  {
    dup2(pipes[0][0], STDIN_FILENO);
    dup2(pipes[1][1], STDOUT_FILENO);
    close_all(pipes);
    execl(ISOLATE_PATH, "isolate", bind_arg, "--chdir=/work",
        "--inherit-fds", memory_arg, time_arg, wall_arg, "--tty-hack",
        "--run", "--", "./exec", NULL);
    _exit(127);
  }
  close_all(pipes);
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (;;)
  {
    pid_t manager_done = waitpid(manager, &manager_status, WNOHANG);
    pid_t contestant_done = waitpid(contestant, &contestant_status, WNOHANG);
    if (manager_done == manager && contestant_done == contestant)
      break;
    if (manager_done < 0 || contestant_done < 0)
    {
      kill_if_running(manager);
      kill_if_running(contestant);
      waitpid(manager, NULL, 0);
      waitpid(contestant, NULL, 0);
      return 2;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed_ms = (long long)(now.tv_sec - start.tv_sec) * 1000 +
        (now.tv_nsec - start.tv_nsec) / 1000000;
    if (elapsed_ms > (long long)time_ms + 5000)
    {
      kill_if_running(manager);
      kill_if_running(contestant);
      waitpid(manager, NULL, 0);
      waitpid(contestant, NULL, 0);
      return 1;
    }
    usleep(10000);
  }
  return WIFEXITED(manager_status) && WEXITSTATUS(manager_status) == 0 &&
      WIFEXITED(contestant_status) && WEXITSTATUS(contestant_status) == 0 ? 0 : 1;
}
