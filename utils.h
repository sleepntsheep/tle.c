#ifndef TLE_UTILS_H
#define TLE_UTILS_H
#include <stddef.h>
#include <stdint.h>

#define expect(x) do { if(!(x)) { fprintf(stderr, "expected %s on %s:%d\n", #x, __FILE__, __LINE__); exit(1); } } while (0)

const char*
get_content_type(const char *file_path);

char*
recv_string(int);

unsigned
recv_u32(int);

void
send_string(int, const char *, unsigned);

void
send_u32(int, unsigned);

char *
read_file_binary(const char *, int *);

char *
read_file(const char *, int *);

void die(const char *fmt, ...);
void info(const char *fmt, ...);
void warn(const char *fmt, ...);
void diep(const char *fmt, ...);
void warnp(const char *fmt, ...);
char *kprintf(const char* fmt, ...);

#endif

