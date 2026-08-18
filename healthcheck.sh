#!/bin/sh
set -eu

url=${1:-${TLE_BASE_URL:-http://127.0.0.1:8080}}
body=$(mktemp)
trap 'rm -f "$body"' EXIT INT TERM

status=$(curl --silent --show-error --location --output "$body" \
  --write-out '%{http_code}' "$url/?page=leaderboard")
if [ "$status" != 200 ]; then
  echo "health check failed: HTTP $status" >&2
  exit 1
fi
grep -q '<title>leaderboard</title>' "$body"
echo "health check passed: $url"
