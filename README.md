
# tle (Time Limit Exceeded) online judge

an online judge for competitive programming, purely developed in C


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
taken while the judge is running.
