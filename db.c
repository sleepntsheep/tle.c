#include "common.h"
#include "config.h"
#include "db.h"
#include <pthread.h>

sqlite3 *db;
static pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;

void
db_lock(void)
{
  pthread_mutex_lock(&db_mutex);
}

void
db_unlock(void)
{
  pthread_mutex_unlock(&db_mutex);
}

static void
exec_sql(const char *sql)
{
  char *error = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
  if (rc != SQLITE_OK)
  {
    die("sqlite query failed: %s", error ? error : sqlite3_errmsg(db));
  }
  sqlite3_free(error);
}

static void
ensure_column(const char *table, const char *column, const char *definition)
{
  const char *decl_type = NULL;
  const char *collation = NULL;
  int not_null = 0;
  int primary_key = 0;
  int autoincrement = 0;
  int rc = sqlite3_table_column_metadata(db, NULL, table, column,
      &decl_type, &collation, &not_null, &primary_key, &autoincrement);
  if (rc == SQLITE_OK)
    return;
  if (rc != SQLITE_ERROR)
    die("failed checking SQLite column %s.%s: %s", table, column,
        sqlite3_errmsg(db));

  char sql[256];
  snprintf(sql, sizeof sql, "ALTER TABLE %s ADD COLUMN %s %s",
      table, column, definition);
  exec_sql(sql);
}

static int
schema_version(void)
{
  sqlite3_stmt *stmt;
  int version = 0;
  if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(version),0) FROM schema_version",
        -1, &stmt, NULL) == SQLITE_OK)
  {
    if (sqlite3_step(stmt) == SQLITE_ROW)
      version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  return version;
}

static void
set_schema_version(int version)
{
  sqlite3_stmt *stmt;
  exec_sql("DELETE FROM schema_version;");
  if (sqlite3_prepare_v2(db, "INSERT INTO schema_version(version) VALUES(?1)",
        -1, &stmt, NULL) != SQLITE_OK)
    die("failed preparing schema migration: %s", sqlite3_errmsg(db));
  sqlite3_bind_int(stmt, 1, version);
  if (sqlite3_step(stmt) != SQLITE_DONE)
    die("failed applying schema migration: %s", sqlite3_errmsg(db));
  sqlite3_finalize(stmt);
}

void
db_init(void)
{
  int rc = sqlite3_open(DB_PATH, &db);
  if (rc != SQLITE_OK)
    die("failed to open SQLite database %s: %s", DB_PATH, sqlite3_errmsg(db));

  sqlite3_busy_timeout(db, 5000);
  exec_sql("PRAGMA journal_mode = WAL;");
  exec_sql("PRAGMA foreign_keys = ON;");
  exec_sql("CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);");
  exec_sql("INSERT INTO schema_version(version) SELECT 0 WHERE NOT EXISTS (SELECT 1 FROM schema_version);");
  int version = schema_version();

  exec_sql(
    "CREATE TABLE IF NOT EXISTS tasks ("
    "task_pk INTEGER PRIMARY KEY, name TEXT NOT NULL, desc_path TEXT NOT NULL,"
    "memory_limit INTEGER NOT NULL, time_limit INTEGER NOT NULL,"
    "max_score REAL NOT NULL, count_cases INTEGER NOT NULL, comparison TEXT NOT NULL,"
    "is_hidden INTEGER NOT NULL DEFAULT 0"
    ");"
  );
  exec_sql(
    "CREATE TABLE IF NOT EXISTS users ("
    "user_id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT NOT NULL UNIQUE,"
    "hashed_password TEXT NOT NULL, registration_time TEXT DEFAULT CURRENT_TIMESTAMP"
    ");"
  );
  exec_sql(
    "CREATE TABLE IF NOT EXISTS submissions ("
    "submission_id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER,"
    "task_pk INTEGER NOT NULL REFERENCES tasks(task_pk), result TEXT,"
    "time_used INTEGER, memory_used INTEGER, score REAL, code TEXT,"
    "verdict TEXT, failed_case INTEGER, verdict_message TEXT,"
    "compiler_output TEXT,"
    "submission_time TEXT DEFAULT CURRENT_TIMESTAMP, is_public INTEGER NOT NULL DEFAULT 0"
    ");"
  );
  if (version < 2)
  {
    ensure_column("submissions", "compiler_output", "TEXT");
    set_schema_version(2);
    version = 2;
  }
  exec_sql(
    "CREATE TABLE IF NOT EXISTS sessions ("
    "token TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,"
    "csrf_token TEXT NOT NULL, created_at TEXT DEFAULT CURRENT_TIMESTAMP"
    ");"
  );
  if (version < 3)
  {
    ensure_column("sessions", "csrf_token", "TEXT");
    set_schema_version(3);
  }
  exec_sql(
    "CREATE TABLE IF NOT EXISTS grading_jobs ("
    "submission_id INTEGER PRIMARY KEY REFERENCES submissions(submission_id) ON DELETE CASCADE,"
    "status TEXT NOT NULL DEFAULT 'queued' CHECK(status IN ('queued','grading','finished')),"
    "claimed_at TEXT, claim_token TEXT, attempts INTEGER NOT NULL DEFAULT 0,"
    "created_at TEXT DEFAULT CURRENT_TIMESTAMP"
    ");"
  );
  exec_sql("CREATE INDEX IF NOT EXISTS grading_jobs_queue_idx ON grading_jobs(status, submission_id);");
  exec_sql(
    "CREATE TABLE IF NOT EXISTS subtask_groups ("
    "task_pk INTEGER NOT NULL REFERENCES tasks(task_pk), group_idx INTEGER NOT NULL,"
    "score REAL NOT NULL, case_start INTEGER NOT NULL, case_count INTEGER NOT NULL,"
    "PRIMARY KEY(task_pk, group_idx)"
    ");"
  );

  info("SQLite database initialized at %s", DB_PATH);
}

void
db_cleanup(void)
{
  if (db)
  {
    sqlite3_close(db);
    db = NULL;
  }
}
