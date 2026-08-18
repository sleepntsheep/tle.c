#include "utils.h"
#include <stdarg.h>
#include "common.h"

  char *
read_file(const char *path, int *out_size)
{
  int file_size;
  char *file_content;
  FILE *file;

  if (!(file = fopen(path, "r")))
    return NULL;

  fseek(file, 0, SEEK_END);
  file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  file_content = malloc(file_size + 1);
  if (!file_content)
  {
    fclose(file);
    return NULL;
  }

  fread(file_content, 1, file_size, file);
  fclose(file);

  file_content[file_size]  = 0;

  if (out_size)
    *out_size = file_size;

  return file_content;
}


  char *
read_file_binary(const char *path, int *out_size)
{
  int file_size;
  char *file_content;
  FILE *file;

  if (!(file = fopen(path, "rb")))
    return NULL;

  fseek(file, 0, SEEK_END);
  file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  file_content = malloc(file_size + 1);
  if (!file_content)
  {
    fclose(file);
    return NULL;
  }

  fread(file_content, 1, file_size, file);
  fclose(file);
  file_content[file_size] = 0;

  if (out_size)
    *out_size = file_size;
  return file_content;
}

  const char*
get_content_type(const char *file_path)
{
  const char *ext = strrchr(file_path, '.');
  if (ext)
  {
    if (!strcmp(ext, ".pdf"))
      return "application/pdf";
    else if (!strcmp(ext, ".txt"))
      return "text/plain";
  }
  return "application/octet-stream";
}

  void
send_u32(int fd, unsigned value)
{
  value = htonl(value);
  if (write(fd, &value, sizeof(value)) == -1)
    perror("send_int");
}

  void
send_string(int fd, const char *str, unsigned len)
{
  send_u32(fd, len);
  if (write(fd, str, len) == -1)
    perror("send_string");
}

  unsigned
recv_u32(int fd)
{
  unsigned value;
  int nn; 

  nn = read(fd, &value, sizeof value);
  if (nn <= 0)
  {
    errno = 1;
    return 0;
  }
  return ntohl(value);
}

char *
recv_string(int fd)
{
    unsigned len = recv_u32(fd);
    int n;
    char *s;

    s = malloc(len + 1);
    if (!s)
      return NULL;

    n = read(fd, s, len);
    if (n <= 0)
    {
      free(s);
      return NULL;
    }

    s[n] = '\0';
    return s;
}

void 
die(const char *fmt, ...)
{
  va_list va;
  va_start(va, fmt);
  fprintf(stderr, "die: ");
  vfprintf(stderr, fmt, va);
  fprintf(stderr, "\n");
  va_end(va);
  exit(1);
}

void 
diep(const char *fmt, ...)
{
  va_list va;
  va_start(va, fmt);
  fprintf(stderr, "die: ");
  vfprintf(stderr, fmt, va);
  perror("");
  fprintf(stderr, "\n");
  va_end(va);
  exit(1);
}

void 
info(const char *fmt, ...)
{
  va_list va;
  va_start(va, fmt);
  fprintf(stderr, "info: ");
  vfprintf(stderr, fmt, va);
  fprintf(stderr, "\n");
  va_end(va);
}

void 
warn(const char *fmt, ...)
{
  va_list va;
  va_start(va, fmt);
  fprintf(stderr, "warn: ");
  vfprintf(stderr, fmt, va);
  fprintf(stderr, "\n");
  va_end(va);
}

void 
warnp(const char *fmt, ...)
{
  va_list va;
  va_start(va, fmt);
  fprintf(stderr, "warn: ");
  vfprintf(stderr, fmt, va);
  perror("");
  fprintf(stderr, "\n");
  va_end(va);
}

char *
kprintf(const char* fmt, ...)
{
  va_list va;
  char *buffer;
  int size;

  va_start(va, fmt);

  size = vsnprintf(NULL, 0, fmt, va);
  if (size < 0)
  {
    va_end(va);
    return NULL;
  }

  buffer = malloc(size + 1);
  if (!buffer)
  {
    va_end(va);
    return NULL;
  }
  va_end(va);

  va_start(va, fmt);
  vsnprintf(buffer, size + 1, fmt, va);
  va_end(va);

  return buffer;
}


