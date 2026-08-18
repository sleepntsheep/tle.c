#!/bin/sh
set -eu

if [ "${TLE_PRODUCTION:-0}" = 1 ]; then
  if [ -z "${TLE_TLS_CERT:-}" ] || [ -z "${TLE_TLS_KEY:-}" ]; then
    echo "TLE_PRODUCTION requires TLE_TLS_CERT and TLE_TLS_KEY" >&2
    exit 2
  fi
  if [ "$(id -u)" = 0 ]; then
    echo "refusing to run the whole judge as root; configure isolate separately" >&2
    exit 2
  fi
fi

grader_pid=
cleanup()
{
  trap - EXIT INT TERM
  if [ -n "${grader_pid}" ]; then
    kill "${grader_pid}" 2>/dev/null || true
    wait "${grader_pid}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

./tle_grader &
grader_pid=$!
./tle_web "$@"
