#include "common.h"
#include <sys/wait.h>
#include <dirent.h>

/* prototypes */
int grade(unsigned, unsigned, const char *, unsigned *, unsigned *, double *, char **, char **);
void cleanup(int);
int safe_tkcmp(const char *, const char *);
int copy(char *, char *);

static int
write_submission_source(const char *box_path, const char *code)
{
  char path[600];
  FILE *fp;
  size_t length = strlen(code);
  snprintf(path, sizeof path, "%s/submission.cpp", box_path);
  fp = fopen(path, "w");
  if (!fp)
    return -1;
  if (fwrite(code, 1, length, fp) != length || fclose(fp) != 0)
    return -1;
  return 0;
}

static int
write_text_path(const char *path, const char *text)
{
  FILE *fp = fopen(path, "w");
  size_t length = strlen(text);
  if (!fp)
    return -1;
  if (fwrite(text, 1, length, fp) != length || fclose(fp) != 0)
    return -1;
  return 0;
}

static char *
trim_line(char *line)
{
  char *end;
  while (*line && strchr(" \t\r\n", *line))
    ++line;
  end = line + strlen(line);
  while (end > line && strchr(" \t\r\n", end[-1]))
    *--end = '\0';
  return line;
}

/* Return 1 when no hook exists, 0 on success, 2 on compilation failure. */
static int
run_task_compile_script(const char *task_path, const char *box_path,
    const char *code, const struct task *task, char **compiler_output)
{
  char script_path[1200], diagnostic_path[600];
  pid_t pid;
  int status, elapsed = 0;

  snprintf(script_path, sizeof script_path, "%s/script/compile", task_path);
  if (access(script_path, X_OK) != 0)
    return 1;
  if (write_submission_source(box_path, code) != 0)
    return -1;
  snprintf(diagnostic_path, sizeof diagnostic_path, "%s/compile.err", box_path);
  unlink(diagnostic_path);

  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0)
  {
    char limit[32];
    if (chdir(box_path) != 0)
      _exit(127);
    setenv("TASK_HOME", task_path, 1);
    setenv("WORK_DIR", box_path, 1);
    setenv("SUBMISSION", "./submission.cpp", 1);
    setenv("EXECUTABLE", "./exec", 1);
    setenv("COMPILER_OUTPUT", "./compile.err", 1);
    snprintf(limit, sizeof limit, "%u", task->memory_limit);
    setenv("MEMORY_LIMIT", limit, 1);
    snprintf(limit, sizeof limit, "%u", task->time_limit);
    setenv("TIME_LIMIT", limit, 1);
    execl("/bin/sh", "sh", script_path, NULL);
    _exit(127);
  }
  for (;;)
  {
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid)
      break;
    if (waited < 0)
      return -1;
    if (elapsed++ >= 300)
    {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return -1;
    }
    usleep(100000);
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
  {
    int size = 0;
    *compiler_output = read_file(diagnostic_path, &size);
    return 2;
  }
  {
    char executable_path[600];
    snprintf(executable_path, sizeof executable_path, "%s/exec", box_path);
    if (access(executable_path, X_OK) != 0)
    {
      int size = 0;
      *compiler_output = read_file(diagnostic_path, &size);
      return 2;
    }
  }
  return 0;
}

struct task_stage_env
{
  const char *name;
  const char *value;
};

static int
execute_task_stage(const char *script_path, const char *box_path,
    const struct task_stage_env *env, size_t env_count)
{
  pid_t pid;
  int status, elapsed = 0;
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0)
  {
    if (chdir(box_path) != 0)
      _exit(127);
    for (size_t i = 0; i < env_count; ++i)
      setenv(env[i].name, env[i].value, 1);
    execl("/bin/sh", "sh", script_path, NULL);
    _exit(127);
  }
  for (;;)
  {
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid)
      break;
    if (waited < 0)
      return -1;
    if (elapsed++ >= 300)
    {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return -1;
    }
    usleep(100000);
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Return 1 when absent, 0 when the custom run completed, -1 on script error. */
static int
run_task_run_script(const char *task_path, const char *box_path,
    const struct task *task, unsigned case_id, const char *input_file,
    const char *output_file, const char *stat_file)
{
  char script_path[1200], case_text[32], time_text[32], memory_text[32];
  struct task_stage_env env[11];
  int i = 0;
  snprintf(script_path, sizeof script_path, "%s/script/run", task_path);
  if (access(script_path, X_OK) != 0)
    return 1;
  snprintf(case_text, sizeof case_text, "%u", case_id);
  snprintf(time_text, sizeof time_text, "%u", task->time_limit);
  snprintf(memory_text, sizeof memory_text, "%u", task->memory_limit);
  env[i++] = (struct task_stage_env){"TASK_HOME", task_path};
  env[i++] = (struct task_stage_env){"WORK_DIR", box_path};
  env[i++] = (struct task_stage_env){"EXECUTABLE", "./exec"};
  env[i++] = (struct task_stage_env){"CASE_ID", case_text};
  env[i++] = (struct task_stage_env){"INPUT_FILE", input_file};
  env[i++] = (struct task_stage_env){"OUTPUT_FILE", output_file};
  env[i++] = (struct task_stage_env){"STAT_FILE", stat_file};
  env[i++] = (struct task_stage_env){"TIME_LIMIT", time_text};
  env[i++] = (struct task_stage_env){"MEMORY_LIMIT", memory_text};
  return execute_task_stage(script_path, box_path, env, (size_t)i);
}

/* Return 1 when absent, 0 for ok, 2 for wrong answer, -1 on script error. */
static int
run_task_check_script(const char *task_path, const char *box_path,
    unsigned case_id, const char *input_file, const char *answer_file,
    const char *output_file, char **message)
{
  char script_path[1200], result_path[600], case_text[32];
  struct task_stage_env env[8];
  int i = 0, size = 0;
  char *check_result, *first_line;
  *message = NULL;
  snprintf(script_path, sizeof script_path, "%s/script/check", task_path);
  if (access(script_path, X_OK) != 0)
    return 1;
  snprintf(result_path, sizeof result_path, "%s/check-result-%u", box_path, case_id);
  unlink(result_path);
  snprintf(case_text, sizeof case_text, "%u", case_id);
  env[i++] = (struct task_stage_env){"TASK_HOME", task_path};
  env[i++] = (struct task_stage_env){"WORK_DIR", box_path};
  env[i++] = (struct task_stage_env){"CASE_ID", case_text};
  env[i++] = (struct task_stage_env){"INPUT_FILE", input_file};
  env[i++] = (struct task_stage_env){"ANSWER_FILE", answer_file};
  env[i++] = (struct task_stage_env){"OUTPUT_FILE", output_file};
  env[i++] = (struct task_stage_env){"RESULT_FILE", result_path};
  if (execute_task_stage(script_path, box_path, env, (size_t)i) != 0)
    return -1;
  check_result = read_file(result_path, &size);
  if (!check_result)
    return -1;
  first_line = trim_line(check_result);
  if (!strncmp(first_line, "message=", 8))
    *message = strdup(first_line + 8);
  int verdict = !strncmp(first_line, "ok", 2) ? 0 :
      (!strncmp(first_line, "wa", 2) ? 2 : -1);
  free(check_result);
  return verdict;
}

/* Return 1 when absent, 0 on success, -1 on script or protocol error. */
static int
run_task_score_script(const char *task_path, const char *box_path,
    const struct task *task, unsigned passed_cases, const char *last_result,
    double *score)
{
  char script_path[1200], result_path[600], cases_text[32], count_text[32], max_text[64];
  struct task_stage_env env[8];
  int i = 0, size = 0;
  char *score_result, *line, *saveptr = NULL;
  int found = 0;
  snprintf(script_path, sizeof script_path, "%s/script/score", task_path);
  if (access(script_path, X_OK) != 0)
    return 1;
  snprintf(result_path, sizeof result_path, "%s/score-result", box_path);
  unlink(result_path);
  snprintf(cases_text, sizeof cases_text, "%u", passed_cases);
  snprintf(count_text, sizeof count_text, "%u", task->count_cases);
  snprintf(max_text, sizeof max_text, "%.17g", task->max_score);
  env[i++] = (struct task_stage_env){"TASK_HOME", task_path};
  env[i++] = (struct task_stage_env){"WORK_DIR", box_path};
  env[i++] = (struct task_stage_env){"RESULT_FILE", result_path};
  env[i++] = (struct task_stage_env){"PASSED_CASES", cases_text};
  env[i++] = (struct task_stage_env){"CASE_COUNT", count_text};
  env[i++] = (struct task_stage_env){"MAX_SCORE", max_text};
  env[i++] = (struct task_stage_env){"LAST_RESULT", last_result ? last_result : ""};
  if (execute_task_stage(script_path, box_path, env, (size_t)i) != 0)
    return -1;
  score_result = read_file(result_path, &size);
  if (!score_result)
    return -1;
  for (line = strtok_r(score_result, "\n", &saveptr); line;
      line = strtok_r(NULL, "\n", &saveptr))
  {
    line = trim_line(line);
    if (!strncmp(line, "score=", 6))
    {
      *score = strtod(line + 6, NULL);
      found = 1;
      break;
    }
  }
  free(score_result);
  return found ? 0 : -1;
}

/*
 * Run a trusted task-provided judge script.
 *
 * The script receives TASK_HOME, WORK_DIR, SUBMISSION, RESULT_FILE and
 * MEMORY_LIMIT/TIME_LIMIT. It must write key=value lines to RESULT_FILE:
 *   result=accepted!
 *   score=100
 *   time_ms=12
 *   memory_kb=2048
 *   compiler_output_file=/absolute/path (optional)
 *
 * Task scripts are administrator-owned grading code, just like jury.cpp.
 * They are deliberately not treated as contestant code and must invoke
 * isolate themselves for every contestant-controlled process.
 */
static int
run_task_script(const char *task_path, const char *box_path, const char *code,
    const struct task *task, unsigned *time_used, unsigned *memory_used,
    double *score, char **result, char **compiler_output)
{
  char script_path[1200], result_path[600];
  char *result_file, *line, *saveptr = NULL;
  pid_t pid;
  int status, elapsed = 0;

  snprintf(script_path, sizeof script_path, "%s/script/judge", task_path);
  if (access(script_path, X_OK) != 0)
    return 1; /* no custom script: use the built-in evaluator */
  if (write_submission_source(box_path, code) != 0)
    return -1;
  snprintf(result_path, sizeof result_path, "%s/task-result", box_path);
  unlink(result_path);

  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0)
  {
    char limit[32];
    setenv("TASK_HOME", task_path, 1);
    setenv("WORK_DIR", box_path, 1);
    setenv("SUBMISSION", "./submission.cpp", 1);
    setenv("RESULT_FILE", result_path, 1);
    snprintf(limit, sizeof limit, "%u", task->memory_limit);
    setenv("MEMORY_LIMIT", limit, 1);
    snprintf(limit, sizeof limit, "%u", task->time_limit);
    setenv("TIME_LIMIT", limit, 1);
    execl("/bin/sh", "sh", script_path, NULL);
    _exit(127);
  }

  /* A broken administrator script must not wedge the only grading worker. */
  for (;;)
  {
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid)
      break;
    if (waited < 0)
      return -1;
    if (elapsed++ >= 300)
    {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return -1;
    }
    usleep(100000);
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return -1;

  result_file = read_file(result_path, NULL);
  if (!result_file)
    return -1;
  *result = NULL;
  *compiler_output = NULL;
  *time_used = 0;
  *memory_used = 0;
  *score = 0.0;
  for (line = strtok_r(result_file, "\n", &saveptr); line;
      line = strtok_r(NULL, "\n", &saveptr))
  {
    char *equals = strchr(line, '=');
    if (!equals)
      continue;
    *equals++ = '\0';
    equals = trim_line(equals);
    if (!strcmp(trim_line(line), "result"))
      *result = strdup(equals);
    else if (!strcmp(trim_line(line), "score"))
      *score = strtod(equals, NULL);
    else if (!strcmp(trim_line(line), "time_ms"))
      *time_used = (unsigned)strtoul(equals, NULL, 10);
    else if (!strcmp(trim_line(line), "memory_kb"))
      *memory_used = (unsigned)strtoul(equals, NULL, 10);
    else if (!strcmp(trim_line(line), "compiler_output_file"))
    {
      int size = 0;
      *compiler_output = read_file(equals, &size);
    }
  }
  free(result_file);
  if (!*result)
    *result = strdup("internal error: custom judge produced no result");
  return 0;
}

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

    rc = grade(job.submission_id, job.task_pk, job.code, &time_used, &memory_used, &score,
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

static int
copy_task_files(const char *task_path, const char *box_path)
{
  char source_dir[1200], target_dir[600], source[1500], target[900];
  DIR *dir;
  struct dirent *ent;
  struct stat file_stat;

  snprintf(source_dir, sizeof source_dir, "%s/files", task_path);
  if (access(source_dir, R_OK | X_OK) != 0)
    return 0;
  snprintf(target_dir, sizeof target_dir, "%s/files", box_path);
  if (mkdir(target_dir, 0700) != 0 && errno != EEXIST)
    return -1;
  dir = opendir(source_dir);
  if (!dir)
    return -1;
  while ((ent = readdir(dir)) != NULL)
  {
    if (ent->d_name[0] == '.' || strchr(ent->d_name, '/') || strchr(ent->d_name, '\\'))
      continue;
    snprintf(source, sizeof source, "%s/%s", source_dir, ent->d_name);
    snprintf(target, sizeof target, "%s/%s", target_dir, ent->d_name);
    if (stat(source, &file_stat) != 0 || !S_ISREG(file_stat.st_mode))
    {
      closedir(dir);
      return -1;
    }
    if (copy(source, target) != 0)
    {
      closedir(dir);
      return -1;
    }
  }
  closedir(dir);
  return 0;
}

  int
grade(unsigned submission_id, unsigned task_pk, const char *code, unsigned *time_used, unsigned *memory_used,
    double *score, char **result, char **compiler_output)
{
  if (run_isolate_cleanup() != 0)
    warn("isolate cleanup failed before grading");

  *result = strdup("grading");
  *compiler_output = NULL;

  static struct task task;
  char task_path[9999], box_path[256];
  char resolved_task_path[PATH_MAX];

  if (0 != fetch_task_by_pk(task_pk, &task))
    return -1;

  snprintf(task_path, sizeof(task_path), "%s/%s", TASKS_PATH, task.name);
  if (!realpath(task_path, resolved_task_path))
  {
    warn("task path %s is not accessible: %s", task_path, strerror(errno));
    return -1;
  }
  snprintf(task_path, sizeof(task_path), "%s", resolved_task_path);

  {
    char artifact_dir[600];
    if (mkdir(ARTIFACTS_PATH, 0700) != 0 && errno != EEXIST)
      return -1;
    snprintf(artifact_dir, sizeof artifact_dir, "%s/%u", ARTIFACTS_PATH, submission_id);
    if (mkdir(artifact_dir, 0700) != 0 && errno != EEXIST)
      return -1;
    setenv("ARTIFACT_DIR", artifact_dir, 1);
  }

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
  if (copy_task_files(task_path, box_path) != 0)
  {
    warn("failed copying task files for %s", task.name);
    return -1;
  }

  if (!strcmp(task.comparison, "output_only"))
  {
    char answer_path[12000], output_path[600];
    snprintf(answer_path, sizeof answer_path, "%s/tests/1.sol", task_path);
    snprintf(output_path, sizeof output_path, "%s/1.out", box_path);
    if (task.count_cases != 1 || write_text_path(output_path, code) != 0)
      return -1;
    *result = strdup(safe_tkcmp(answer_path, output_path) == 1 ? "accepted!" : "wa#1");
    *score = !strcmp(*result, "accepted!") ? task.max_score : 0.0;
    *time_used = 0;
    *memory_used = 0;
    (void)run_isolate_cleanup();
    return 0;
  }

  {
    int custom_result = run_task_script(task_path, box_path, code, &task,
        time_used, memory_used, score, result, compiler_output);
    if (custom_result <= 0)
    {
      (void)run_isolate_cleanup();
      return custom_result;
    }
  }

  {
    int compile_status = run_task_compile_script(task_path, box_path, code,
        &task, compiler_output);
    if (compile_status < 0)
      return -1;
    if (compile_status == 2)
    {
      free(*result);
      *result = strdup("compilation error");
      *score = 0.0;
      *time_used = 0;
      *memory_used = 0;
      return 0;
    }
    if (compile_status == 1 && 0 != compile(code, box_path, task.memory_limit))
      compile_status = 2;
    if (compile_status == 2)
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
    unlink(stat_file);
    unlink(boxsuboutput);

    /* run isolated submission code */
    {
      int custom_run = run_task_run_script(task_path, box_path, &task, case_id,
          relboxsysinput, relboxsuboutput, stat_file);

      if (custom_run < 0)
      {
        raw_results = sdscatprintf(sdsempty(), "internal error:run script failed#%u", case_id);
        goto end_case;
      }
      if (custom_run == 1)
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
      char *check_message = NULL;
      int custom_check = run_task_check_script(task_path, box_path, case_id,
          sysinput, sysoutput, suboutput, &check_message);
      free(check_message);
      if (custom_check < 0)
      {
        raw_results = sdscatprintf(sdsempty(), "internal error:check script failed#%u", case_id);
        goto end_case;
      }
      if (custom_check == 2)
      {
        raw_results = sdscatprintf(sdsempty(), "wa#%u", case_id);
        goto end_case;
      }
      if (custom_check == 1 && 0 == strcmp(task.comparison, "special"))
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
      else if (custom_check == 1)
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

  {
    double custom_score = total_score;
    int score_status = run_task_score_script(task_path, box_path, &task,
        passed_cases, raw_results, &custom_score);
    if (score_status < 0)
    {
      sdsfree(raw_results);
      raw_results = sdsnew("internal error:score script failed");
      custom_score = 0.0;
    }
    else if (score_status == 0)
      total_score = custom_score < 0.0 ? 0.0 :
          (custom_score > task.max_score ? task.max_score : custom_score);
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
