
cc = gcc
cflags = -g3 -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -pthread

.PHONY: build check runtime-check healthcheck task-check

config.h: config.def.h
	cp config.def.h config.h

build: config.h
	$(cc) $(cflags) -o tle_web    html.c views.c sds.c web.c cJSON.c task.c utils.c db.c md5.c user.c -lsqlite3 -lm -lmicrohttpd -lcrypt
	$(cc) $(cflags) -o tle_grader sds.c db.c grader.c cJSON.c task.c utils.c             -lsqlite3 -lm
	$(cc) $(cflags) -o tle_communicate communicate.c                              -lm

check: build
	git diff --check
	sh -n run.sh backup.sh
	./tle_web --check-config

healthcheck:
	./healthcheck.sh

task-check: build
	./tle_web --check-tasks

runtime-check: build
	./tle_grader --check-runtime
