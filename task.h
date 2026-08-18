#ifndef TLE_TASK_H
#define TLE_TASK_H
#include "sds.h"

struct task
{
  char name[256];
  unsigned pk;
  unsigned memory_limit;
  unsigned time_limit;
  char desc[1024];
  char comparison[99];
  unsigned count_cases;
  double max_score;
  unsigned submission_count;
  unsigned accepted_count;
};

struct task_list_item
{
  unsigned pk;
  char name[256];
  int solved;
  int tried;
  int bookmarked;
};

struct submission_row
{
  unsigned submission_id;
  unsigned time_used;
  unsigned memory_used;
  int is_public;
  char username[256];
  char task_name[256];
  char verdict[64];
  char verdict_message[256];
  char subtask_results[1024];
  char submission_time[64];
  double score;
};

struct admin_task_item
{
  unsigned pk;
  char name[256];
  int hidden;
};

struct submission_job
{
  unsigned submission_id;
  int user_id;
  unsigned task_pk;
  int is_public;
  char claim_token[96];
  char *code;
};

struct submission_detail
{
  unsigned submission_id;
  int user_id;
  unsigned task_pk;
  unsigned time_used;
  unsigned memory_used;
  double score;
  int is_public;
  int failed_case;
  char username[256];
  char task_name[256];
  char verdict[64];
  char verdict_message[1024];
  char subtask_results[1024];
  char compiler_output[16384];
  char job_status[16];
  char submission_time[64];
};

struct leaderboard_row
{
  char username[256];
  int solved;
  double total_score;
};

struct subtask_group
{
  unsigned case_start;
  unsigned case_count;
  double score;
};

int 
fetch_task_by_pk(unsigned pk, struct task *task_data);

int 
fetch_task_name_by_pk(unsigned pk, char *out, int out_size);

sds 
fetch_submission_code_by_id(unsigned pk);

int 
check_task_pk_validity(unsigned pk);

int
fetch_tasks(struct task_list_item **items, size_t *count,
    int user_id, const char *show_solved);

int
fetch_tasks_filtered(struct task_list_item **items, size_t *count,
    int user_id, const char *show_solved, const char *query,
    const char *state);

int
toggle_bookmark(int user_id, unsigned task_pk);

void
free_tasks(struct task_list_item *items);

int
fetch_submissions(struct submission_row **rows, size_t *count,
    unsigned from, unsigned limit, unsigned *last_id,
    const char *user_filter, unsigned task_filter, const char *verdict_filter);

void
free_submissions(struct submission_row *rows);

int
fetch_submission_detail(unsigned id, struct submission_detail *out);

int
fetch_leaderboard(struct leaderboard_row **rows, size_t *count);

void
free_leaderboard(struct leaderboard_row *rows);

int
fetch_subtask_groups(unsigned task_pk, struct subtask_group **groups, size_t *count);

void
free_subtask_groups(struct subtask_group *groups);

int
fetch_admin_tasks(struct admin_task_item **items, size_t *count);

void
free_admin_tasks(struct admin_task_item *items);

void
read_tasks(void);


int
task_toggle_visibility(unsigned pk);

int
requeue_submission(unsigned submission_id);

int
enqueue_submission(int user_id, unsigned task_pk, const char *code, int is_public);

int
claim_submission(struct submission_job *job);

int
complete_submission(unsigned submission_id, const char *result,
    unsigned time_used, unsigned memory_used, double score,
    const char *compiler_output, const char *subtask_results,
    const char *claim_token);

void
free_submission_job(struct submission_job *job);

#endif
