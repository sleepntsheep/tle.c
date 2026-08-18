
# tle (Time Limit Exceeded) online judge

an online judge for competitive programming, purely developed in C

The interface uses a small monochrome terminal-style favicon. Its mark is a
minimal `>_` shape, representing a submitted program and the judge response:

![tle favicon](assets/favicon.svg)


# deployment:

Copy `config.def.h` to `config.h` and modify `config.h` as you like. The
default database is the local SQLite file `tle.db`; no database server or user
setup is required.

Set the administrator password through the environment before starting the web
server:

```
export TLE_ADMIN_PASSWORD='choose-a-long-random-password'
```

Then run `make` to compile, followed by `./run.sh` to start the grader and web
server.

Run `make task-check` before deploying task data. It validates manifests,
special checkers, and the expected test-input files.

Tasks may optionally provide an executable `script/judge`. When present, the
grader runs that administrator-owned script instead of the built-in batch
evaluator. It receives `TASK_HOME`, `WORK_DIR`, `SUBMISSION`, `RESULT_FILE`,
`TIME_LIMIT`, and `MEMORY_LIMIT`. The script must run contestant-controlled
programs through `isolate` and write these key-value fields to `RESULT_FILE`:

```
result=accepted!
score=100
time_ms=12
memory_kb=2048
compiler_output_file=/path/to/compiler.err
```

Tasks may instead provide an executable `script/compile` to replace only the
built-in compilation step. It receives the same task/workspace variables plus
`EXECUTABLE=./exec` and `COMPILER_OUTPUT=./compile.err`; it must create an
executable at `EXECUTABLE` on success. The normal isolated execution and
comparison path then continues unchanged.

An executable `script/run` can replace execution for each testcase. It receives
`CASE_ID`, `INPUT_FILE`, `OUTPUT_FILE`, `STAT_FILE`, `EXECUTABLE`, and the task
limits. It must write the contestant output and an isolate-compatible stat
file. An executable `script/check` can replace comparison for each testcase;
it receives `INPUT_FILE`, `ANSWER_FILE`, `OUTPUT_FILE`, `CASE_ID`, and
`RESULT_FILE`, and writes `ok` or `wa` as the first line of `RESULT_FILE`.
An executable `script/score` can replace the final score calculation. It
receives `PASSED_CASES`, `CASE_COUNT`, `MAX_SCORE`, `LAST_RESULT`, and
`RESULT_FILE`, and must write `score=<number>` to `RESULT_FILE`.

This provides a stable extension point for multi-file, output-only,
communication, and interactive tasks while ordinary tasks continue using the
built-in evaluator. Task scripts are trusted grading code and must never be
created from contestant submissions.

Run `make runtime-check` on the deployment host. The grader requires an
`isolate` installation with the privileges expected by that installation;
the check actually initializes and cleans up a sandbox instead of assuming
that the grader process itself must run as root.
`run.sh` is intended for a single local/staging process group. In production,
run the web process and grader under separate supervisor services, with only
the grader granted the privileges required by `isolate`.

For production, terminate HTTPS at the reverse proxy and keep the judge bound
to an internal interface. If the judge itself terminates TLS, set `TLE_TLS_CERT`
and `TLE_TLS_KEY` to PEM certificate and private-key files. Do not expose the
default HTTP mode directly to an untrusted network.

Set `TLE_BASE_URL` to the public HTTPS URL when deploying behind a domain or
reverse proxy. Example systemd units and a daily backup timer are in `deploy/`.
Install the units for the account that owns the deployment (the examples use
`ray`), then run `systemctl enable --now tle-grader tle-web tle-backup.timer`.
Keep `/etc/tle/tle.env` mode `0600` because it contains deployment secrets.
After starting the services, run `/opt/selfhost/tle/healthcheck.sh https://judge.example.com`.
To verify a backup without changing the live database, run
`sqlite3 backups/tle.db.*.backup 'PRAGMA integrity_check;'`. To restore, stop
both services, copy the verified backup over `tle.db`, then start the services
again and run `make runtime-check`.

Submissions and grading jobs are stored separately in SQLite. `tle_web`
inserts a submission and its `grading_jobs` row atomically. `tle_grader`
atomically claims queued jobs and writes structured verdict fields such as
`accepted`, `wrong_answer`, or `time_limit`. This allows the grader to restart
without losing submissions and does not require a FIFO.

Before upgrading, make a backup with `./backup.sh`. The database keeps a
schema version and uses SQLite's online backup mechanism, so backups can be
taken while the judge is running. The same command also archives `tasks/` so
the problem statements, checkers, and test data can be restored with the
database. Set `TLE_TASKS_DIR` if the task directory is elsewhere.
