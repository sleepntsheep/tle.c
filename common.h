#ifndef TLE_COMMON_H
#define TLE_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "config.h"
#include "db.h"
#include "task.h"
#include "utils.h"
#include "config.h"
#include "sds.h"

#ifdef _WIN32
#include "winsock2.h"
#endif

#define sizeof(x) (ptrdiff_t)(sizeof(x))
#define countof(x) (sizeof(x)/sizeof(*(x)))
#define lengthof(x) (countof(x) - 1)

#endif

