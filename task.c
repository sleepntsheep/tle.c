#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "task.h"
#include "cJSON.h"
#include "common.h"
#include "db.h"
#include "config.h"

static void
copy_text(char *out, size_t out_size, const unsigned char *value)
{
  snprintf(out, out_size, "%s", value ? (const char *)value : "");
}

static int
valid_description_name(const char *name)
{
  return name && *name && strcmp(name, ".") != 0 && strcmp(name, "..") != 0
      && !strchr(name, '/') && !strchr(name, '\\');
}

static void
mark_task_seen(unsigned task_pk)
{
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO temp.seen_task_pks(task_pk) VALUES(?1)",
        -1, &stmt, NULL) != SQLITE_OK)
    die("failed to track task %u: %s", task_pk, sqlite3_errmsg(db));
  sqlite3_bind_int(stmt, 1, (int)task_pk);
  if (sqlite3_step(stmt) != SQLITE_DONE)
    die("failed to track task %u: %s", task_pk, sqlite3_errmsg(db));
  sqlite3_finalize(stmt);
}

static void
store_subtask_groups(unsigned task_pk, const cJSON *task_json)
{
  const cJSON *subtasks = cJSON_GetObjectItemCaseSensitive(task_json, "subtasks");
  sqlite3_stmt *del, *ins;

  if (sqlite3_prepare_v2(db, "DELETE FROM subtask_groups WHERE task_pk=?1", -1, &del, NULL) != SQLITE_OK)
    die("failed to prepare subtask delete: %s", sqlite3_errmsg(db));
  sqlite3_bind_int(del, 1, (int)task_pk);
  sqlite3_step(del);
  sqlite3_finalize(del);

  if (!subtasks || !cJSON_IsArray(subtasks))
    return;

  if (sqlite3_prepare_v2(db,
        "INSERT INTO subtask_groups(task_pk,group_idx,score,case_start,case_count) VALUES(?1,?2,?3,?4,?5)",
        -1, &ins, NULL) != SQLITE_OK)
    die("failed to prepare subtask insert: %s", sqlite3_errmsg(db));

  unsigned case_start = 1;
  int idx = 0;
  const cJSON *group;
  cJSON_ArrayForEach(group, subtasks)
  {
    const cJSON *score_item, *count_item;
    double score;
    int count;
    if (!cJSON_IsArray(group))
      die("invalid subtask group in task %u (not an array)", task_pk);
    score_item = cJSON_GetArrayItem(group, 0);
    count_item = cJSON_GetArrayItem(group, 1);
    if (!score_item || !cJSON_IsNumber(score_item) ||
        !count_item || !cJSON_IsNumber(count_item))
      die("invalid subtask group in task %u (expected [score, count])", task_pk);
    score = score_item->valuedouble;
    count = count_item->valueint;
    if (count <= 0)
      die("invalid subtask group in task %u (count must be positive)", task_pk);
    sqlite3_reset(ins);
    sqlite3_bind_int(ins, 1, (int)task_pk);
    sqlite3_bind_int(ins, 2, idx);
    sqlite3_bind_double(ins, 3, score);
    sqlite3_bind_int(ins, 4, (int)case_start);
    sqlite3_bind_int(ins, 5, count);
    if (sqlite3_step(ins) != SQLITE_DONE)
      die("failed to insert subtask group: %s", sqlite3_errmsg(db));
    case_start += (unsigned)count;
    ++idx;
  }
  sqlite3_finalize(ins);
}

static void
consider_task(const struct task *task)
{
  const char *sql =
    "INSERT INTO tasks(task_pk,name,desc_path,memory_limit,time_limit,max_score,count_cases,comparison) "
    "VALUES(?1,?2,?3,?4,?5,?6,?7,?8) ON CONFLICT(task_pk) DO UPDATE SET "
    "name=excluded.name, desc_path=excluded.desc_path, memory_limit=excluded.memory_limit, "
    "time_limit=excluded.time_limit, max_score=excluded.max_score, count_cases=excluded.count_cases, "
    "comparison=excluded.comparison";
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    die("failed to prepare task %s: %s", task->name, sqlite3_errmsg(db));
  sqlite3_bind_int(stmt, 1, (int)task->pk);
  sqlite3_bind_text(stmt, 2, task->name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, task->desc, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, (int)task->memory_limit);
  sqlite3_bind_int(stmt, 5, (int)task->time_limit);
  sqlite3_bind_double(stmt, 6, task->max_score);
  sqlite3_bind_int(stmt, 7, (int)task->count_cases);
  sqlite3_bind_text(stmt, 8, task->comparison, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE)
    die("failed to insert task %s: %s", task->name, sqlite3_errmsg(db));
  sqlite3_finalize(stmt);
}

void
read_tasks(void)
{
  DIR *dir = opendir(TASKS_PATH);
  struct dirent *ent;
  int count = 0;
  if (!dir)
    die("%s folder not present", TASKS_PATH);

  if (sqlite3_exec(db, "CREATE TEMP TABLE IF NOT EXISTS seen_task_pks(task_pk INTEGER PRIMARY KEY); DELETE FROM seen_task_pks;", NULL, NULL, NULL) != SQLITE_OK)
    die("failed to initialize task tracking: %s", sqlite3_errmsg(db));

  while ((ent = readdir(dir)) != NULL)
  {
    char path[1024];
    int file_size;
    struct task task = { 0 };
    const cJSON *pk, *time, *memory, *desc, *max_score, *comparison, *count_cases;
    cJSON *task_json;
    char *file_content;

    if (ent->d_name[0] == '.')
      continue;
    if (strlen(ent->d_name) >= sizeof task.name)
    {
      warn("task directory name is too long: %s", ent->d_name);
      continue;
    }
    snprintf(path, sizeof path, "%s/%s/manifest.json", TASKS_PATH, ent->d_name);
    char directory_path[1024];
    struct stat directory_stat;
    if (snprintf(directory_path, sizeof directory_path, "%s/%s", TASKS_PATH,
          ent->d_name) >= (int)sizeof directory_path ||
        snprintf(path, sizeof path, "%s/%s/manifest.json", TASKS_PATH,
          ent->d_name) >= (int)sizeof path)
    {
      warn("task path is too long: %s", ent->d_name);
      continue;
    }
    if (stat(directory_path, &directory_stat) != 0 || !S_ISDIR(directory_stat.st_mode))
      continue;
    file_content = read_file(path, &file_size);
    if (!file_content)
    {
      warn("failed reading %s", path);
      continue;
    }
    task_json = cJSON_ParseWithLength(file_content, file_size);
    if (!task_json)
      die("failed parsing %s: error before %100s", path, cJSON_GetErrorPtr());

    pk = cJSON_GetObjectItemCaseSensitive(task_json, "pk");
    time = cJSON_GetObjectItemCaseSensitive(task_json, "time");
    memory = cJSON_GetObjectItemCaseSensitive(task_json, "mem");
    desc = cJSON_GetObjectItemCaseSensitive(task_json, "desc");
    count_cases = cJSON_GetObjectItemCaseSensitive(task_json, "ncase");
    comparison = cJSON_GetObjectItemCaseSensitive(task_json, "cmp_type");
    max_score = cJSON_GetObjectItemCaseSensitive(task_json, "max_score");
    if (!pk || !cJSON_IsNumber(pk) || !time || !cJSON_IsNumber(time) ||
        !memory || !cJSON_IsNumber(memory) || !count_cases || !cJSON_IsNumber(count_cases) ||
        !comparison || !cJSON_IsString(comparison) || (desc && !cJSON_IsString(desc)) ||
        (max_score && !cJSON_IsNumber(max_score)))
      die("invalid task manifest: %s", path);

    copy_text(task.name, sizeof task.name, (const unsigned char *)ent->d_name);
    copy_text(task.desc, sizeof task.desc,
        (const unsigned char *)(desc ? desc->valuestring : "desc.pdf"));
    if (!valid_description_name(task.desc))
    {
      warn("invalid description filename in %s", path);
      cJSON_Delete(task_json);
      free(file_content);
      continue;
    }
    copy_text(task.comparison, sizeof task.comparison,
        (const unsigned char *)comparison->valuestring);
    task.pk = (unsigned)pk->valueint;
    task.time_limit = (unsigned)time->valueint;
    task.memory_limit = (unsigned)memory->valueint;
    task.count_cases = (unsigned)count_cases->valueint;
    task.max_score = max_score ? max_score->valuedouble : 100.0;
    if (!strcmp(task.comparison, "output_only") && task.count_cases != 1)
      die("output-only task %s must declare exactly one testcase", task.name);
    {
      char asset_path[1024];
      struct stat script_stat;
      if (strcmp(task.comparison, "special") == 0 &&
          access((snprintf(asset_path, sizeof asset_path, "%s/%s/jury.cpp", TASKS_PATH, task.name), asset_path), F_OK) != 0 &&
          access((snprintf(asset_path, sizeof asset_path, "%s/%s/jury", TASKS_PATH, task.name), asset_path), F_OK) != 0)
        die("special task %s has no jury.cpp or compiled jury", task.name);
      {
        static const char *stage_names[] = {"judge", "compile", "run", "check", "score"};
        for (size_t stage = 0; stage < countof(stage_names); ++stage)
        {
          snprintf(asset_path, sizeof asset_path, "%s/%s/script/%s",
              TASKS_PATH, task.name, stage_names[stage]);
          if (access(asset_path, F_OK) == 0 &&
              (stat(asset_path, &script_stat) != 0 || !S_ISREG(script_stat.st_mode) ||
               access(asset_path, X_OK) != 0))
            die("task %s has a script/%s that is not a regular executable file",
                task.name, stage_names[stage]);
        }
      }
      for (unsigned case_id = 1; case_id <= task.count_cases; ++case_id)
      {
        snprintf(asset_path, sizeof asset_path, "%s/%s/tests/%u.in", TASKS_PATH, task.name, case_id);
        if (access(asset_path, R_OK) != 0) die("task %s is missing %s", task.name, asset_path);
      }
    }
    consider_task(&task);
    mark_task_seen(task.pk);
    store_subtask_groups(task.pk, task_json);
    cJSON_Delete(task_json);
    free(file_content);
    ++count;
  }
  closedir(dir);
  if (sqlite3_exec(db,
        "UPDATE tasks SET is_hidden=1 WHERE task_pk NOT IN (SELECT task_pk FROM temp.seen_task_pks);",
        NULL, NULL, NULL) != SQLITE_OK)
    die("failed to hide removed tasks: %s", sqlite3_errmsg(db));
  sqlite3_exec(db, "DROP TABLE temp.seen_task_pks;", NULL, NULL, NULL);
  info("considered %d tasks", count);
}

int
fetch_task_by_pk(unsigned pk, struct task *task_data)
{
  const char *sql =
    "SELECT t.task_pk,t.name,t.memory_limit,t.time_limit,t.desc_path,t.count_cases,t.max_score,t.comparison,"
    "(SELECT COUNT(*) FROM submissions s WHERE s.task_pk=t.task_pk),"
    "(SELECT COUNT(*) FROM submissions s WHERE s.task_pk=t.task_pk AND s.verdict='accepted') "
    "FROM tasks t WHERE t.task_pk=?1";
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, (int)pk);
  if (sqlite3_step(stmt) != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    return -1;
  }
  task_data->pk = (unsigned)sqlite3_column_int(stmt, 0);
  copy_text(task_data->name, sizeof task_data->name, sqlite3_column_text(stmt, 1));
  task_data->memory_limit = (unsigned)sqlite3_column_int(stmt, 2);
  task_data->time_limit = (unsigned)sqlite3_column_int(stmt, 3);
  copy_text(task_data->desc, sizeof task_data->desc, sqlite3_column_text(stmt, 4));
  task_data->count_cases = (unsigned)sqlite3_column_int(stmt, 5);
  task_data->max_score = sqlite3_column_double(stmt, 6);
  copy_text(task_data->comparison, sizeof task_data->comparison, sqlite3_column_text(stmt, 7));
  task_data->submission_count = (unsigned)sqlite3_column_int(stmt, 8);
  task_data->accepted_count = (unsigned)sqlite3_column_int(stmt, 9);
  sqlite3_finalize(stmt);
  return 0;
}

int
task_toggle_visibility(unsigned pk)
{
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "UPDATE tasks SET is_hidden = CASE is_hidden WHEN 0 THEN 1 ELSE 0 END WHERE task_pk=?1", -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, (int)pk);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int
requeue_submission(unsigned submission_id)
{
  sqlite3_stmt *stmt;
  const char *sql =
    "UPDATE grading_jobs SET status='queued',claimed_at=NULL,claim_token=NULL "
    "WHERE submission_id=?1 AND status IN ('finished','grading')";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, (int)submission_id);
  if (sqlite3_step(stmt) != SQLITE_DONE)
  {
    sqlite3_finalize(stmt);
    return -1;
  }
  int changed = sqlite3_changes(db);
  sqlite3_finalize(stmt);
  if (!changed)
    return 0;
  if (sqlite3_prepare_v2(db,
        "UPDATE submissions SET verdict=NULL,failed_case=NULL,verdict_message=NULL,time_used=NULL,memory_used=NULL,score=NULL,compiler_output=NULL WHERE submission_id=?1",
        -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, (int)submission_id);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 1 : -1;
}

int
fetch_task_name_by_pk(unsigned pk, char *out, int out_size)
{
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT name FROM tasks WHERE task_pk=?1", -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, (int)pk);
  if (sqlite3_step(stmt) != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    return -1;
  }
  const char *name = (const char *)sqlite3_column_text(stmt, 0);
  int length = name ? (int)strlen(name) : 0;
  if (length >= out_size)
  {
    sqlite3_finalize(stmt);
    return -1;
  }
  memcpy(out, name, (size_t)length + 1);
  sqlite3_finalize(stmt);
  return length;
}

int
check_task_pk_validity(unsigned pk)
{
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM tasks WHERE task_pk=?1 AND is_hidden=0", -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, (int)pk);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

int
fetch_tasks(struct task_list_item **items, size_t *count, int user_id, const char *show_solved)
{
  return fetch_tasks_filtered(items, count, user_id, show_solved, NULL, NULL);
}

int
fetch_tasks_filtered(struct task_list_item **items, size_t *count, int user_id,
    const char *show_solved, const char *query, const char *state)
{
  sds sql = sdscat(sdsempty(),
    "SELECT t.task_pk,t.name,"
    "EXISTS(SELECT 1 FROM submissions s WHERE s.task_pk=t.task_pk AND s.user_id=?1 AND (s.verdict='accepted' OR s.result LIKE 'accepted%')),"
    "EXISTS(SELECT 1 FROM submissions s WHERE s.task_pk=t.task_pk AND s.user_id=?1),"
    "EXISTS(SELECT 1 FROM bookmarks b WHERE b.task_pk=t.task_pk AND b.user_id=?1) "
    "FROM tasks t WHERE t.is_hidden=0");
  int n = 1;
  if (show_solved && !strcmp(show_solved, "0"))
    sql = sdscat(sql, " AND NOT EXISTS(SELECT 1 FROM submissions s WHERE s.task_pk=t.task_pk AND s.user_id=?1 AND (s.verdict='accepted' OR s.result LIKE 'accepted%'))");
  if (query && *query) sql = sdscatprintf(sql, " AND instr(lower(t.name),lower(?%d))>0", ++n);
  if (state && !strcmp(state, "solved")) sql = sdscat(sql, " AND EXISTS(SELECT 1 FROM submissions s WHERE s.task_pk=t.task_pk AND s.user_id=?1 AND (s.verdict='accepted' OR s.result LIKE 'accepted%'))");
  if (state && !strcmp(state, "tried")) sql = sdscat(sql, " AND EXISTS(SELECT 1 FROM submissions s WHERE s.task_pk=t.task_pk AND s.user_id=?1) AND NOT EXISTS(SELECT 1 FROM submissions s WHERE s.task_pk=t.task_pk AND s.user_id=?1 AND (s.verdict='accepted' OR s.result LIKE 'accepted%'))");
  if (state && !strcmp(state, "bookmarked")) sql = sdscat(sql, " AND EXISTS(SELECT 1 FROM bookmarks b WHERE b.task_pk=t.task_pk AND b.user_id=?1)");
  sql = sdscat(sql, " ORDER BY t.task_pk DESC");
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) { sdsfree(sql); return -1; }
  sdsfree(sql);
  sqlite3_bind_int(stmt, 1, user_id);
  int p = 2;
  if (query && *query) sqlite3_bind_text(stmt, p++, query, -1, SQLITE_TRANSIENT);
  size_t capacity = 16;
  *items = calloc(capacity, sizeof **items);
  *count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    if (*count == capacity)
    {
      capacity *= 2;
      *items = realloc(*items, capacity * sizeof **items);
      if (!*items) { sqlite3_finalize(stmt); return -1; }
    }
    (*items)[*count].pk = (unsigned)sqlite3_column_int(stmt, 0);
    copy_text((*items)[*count].name, sizeof ((*items)[*count].name), sqlite3_column_text(stmt, 1));
    (*items)[*count].solved = sqlite3_column_int(stmt, 2) != 0;
    (*items)[*count].tried = sqlite3_column_int(stmt, 3) != 0;
    (*items)[*count].bookmarked = sqlite3_column_int(stmt, 4) != 0;
    ++*count;
  }
  int rc = sqlite3_errcode(db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

void free_tasks(struct task_list_item *items) { free(items); }

int
toggle_bookmark(int user_id, unsigned task_pk)
{
  sqlite3_stmt *stmt;
  if (user_id < 0 || sqlite3_prepare_v2(db,
      "DELETE FROM bookmarks WHERE user_id=?1 AND task_pk=?2", -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, user_id); sqlite3_bind_int(stmt, 2, (int)task_pk);
  if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); return -1; }
  int removed = sqlite3_changes(db); sqlite3_finalize(stmt);
  if (removed) return 0;
  if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO bookmarks(user_id,task_pk) VALUES(?1,?2)", -1, &stmt, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_int(stmt, 1, user_id); sqlite3_bind_int(stmt, 2, (int)task_pk);
  int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 1 : -1;
}

int
fetch_submissions(struct submission_row **rows, size_t *count, unsigned from, unsigned limit, unsigned *last_id,
    const char *user_filter, unsigned task_filter, const char *verdict_filter)
{
  sds sql = sdscatprintf(sdsempty(),
    "SELECT s.submission_id,COALESCE(s.verdict,s.result,j.status),s.time_used,s.memory_used,s.submission_time,s.is_public,u.username,t.name,s.score,s.verdict_message "
    "FROM submissions s LEFT JOIN grading_jobs j ON j.submission_id=s.submission_id "
    "LEFT JOIN users u ON u.user_id=s.user_id LEFT JOIN tasks t ON t.task_pk=s.task_pk "
    "WHERE s.submission_id<?1");
  int n = 1;
  if (user_filter && *user_filter)
    sql = sdscatprintf(sql, " AND u.username=?%d", ++n);
  if (task_filter)
    sql = sdscatprintf(sql, " AND s.task_pk=?%d", ++n);
  if (verdict_filter && *verdict_filter)
    sql = sdscatprintf(sql, " AND s.verdict=?%d", ++n);
  sql = sdscatprintf(sql, " ORDER BY s.submission_id DESC LIMIT ?%d", ++n);

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
  {
    sdsfree(sql);
    return -1;
  }
  sdsfree(sql);

  int p = 1;
  sqlite3_bind_int64(stmt, p++, from);
  if (user_filter && *user_filter)
    sqlite3_bind_text(stmt, p++, user_filter, -1, SQLITE_TRANSIENT);
  if (task_filter)
    sqlite3_bind_int(stmt, p++, (int)task_filter);
  if (verdict_filter && *verdict_filter)
    sqlite3_bind_text(stmt, p++, verdict_filter, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, p++, (int)limit);

  *rows = calloc(limit ? limit : 1, sizeof **rows);
  *count = 0; *last_id = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && *count < limit)
  {
    struct submission_row *row = &(*rows)[*count];
    row->submission_id = (unsigned)sqlite3_column_int(stmt, 0);
    row->time_used = (unsigned)sqlite3_column_int(stmt, 2);
    row->memory_used = (unsigned)sqlite3_column_int(stmt, 3);
    row->is_public = sqlite3_column_int(stmt, 5) != 0;
    row->score = sqlite3_column_type(stmt, 8) == SQLITE_NULL ? 0.0 : sqlite3_column_double(stmt, 8);
    copy_text(row->verdict, sizeof row->verdict, sqlite3_column_text(stmt, 1));
    copy_text(row->verdict_message, sizeof row->verdict_message, sqlite3_column_text(stmt, 9));
    copy_text(row->submission_time, sizeof row->submission_time, sqlite3_column_text(stmt, 4));
    copy_text(row->username, sizeof row->username, sqlite3_column_text(stmt, 6));
    copy_text(row->task_name, sizeof row->task_name, sqlite3_column_text(stmt, 7));
    if (!row->username[0]) strcpy(row->username, "anonymous");
    if (!row->task_name[0]) strcpy(row->task_name, "unknown");
    *last_id = row->submission_id;
    ++*count;
  }
  int rc = sqlite3_errcode(db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

void free_submissions(struct submission_row *rows) { free(rows); }

int
fetch_admin_tasks(struct admin_task_item **items, size_t *count)
{
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT task_pk,name,is_hidden FROM tasks ORDER BY task_pk DESC", -1, &stmt, NULL) != SQLITE_OK) return -1;
  size_t capacity = 16;
  *items = calloc(capacity, sizeof **items); *count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    if (*count == capacity)
    {
      capacity *= 2; *items = realloc(*items, capacity * sizeof **items);
      if (!*items) { sqlite3_finalize(stmt); return -1; }
    }
    (*items)[*count].pk = (unsigned)sqlite3_column_int(stmt, 0);
    copy_text((*items)[*count].name, sizeof ((*items)[*count].name), sqlite3_column_text(stmt, 1));
    (*items)[*count].hidden = sqlite3_column_int(stmt, 2) != 0;
    ++*count;
  }
  int rc = sqlite3_errcode(db); sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

void free_admin_tasks(struct admin_task_item *items) { free(items); }

sds
fetch_submission_code_by_id(unsigned pk)
{
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT code,is_public FROM submissions WHERE submission_id=?1", -1, &stmt, NULL) != SQLITE_OK) return NULL;
  sqlite3_bind_int(stmt, 1, (int)pk);
  if (sqlite3_step(stmt) != SQLITE_ROW || sqlite3_column_int(stmt, 1) == 0)
  {
    sqlite3_finalize(stmt); return NULL;
  }
  sds code = sdsnew((const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return code;
}

int
enqueue_submission(int user_id, unsigned task_pk, const char *code, int is_public)
{
  sqlite3_stmt *stmt;
  if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return -1;
  if (sqlite3_prepare_v2(db, "INSERT INTO submissions(user_id,task_pk,code,is_public) VALUES(?1,?2,?3,?4)", -1, &stmt, NULL) != SQLITE_OK) goto fail;
  if (user_id < 0) sqlite3_bind_null(stmt, 1); else sqlite3_bind_int(stmt, 1, user_id);
  sqlite3_bind_int(stmt, 2, (int)task_pk); sqlite3_bind_text(stmt, 3, code, -1, SQLITE_TRANSIENT); sqlite3_bind_int(stmt, 4, is_public != 0);
  if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); goto fail; }
  sqlite3_finalize(stmt);
  sqlite3_int64 id = sqlite3_last_insert_rowid(db);
  if (sqlite3_prepare_v2(db, "INSERT INTO grading_jobs(submission_id,status) VALUES(?1,'queued')", -1, &stmt, NULL) != SQLITE_OK) goto fail;
  sqlite3_bind_int64(stmt, 1, id);
  if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); goto fail; }
  sqlite3_finalize(stmt);
  if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) return -1;
  return (int)id;
fail:
  sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
  return -1;
}

int
claim_submission(struct submission_job *job)
{
  static unsigned sequence;
  struct timespec now;
  char token[96];
  sqlite3_stmt *stmt;
  int result = 0;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  snprintf(token, sizeof token, "%ld-%ld-%u", (long)getpid(), (long)now.tv_nsec, sequence++);
  if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return -1;
  sqlite3_exec(db, "UPDATE grading_jobs SET status='queued',claimed_at=NULL,claim_token=NULL WHERE status='grading' AND claimed_at < datetime('now','-10 minutes')", NULL, NULL, NULL);
  if (sqlite3_prepare_v2(db, "SELECT s.submission_id,s.user_id,s.task_pk,s.is_public,s.code FROM grading_jobs j JOIN submissions s ON s.submission_id=j.submission_id WHERE j.status='queued' AND s.code IS NOT NULL ORDER BY j.submission_id LIMIT 1", -1, &stmt, NULL) != SQLITE_OK) goto fail;
  if (sqlite3_step(stmt) != SQLITE_ROW)
  {
    sqlite3_finalize(stmt); sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return 0;
  }
  job->submission_id = (unsigned)sqlite3_column_int(stmt, 0);
  job->user_id = sqlite3_column_type(stmt, 1) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt, 1);
  job->task_pk = (unsigned)sqlite3_column_int(stmt, 2);
  job->is_public = sqlite3_column_int(stmt, 3) != 0;
  job->code = strdup((const char *)sqlite3_column_text(stmt, 4));
  sqlite3_finalize(stmt);
  if (!job->code) goto fail;
  if (sqlite3_prepare_v2(db, "UPDATE grading_jobs SET status='grading',claimed_at=datetime('now'),claim_token=?1,attempts=attempts+1 WHERE submission_id=?2 AND status='queued'", -1, &stmt, NULL) != SQLITE_OK) goto fail;
  sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT); sqlite3_bind_int(stmt, 2, (int)job->submission_id);
  if (sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(db) != 1) { sqlite3_finalize(stmt); goto fail; }
  sqlite3_finalize(stmt);
  strncpy(job->claim_token, token, sizeof job->claim_token - 1); job->claim_token[sizeof job->claim_token - 1] = 0;
  if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto fail_no_rollback;
  result = 1;
  return result;
fail:
  free_submission_job(job);
  sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
fail_no_rollback:
  return -1;
}

static void
parse_verdict(const char *result, char *verdict, size_t verdict_size, int *failed_case)
{
  *failed_case = -1;
  if (!strcmp(result, "accepted!")) snprintf(verdict, verdict_size, "accepted");
  else if (!strncmp(result, "compilation error", 18)) snprintf(verdict, verdict_size, "compilation_error");
  else if (sscanf(result, "wa#%d", failed_case) == 1) snprintf(verdict, verdict_size, "wrong_answer");
  else if (sscanf(result, "time limit exceeded#%d", failed_case) == 1) snprintf(verdict, verdict_size, "time_limit");
  else if (sscanf(result, "memory limit exceeded#%d", failed_case) == 1) snprintf(verdict, verdict_size, "memory_limit");
  else if (!strncmp(result, "signal(", 7) || !strncmp(result, "exitcode(", 9)) snprintf(verdict, verdict_size, "runtime_error");
  else snprintf(verdict, verdict_size, "internal_error");
}

int
complete_submission(unsigned submission_id, const char *result, unsigned time_used,
    unsigned memory_used, double score, const char *compiler_output,
    const char *claim_token)
{
  char verdict[32], failed_case_text[16];
  int failed_case;
  parse_verdict(result, verdict, sizeof verdict, &failed_case);
  if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return -1;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "UPDATE grading_jobs SET status='finished',claimed_at=NULL,claim_token=NULL WHERE submission_id=?1 AND claim_token=?2 AND status='grading'", -1, &stmt, NULL) != SQLITE_OK) goto fail;
  sqlite3_bind_int(stmt, 1, (int)submission_id); sqlite3_bind_text(stmt, 2, claim_token, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(db) != 1) { sqlite3_finalize(stmt); goto fail; }
  sqlite3_finalize(stmt);
  if (sqlite3_prepare_v2(db, "UPDATE submissions SET verdict=?1,failed_case=?2,verdict_message=?3,time_used=?4,memory_used=?5,score=?6,compiler_output=?7 WHERE submission_id=?8", -1, &stmt, NULL) != SQLITE_OK) goto fail;
  sqlite3_bind_text(stmt, 1, verdict, -1, SQLITE_TRANSIENT);
  if (failed_case < 0) sqlite3_bind_null(stmt, 2); else { snprintf(failed_case_text, sizeof failed_case_text, "%d", failed_case); sqlite3_bind_int(stmt, 2, failed_case); }
  sqlite3_bind_text(stmt, 3, result, -1, SQLITE_TRANSIENT); sqlite3_bind_int(stmt, 4, (int)time_used); sqlite3_bind_int(stmt, 5, (int)memory_used); sqlite3_bind_double(stmt, 6, score);
  if (compiler_output && *compiler_output)
    sqlite3_bind_text(stmt, 7, compiler_output, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(stmt, 7);
  sqlite3_bind_int(stmt, 8, (int)submission_id);
  if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); goto fail; }
  sqlite3_finalize(stmt);
  if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) return -1;
  return 0;
fail:
  sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
  return -1;
}

void
free_submission_job(struct submission_job *job)
{
  free(job->code);
  job->code = NULL;
}

int
fetch_submission_detail(unsigned id, struct submission_detail *out)
{
  const char *sql =
    "SELECT s.submission_id,s.user_id,s.task_pk,COALESCE(s.verdict,s.result,j.status),"
    "s.time_used,s.memory_used,s.score,s.is_public,s.failed_case,s.verdict_message,"
    "s.compiler_output,s.submission_time,u.username,t.name,j.status "
    "FROM submissions s LEFT JOIN grading_jobs j ON j.submission_id=s.submission_id "
    "LEFT JOIN users u ON u.user_id=s.user_id LEFT JOIN tasks t ON t.task_pk=s.task_pk "
    "WHERE s.submission_id=?1";
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, (int)id);
  if (sqlite3_step(stmt) != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    return -1;
  }
  memset(out, 0, sizeof *out);
  out->submission_id = (unsigned)sqlite3_column_int(stmt, 0);
  out->user_id = sqlite3_column_type(stmt, 1) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt, 1);
  out->task_pk = (unsigned)sqlite3_column_int(stmt, 2);
  copy_text(out->verdict, sizeof out->verdict, sqlite3_column_text(stmt, 3));
  out->time_used = (unsigned)sqlite3_column_int(stmt, 4);
  out->memory_used = (unsigned)sqlite3_column_int(stmt, 5);
  out->score = sqlite3_column_double(stmt, 6);
  out->is_public = sqlite3_column_int(stmt, 7) != 0;
  out->failed_case = sqlite3_column_type(stmt, 8) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt, 8);
  copy_text(out->verdict_message, sizeof out->verdict_message, sqlite3_column_text(stmt, 9));
  copy_text(out->compiler_output, sizeof out->compiler_output, sqlite3_column_text(stmt, 10));
  copy_text(out->submission_time, sizeof out->submission_time, sqlite3_column_text(stmt, 11));
  copy_text(out->username, sizeof out->username, sqlite3_column_text(stmt, 12));
  copy_text(out->task_name, sizeof out->task_name, sqlite3_column_text(stmt, 13));
  copy_text(out->job_status, sizeof out->job_status, sqlite3_column_text(stmt, 14));
  if (!out->username[0]) strcpy(out->username, "anonymous");
  if (!out->task_name[0]) strcpy(out->task_name, "unknown");
  if (!out->verdict[0]) strcpy(out->verdict, out->job_status[0] ? out->job_status : "pending");
  sqlite3_finalize(stmt);
  return 0;
}

int
fetch_leaderboard(struct leaderboard_row **rows, size_t *count)
{
  const char *sql =
    "SELECT u.username,COUNT(a.task_pk),COALESCE(SUM(a.best),0) "
    "FROM users u LEFT JOIN ("
    "  SELECT user_id,task_pk,MAX(score) AS best FROM submissions "
    "  WHERE verdict='accepted' GROUP BY user_id,task_pk"
    ") a ON a.user_id=u.user_id "
    "GROUP BY u.user_id "
    "ORDER BY COUNT(a.task_pk) DESC, COALESCE(SUM(a.best),0) DESC, u.username ASC";
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  size_t capacity = 16;
  *rows = calloc(capacity, sizeof **rows);
  *count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    if (*count == capacity)
    {
      capacity *= 2;
      *rows = realloc(*rows, capacity * sizeof **rows);
      if (!*rows) { sqlite3_finalize(stmt); return -1; }
    }
    copy_text((*rows)[*count].username, sizeof ((*rows)[*count].username), sqlite3_column_text(stmt, 0));
    (*rows)[*count].solved = sqlite3_column_int(stmt, 1);
    (*rows)[*count].total_score = sqlite3_column_double(stmt, 2);
    ++*count;
  }
  int rc = sqlite3_errcode(db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

void free_leaderboard(struct leaderboard_row *rows) { free(rows); }

int
fetch_subtask_groups(unsigned task_pk, struct subtask_group **groups, size_t *count)
{
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
        "SELECT case_start,case_count,score FROM subtask_groups WHERE task_pk=?1 ORDER BY group_idx",
        -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, (int)task_pk);
  size_t capacity = 8;
  *groups = calloc(capacity, sizeof **groups);
  *count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    if (*count == capacity)
    {
      capacity *= 2;
      *groups = realloc(*groups, capacity * sizeof **groups);
      if (!*groups) { sqlite3_finalize(stmt); return -1; }
    }
    (*groups)[*count].case_start = (unsigned)sqlite3_column_int(stmt, 0);
    (*groups)[*count].case_count = (unsigned)sqlite3_column_int(stmt, 1);
    (*groups)[*count].score = sqlite3_column_double(stmt, 2);
    ++*count;
  }
  int rc = sqlite3_errcode(db);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

void free_subtask_groups(struct subtask_group *groups) { free(groups); }
