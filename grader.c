#include "common.h"
#include <sys/wait.h>

/* prototypes */
int grade(unsigned, const char *, unsigned *, unsigned *, double *, char **, char **);
void cleanup(int);
int safe_tkcmp(const char *, const char *);

static int
run_isolate_cleanup(void)
{
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0)
  {
    execl(ISOLATE_PATH, "isolate", "--cleanup", NULL);
    _exit(127);
  }
  int status;
  if (waitpid(pid, &status, 0) < 0)
    return -1;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int
isolate_init_box(char *box_path, size_t box_path_size)
{
  int pipe_fd[2];
  pid_t pid;
  if (pipe(pipe_fd) != 0)
    return -1;
  pid = fork();
  if (pid < 0)
  {
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    return -1;
  }
  if (pid == 0)
  {
    close(pipe_fd[0]);
    if (dup2(pipe_fd[1], STDOUT_FILENO) < 0)
      _exit(127);
    close(pipe_fd[1]);
    execl(ISOLATE_PATH, "isolate", "--init", NULL);
    _exit(127);
  }
  close(pipe_fd[1]);
  FILE *output = fdopen(pipe_fd[0], "r");
  if (!output)
  {
    close(pipe_fd[0]);
    return -1;
  }
  int read_ok = fgets(box_path, (int)box_path_size, output) != NULL;
  fclose(output);
  int status;
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    read_ok = 0;
  if (!read_ok)
    return -1;
  box_path[strcspn(box_path, "\n")] = '\0';
  return 0;
}

static double
score_for_failed_case(unsigned task_pk, unsigned failed_case, double max_score)
{
  struct subtask_group *groups = NULL;
  size_t count = 0;
  double score = 0.0;
  if (fetch_subtask_groups(task_pk, &groups, &count) != 0)
    return 0.0;
  if (count == 0)
  {
    struct task task_data;
    double uniform_score = 0.0;
    if (fetch_task_by_pk(task_pk, &task_data) == 0 && task_data.count_cases > 0 && failed_case > 0)
      uniform_score = max_score * (double)(failed_case - 1) / (double)task_data.count_cases;
    free_subtask_groups(groups);
    return uniform_score;
  }
  for (size_t i = 0; i < count; ++i)
  {
    unsigned last_case = groups[i].case_start + groups[i].case_count - 1;
    if (failed_case > last_case)
      score += groups[i].score;
  }
  free_subtask_groups(groups);
  return score > max_score ? max_score : score;
}

static int
has_subtask_groups(unsigned task_pk)
{
  struct subtask_group *groups = NULL;
  size_t count = 0;
  int has_groups;
  if (fetch_subtask_groups(task_pk, &groups, &count) != 0)
    return 0;
  has_groups = count != 0;
  free_subtask_groups(groups);
  return has_groups;
}

/* variables */
struct task task;

int
main (int argc, char **argv)
{
  if (argc >= 2 && !strcmp(argv[1], "--check-runtime"))
  {
    if (access(ISOLATE_PATH, X_OK) != 0)
      die("isolate is not executable at %s", ISOLATE_PATH);
    if (access(TASKS_PATH, R_OK | X_OK) != 0)
      die("tasks path is not readable: %s", TASKS_PATH);
    char box_path[256];
    if (isolate_init_box(box_path, sizeof box_path) != 0)
      die("isolate could not initialize a sandbox; check its setuid/configuration");
    if (run_isolate_cleanup() != 0)
      die("isolate sandbox cleanup failed");
    info("grader runtime checks passed");
    return 0;
  }
  if (SIG_ERR == signal(SIGINT, cleanup))
    die("error registering signal handler");
  if (SIG_ERR == signal(SIGTERM, cleanup))
    die("error registering signal handler");

  db_init();
  db_lock();

  info("grader init done");

  for (;; usleep(1000000))
  {
    struct submission_job job = { 0 };
    unsigned time_used = 0, memory_used = 0;
    int rc;
    char *result = NULL;
    char *compiler_output = NULL;
    double score = 0.0;

    rc = claim_submission(&job);
    if (rc < 0)
    {
      warn("database queue claim failed");
      continue;
    }
    if (rc == 0)
      continue;

    info("grading submission %u for task %u", job.submission_id, job.task_pk);

    rc = grade(job.task_pk, job.code, &time_used, &memory_used, &score,
        &result, &compiler_output);
    if (rc != 0)
    {
      free(result);
      result = strdup("internal error: grading failed");
    }

    if (complete_submission(job.submission_id, result, time_used, memory_used,
          score, compiler_output, job.claim_token) != 0)
      warn("submission %u could not be marked finished", job.submission_id);

    free(result);
    free(compiler_output);
    free_submission_job(&job);
  }

  die("unreachable!");
}


int
compile (const char *code, const char *box_path, unsigned memory_limit)
{
  char source_path[600];
  char meta_path[600];
  char mem_option[64];
  FILE *source;
  pid_t pid;

  snprintf(source_path, sizeof source_path, "%s/submission.cpp", box_path);
  snprintf(meta_path, sizeof meta_path, "%s/compile.stat", box_path);
  source = fopen(source_path, "w");
  if (!source)
    return 1;
  if (fwrite(code, 1, strlen(code), source) != strlen(code))
  {
    fclose(source);
    return 1;
  }
  fclose(source);
  snprintf(mem_option, sizeof mem_option, "--mem=%u",
      memory_limit < 262144 ? 262144 : memory_limit);

  expect ((pid = fork ()) != -1);

  if (pid == 0)                 /* child */
  {
    char meta_option[640];
    snprintf(meta_option, sizeof meta_option, "--meta=%s", meta_path);
    execl (ISOLATE_PATH, "isolate", meta_option, mem_option,
        "--stack=262144", "--time=10", "--stdin=./submission.cpp",
        "--stdout=./compile.out", "--stderr=./compile.err", "--run", "--",
        "/usr/bin/g++", "-O1", "-std=c++17", "-o", "./exec", "-xc++",
        "./submission.cpp", NULL);
    _exit(127);
  }
  else if (pid > 0)             /* parent */
  {
    int status;
    if (waitpid (pid, &status, 0) < 0)
      return 1;

    if (WIFEXITED (status) && WEXITSTATUS (status) == 0)
      return 0;
    else
      return 1;
  }
  return 0;
}

  int
compile_path (const char *path, const char *out)
{
  pid_t pid;

  expect ((pid = fork ()) != -1);

  if (pid == 0)                 /* child */
  {
    fflush(stdout);
    execl ("/usr/bin/g++", "g++", path, "-O1", "-std=c++17", "-o", out, "-xc++", NULL);
    _exit(127);
  }
  else if (pid > 0)             /* parent */
  {
    int status;
    waitpid (pid, &status, 0);

    return !WIFEXITED (status) || WEXITSTATUS (status) != 0;
  }
  return -1;
}


int copy(char *source, char *dest)
{
  FILE *fp1, *fp2;
  int a;

  fp1 = fopen(source, "r");
  if (fp1 == NULL)
    return -1;

  fp2 = fopen(dest, "w");
  if (fp2 == NULL)
  {
    fclose(fp1);
    return -1;
  }

  while( (a = fgetc(fp1)) != EOF )
    fputc(a, fp2);

  fclose(fp1);
  fclose(fp2);
  return 0;
}

  int
grade(unsigned task_pk, const char *code, unsigned *time_used, unsigned *memory_used,
    double *score, char **result, char **compiler_output)
{
  if (run_isolate_cleanup() != 0)
    warn("isolate cleanup failed before grading");

  *result = strdup("grading");
  *compiler_output = NULL;

  static struct task task;
  char task_path[9999], box_path[256];

  if (0 != fetch_task_by_pk(task_pk, &task))
    return -1;

  snprintf(task_path, sizeof(task_path), "%s/%s", TASKS_PATH, task.name);

  if (isolate_init_box(box_path, sizeof box_path) != 0)
  {
    warn("failed to initialize isolate sandbox");
    return -1;
  }
  if (strlen(box_path) + sizeof "/box" > sizeof box_path)
  {
    return -1;
  }
  strcat(box_path, "/box");

  if (0 != compile(code, box_path, task.memory_limit))
  {
    char diagnostic_path[600];
    int diagnostic_size = 0;
    char *diagnostic;

    snprintf(diagnostic_path, sizeof diagnostic_path, "%s/compile.err", box_path);
    diagnostic = read_file(diagnostic_path, &diagnostic_size);
    if (diagnostic)
      *compiler_output = diagnostic;
    free(*result);
    *result = strdup("compilation error");
    *score = 0.0;
    *time_used = 0;
    *memory_used = 0;
    return 0;
  }

  unsigned max_time = 0, max_mem = 0;
  double total_score = 0.0;
  sds raw_results = NULL;
  sds first_failure = NULL;
  unsigned passed_cases = 0;
  int uniform_scoring = !has_subtask_groups(task_pk);

  for (unsigned case_id = 1; case_id <= task.count_cases; ++case_id)
  {
    sds sysinput, sysoutput, /* absolute path to testcase input / output */
        boxsysinput, boxsuboutput, /* path of those previous two files copied to isolate's environment */
        relboxsysinput, relboxsuboutput, /* relative path of previous two files, relative to box's environment */
        stat_file,
        suboutput;
        /* submission's output file */
    FILE *stat_fp;
    int subexitcode;
    int subexitsig;

    stat_fp         = NULL;
    subexitcode     = 0;
    subexitsig      = 0;

    sysinput        = sdscatprintf(sdsempty(), "%s/tests/%u.in", task_path, case_id);
    sysoutput       = sdscatprintf(sdsempty(), "%s/tests/%u.sol", task_path, case_id);
    boxsysinput     = sdscatprintf(sdsempty(), "%s/%u.in", box_path, case_id);
    boxsuboutput    = sdscatprintf(sdsempty(), "%s/%u.out", box_path, case_id);
    relboxsysinput  = sdscatprintf(sdsempty(), "./%u.in", case_id);
    relboxsuboutput = sdscatprintf(sdsempty(), "./%u.out", case_id);
    stat_file       = sdscatprintf(sdsempty(), "/tmp/%u.stat", case_id);
    suboutput       = sdscatprintf(sdsempty(), "/tmp/%u.out", case_id);

    copy(sysinput, boxsysinput);

    /* run isolated submission code */
    {
      pid_t pid;

      pid = fork();

      if (0 == pid) /* child */
      {
        execl(ISOLATE_PATH,
            "isolate",
            sdscatprintf(sdsempty(), "--meta=%s", stat_file),
            sdscatprintf(sdsempty(), "--mem=%u", task.memory_limit),
            sdscatprintf(sdsempty(), "--stack=%u", task.memory_limit),
            sdscatprintf(sdsempty(), "--time=%f", task.time_limit / 1000.0f),
            sdscatprintf(sdsempty(), "--stdin=%s", relboxsysinput),
            sdscatprintf(sdsempty(), "--stdout=%s", relboxsuboutput),
            "--run",
            "--",
            "./exec",
            NULL);
        _exit(127);
      }
      else if (0 < pid) /* parent */
      {
        int status;
        waitpid (pid, &status, 0);

        if (WIFEXITED (status) && 0 == WEXITSTATUS (status))
          ;
        else
        {
          ;
        }
      }
      else
        diep("fork failed");
    }

    copy(boxsuboutput, suboutput);

    if (!(stat_fp = fopen(stat_file, "r")))
    {
      raw_results = sdsnew("internalerror: failed opening stat");
      goto end_case;
    }

    char line[256], stat[9] = { 0 };
    unsigned case_time = 0, case_mem = 0;
    while (fgets(line, sizeof(line), stat_fp))
    {
      if (0 == strncmp(line, "time:", 5))
        case_time = (unsigned)(atof(line + 5) * 1000);
      else if (0 == strncmp(line, "status:", 7))
      {
        if (1 != sscanf(line + 7, "%s", stat))
          warn("failed reading status: at %s", line);
      }
      else if (strncmp(line, "max-rss:", 8) == 0)
        case_mem = atoi(line + 8);
      else if (strncmp(line, "exitsig:", 8) == 0)
        subexitsig = atoi(line + 8);
      else if (strncmp(line, "exitcode:", 9) == 0)
      {
        if (1 != sscanf(line + 9, "%d", &subexitcode))
          warn("failed reading exitcode at %s", line);
      }
    }

    sysinput        = sdscatprintf(sdsempty(), "%s/tests/%u.in", task_path, case_id);
    sysoutput       = sdscatprintf(sdsempty(), "%s/tests/%u.sol", task_path, case_id);
    boxsysinput     = sdscatprintf(sdsempty(), "%s/%u.in", box_path, case_id);
    boxsuboutput    = sdscatprintf(sdsempty(), "%s/%u.out", box_path, case_id);
    relboxsysinput  = sdscatprintf(sdsempty(), "./%u.in", case_id);
    relboxsuboutput = sdscatprintf(sdsempty(), "./%u.out", case_id);
    stat_file       = sdscatprintf(sdsempty(), "/tmp/%u.stat", case_id);
    suboutput       = sdscatprintf(sdsempty(), "/tmp/%u.out", case_id);



    fclose(stat_fp); stat_fp = NULL;

    max_time = case_time > max_time ? case_time : max_time;
    max_mem = case_mem > max_mem ? case_mem : max_mem;

    if (0 == strcmp("XX", stat))
    {
      raw_results = sdsnew("internal error:isolate crashed");
      goto end_case;
    }
    else if (0 == strcmp("TO", stat) || max_time > task.time_limit)
    {
      raw_results = sdscatprintf(sdsempty(), "time limit exceeded#%u", case_id);
      goto end_case;
    }
    else if (0 == strcmp("XX", stat))
    {
      raw_results = sdscatprintf(sdsempty(), "internal server error#%u", case_id);
      goto end_case;
    }
    else if (0 == strcmp("SG", stat))
    {
      raw_results = sdscatprintf(sdsempty(), "signal(%d)#%u", subexitsig, case_id);
      goto end_case;
    }
    else if (0 != subexitcode)
    {
      raw_results = sdscatprintf(sdsempty(), "exitcode(%d)#%u", subexitcode, case_id);
      goto end_case;
    }
    else if (stat[0] && max_mem > task.memory_limit)
    {
      raw_results = sdscatprintf(sdsempty(), "memory limit exceeded#%u", case_id);
      goto end_case;
    }
    else
    {
      if (0 == strcmp(task.comparison, "special"))
      {
        int fildes[2];
        pid_t pid;
        sds jury_path;

        expect (pipe (fildes) == 0);
        pid = fork();
        jury_path = sdscatprintf(sdsempty(), "%s/jury", task_path);

        if (0 != access(jury_path, F_OK))
        {
          sds jury_code_path;
          jury_code_path = sdscat(sdsdup(jury_path), ".cpp");
          if (0 != access(jury_code_path, F_OK))
          {
            raw_results = sdsnew("internal error:jury not present");
            sdsfree(jury_code_path);
            goto end_case;
          }
          if (0 != compile_path(jury_code_path, jury_path))
          {
            raw_results = sdsnew("internal error:failed compiling jury");
            sdsfree(jury_code_path);
            goto end_case;
          }
          sdsfree(jury_code_path);
        }

        info("calling %s %s %s %s \n", jury_path,sysinput,sysoutput,suboutput);

        if (0 == pid) /* child */
        {
          expect(0 == close(fildes[0]));
          expect(-1 != dup2(fildes[1], STDOUT_FILENO));
          execl(jury_path, jury_path, sysinput, sysoutput, suboutput, NULL);
          _exit(127);
        }
        else if (0 < pid) /* parent */
        {
          int status;
          char verdict[3];

          sdsfree(jury_path);
          expect(0 == close(fildes[1]));
          waitpid (pid, &status, 0);

          if (WIFEXITED (status) && 0 == WEXITSTATUS (status))
            ;
          else
          {
            raw_results = sdsnew("internal error:jury failed");
            goto end_case;
          }

          expect(-1 != read(fildes[0], verdict, 3));

          if (0 == memcmp("ok", verdict, 2))
          {
          }
          else if (0 == memcmp("wa", verdict, 2))
          {
            raw_results = sdscatprintf(sdsempty(), "wa#%u", case_id);
            goto end_case;
          }
          else
          {
            raw_results = sdscatprintf(sdsempty(), "internal error:unsupported verdict %c%c", verdict[0], verdict[1]);
            goto end_case;
          }
        }
        else
          diep("fork failed");
      }
      else
      {
        if (! safe_tkcmp(sysoutput, suboutput))
        {
          raw_results = sdscatprintf(sdsempty(), "wa#%u", case_id);
          goto end_case;
        }
      }
    }


end_case:
    sdsfree(sysoutput);       sysoutput       = NULL;
    sdsfree(sysinput);        sysinput        = NULL;
    sdsfree(boxsuboutput);    boxsuboutput    = NULL;
    sdsfree(boxsysinput);     boxsysinput     = NULL;
    sdsfree(relboxsuboutput); relboxsuboutput = NULL;
    sdsfree(relboxsysinput);  relboxsysinput  = NULL;
    sdsfree(stat_file);       stat_file       = NULL;
    sdsfree(suboutput);       suboutput       = NULL;
    if (stat_fp)
      fclose(stat_fp);

    if (raw_results)
    {
      if (!uniform_scoring)
        break;
      if (!first_failure)
        first_failure = sdsdup(raw_results);
      sdsfree(raw_results);
      raw_results = NULL;
    }
    else
      ++passed_cases;
  }

  (void)run_isolate_cleanup();

  if (first_failure)
    raw_results = first_failure;

  if (!raw_results)
  {
    raw_results = sdsnew("accepted!");
    total_score = task.max_score;
  }
  else
  {
    if (uniform_scoring)
      total_score = task.count_cases ? task.max_score * (double)passed_cases / task.count_cases : 0.0;
    else
    {
    unsigned failed_case = 0;
    if (sscanf(raw_results, "wa#%u", &failed_case) != 1 &&
        sscanf(raw_results, "time limit exceeded#%u", &failed_case) != 1 &&
        sscanf(raw_results, "memory limit exceeded#%u", &failed_case) != 1 &&
        sscanf(raw_results, "signal(%*d)#%u", &failed_case) != 1 &&
        sscanf(raw_results, "exitcode(%*d)#%u", &failed_case) != 1)
      failed_case = 0;
    if (failed_case)
      total_score = score_for_failed_case(task_pk, failed_case, task.max_score);
    }
  }

  *time_used = max_time;
  *memory_used = max_mem;
  *score = total_score;
  free(*result);
  *result = strdup(raw_results);
  sdsfree(raw_results);

  return 0;
}


void
cleanup(int sig)
{
  (void)sig;
  info("cleaning up...");
  db_cleanup();
  info("closed database");
  exit(EXIT_SUCCESS);
}

static const char *WHITESPACE = "\t\n\r ";

  char *
get_next_token(FILE *fp) 
{
  int ch, idx, capacity;
  char *token;

  idx = 0;
  capacity = 256;
  token = malloc(capacity);

  while ((ch = fgetc(fp)) != EOF && strchr(WHITESPACE, ch));

  if (ch == EOF)
  {
    free(token);
    return NULL;
  }

  do
  {
    token[idx++] = (char)ch;
    if (idx >= capacity)
      token = realloc(token, capacity *= 2);
  } while ((ch = fgetc(fp)) != EOF && !strchr(WHITESPACE, ch));

  token[idx] = '\0';
  return token;
}

int safe_tkcmp(const char *p1, const char *p2)
{
  FILE *f1, *f2;
  f1 = fopen(p1, "r");
  f2 = fopen(p2, "r");

  if (!f1 || !f2)
  {
    warnp("failed to open output files for comparison %s %s\n", p1, p2);
    if (f1)
      fclose(f1);
    if (f2)
      fclose(f2);
    return -1;
  }

  char *t1, *t2;
  int match = 1;

  for (;;)
  {
    t1 = get_next_token(f1);
    t2 = get_next_token(f2);

    if (t2 == NULL && t1 == NULL)
      break;

    if (t2 == NULL || t1 == NULL)
    {
      match = 0;
      break;
    }

    if (0 != strcmp(t1, t2))
    {
      match = 0;
      break;
    }

    free(t1);
    free(t2);
  }

  fclose(f1);
  fclose(f2);
  return match;
}
