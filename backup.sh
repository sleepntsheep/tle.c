#!/bin/sh
set -eu

database=${1:-tle.db}
backup_dir=${TLE_BACKUP_DIR:-backups}
keep=${TLE_BACKUP_KEEP:-14}
task_dir=${TLE_TASKS_DIR:-tasks}
artifact_dir=${TLE_ARTIFACTS_DIR:-artifacts}
mkdir -p "$backup_dir"
destination=${2:-"${backup_dir}/$(basename "$database").$(date -u +%Y%m%dT%H%M%SZ).backup"}

if ! command -v sqlite3 >/dev/null 2>&1; then
  echo "sqlite3 command is required for backups" >&2
  exit 2
fi

sqlite3 "$database" ".backup '$destination'"
echo "SQLite backup written to $destination"

if [ -d "$task_dir" ]; then
  task_archive="${backup_dir}/tle-tasks.$(date -u +%Y%m%dT%H%M%SZ).tar.gz"
  tar -czf "$task_archive" -C "$(dirname "$task_dir")" "$(basename "$task_dir")"
  echo "Task archive written to $task_archive"
fi

if [ -d "$artifact_dir" ]; then
  artifact_archive="${backup_dir}/tle-artifacts.$(date -u +%Y%m%dT%H%M%SZ).tar.gz"
  tar -czf "$artifact_archive" -C "$(dirname "$artifact_dir")" "$(basename "$artifact_dir")"
  echo "Artifact archive written to $artifact_archive"
fi

find "$backup_dir" -maxdepth 1 -type f \
  -name "$(basename "$database").*.backup" -printf '%T@ %p\n' 2>/dev/null |
  sort -nr | tail -n +$((keep + 1)) | cut -d' ' -f2- |
  while IFS= read -r old_backup; do
    [ -n "$old_backup" ] && rm -f -- "$old_backup"
  done

find "$backup_dir" -maxdepth 1 -type f -name 'tle-tasks.*.tar.gz' -printf '%T@ %p\n' 2>/dev/null |
  sort -nr | tail -n +$((keep + 1)) | cut -d' ' -f2- |
  while IFS= read -r old_archive; do
    [ -n "$old_archive" ] && rm -f -- "$old_archive"
  done

find "$backup_dir" -maxdepth 1 -type f -name 'tle-artifacts.*.tar.gz' -printf '%T@ %p\n' 2>/dev/null |
  sort -nr | tail -n +$((keep + 1)) | cut -d' ' -f2- |
  while IFS= read -r old_archive; do
    [ -n "$old_archive" ] && rm -f -- "$old_archive"
  done
