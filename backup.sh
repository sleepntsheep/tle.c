#!/bin/sh
set -eu

database=${1:-tle.db}
backup_dir=${TLE_BACKUP_DIR:-backups}
keep=${TLE_BACKUP_KEEP:-14}
mkdir -p "$backup_dir"
destination=${2:-"${backup_dir}/$(basename "$database").$(date -u +%Y%m%dT%H%M%SZ).backup"}

if ! command -v sqlite3 >/dev/null 2>&1; then
  echo "sqlite3 command is required for backups" >&2
  exit 2
fi

sqlite3 "$database" ".backup '$destination'"
echo "SQLite backup written to $destination"

find "$backup_dir" -maxdepth 1 -type f \
  -name "$(basename "$database").*.backup" -printf '%T@ %p\n' 2>/dev/null |
  sort -nr | tail -n +$((keep + 1)) | cut -d' ' -f2- |
  while IFS= read -r old_backup; do
    [ -n "$old_backup" ] && rm -f -- "$old_backup"
  done
