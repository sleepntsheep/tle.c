#include <crypt.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include "common.h"
#include "user.h"
#include "md5.h"

static int
secure_equal(const char *a, const char *b)
{
  size_t a_len = strlen(a), b_len = strlen(b), n = a_len > b_len ? a_len : b_len;
  unsigned char diff = (unsigned char)(a_len ^ b_len);
  for (size_t i = 0; i < n; ++i)
  {
    unsigned char ac = i < a_len ? (unsigned char)a[i] : 0;
    unsigned char bc = i < b_len ? (unsigned char)b[i] : 0;
    diff |= (unsigned char)(ac ^ bc);
  }
  return diff == 0;
}

static char *
hash_password(const char *password)
{
  static const char alphabet[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  unsigned char random_bytes[16];
  char salt[32] = "$6$";
  struct crypt_data crypt_buffer;
  int random_fd = open("/dev/urandom", O_RDONLY), offset = 3;
  ssize_t got = 0;

  if (random_fd < 0)
    return NULL;
  while (got < (ssize_t)sizeof random_bytes)
  {
    ssize_t n = read(random_fd, random_bytes + got, sizeof random_bytes - (size_t)got);
    if (n <= 0)
    {
      close(random_fd);
      return NULL;
    }
    got += n;
  }
  close(random_fd);
  for (int i = 0; i < 16; ++i)
    salt[offset++] = alphabet[random_bytes[i] % (sizeof alphabet - 1)];
  salt[offset] = 0;

  memset(&crypt_buffer, 0, sizeof crypt_buffer);
  char *hash = crypt_r(password, salt, &crypt_buffer);
  return hash ? strdup(hash) : NULL;
}

static void
legacy_md5(const char *password, char output[33])
{
  MD5_CTX context;
  MD5Init(&context);
  MD5Update(&context, password, strlen(password));
  MD5Final(&context);
  MD5String(&context, output);
  output[32] = 0;
}

int
user_exists(const char *username)
{
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE username = ?1", -1, &stmt, NULL);
  if (rc != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

int
user_register(const char *username, const char *password)
{
  sqlite3_stmt *stmt;
  char *password_hash;
  int rc;

  if (!username || !password || !username[0] || !password[0] ||
      strlen(username) > 31 || strlen(password) > 255)
    return -1;
  if (user_exists(username) == 1)
    return -2;
  password_hash = hash_password(password);
  if (!password_hash)
    return -1;

  rc = sqlite3_prepare_v2(db,
      "INSERT INTO users(username, hashed_password) VALUES(?1, ?2)", -1, &stmt, NULL);
  if (rc == SQLITE_OK)
  {
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password_hash, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  free(password_hash);
  if (rc == SQLITE_CONSTRAINT)
    return -2;
  if (rc != SQLITE_DONE)
  {
    warn("insert user failed: %s", sqlite3_errmsg(db));
    return -1;
  }
  return 0;
}

int
user_valid_auth(char *username, char *password)
{
  sqlite3_stmt *stmt;
  const char *stored;
  char computed[33];
  int rc, valid = 0, user_id;

  if (!username || !password)
    return 0;
  rc = sqlite3_prepare_v2(db,
      "SELECT user_id, hashed_password FROM users WHERE username = ?1", -1, &stmt, NULL);
  if (rc != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
  }

  user_id = sqlite3_column_int(stmt, 0);
  stored = (const char *)sqlite3_column_text(stmt, 1);
  if (stored && strncmp(stored, "$6$", 3) == 0)
  {
    struct crypt_data crypt_buffer;
    memset(&crypt_buffer, 0, sizeof crypt_buffer);
    char *hash = crypt_r(password, stored, &crypt_buffer);
    valid = hash && secure_equal(hash, stored);
  }
  else if (stored)
  {
    legacy_md5(password, computed);
    valid = secure_equal(computed, stored);
  }

  if (valid && stored && strncmp(stored, "$6$", 3) != 0)
  {
    char *new_hash = hash_password(password);
    if (new_hash)
    {
      sqlite3_stmt *update;
      if (sqlite3_prepare_v2(db, "UPDATE users SET hashed_password = ?1 WHERE user_id = ?2",
            -1, &update, NULL) == SQLITE_OK)
      {
        sqlite3_bind_text(update, 1, new_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(update, 2, user_id);
        sqlite3_step(update);
        sqlite3_finalize(update);
      }
      free(new_hash);
    }
  }
  sqlite3_finalize(stmt);
  return valid;
}

int
user_fetch_id_by_username(const char *username)
{
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, "SELECT user_id FROM users WHERE username = ?1", -1, &stmt, NULL);
  if (rc != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    return -1;
  }
  int id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return id;
}

static int
random_hex(char *out, size_t out_size)
{
  static const char hex[] = "0123456789abcdef";
  unsigned char bytes[32];
  int fd = open("/dev/urandom", O_RDONLY);
  size_t got = 0;
  if (fd < 0 || out_size < sizeof bytes * 2 + 1)
  {
    if (fd >= 0) close(fd);
    return -1;
  }
  while (got < sizeof bytes)
  {
    ssize_t n = read(fd, bytes + got, sizeof bytes - got);
    if (n <= 0)
    {
      close(fd);
      return -1;
    }
    got += (size_t)n;
  }
  close(fd);
  for (size_t i = 0; i < sizeof bytes; ++i)
  {
    out[i * 2] = hex[bytes[i] >> 4];
    out[i * 2 + 1] = hex[bytes[i] & 15];
  }
  out[sizeof bytes * 2] = 0;
  return 0;
}

int
user_fetch_username_by_id(int user_id, char *out, size_t out_size)
{
  sqlite3_stmt *stmt;
  if (!out || out_size == 0 || sqlite3_prepare_v2(db,
        "SELECT username FROM users WHERE user_id=?1", -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(stmt, 1, user_id);
  if (sqlite3_step(stmt) != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    return -1;
  }
  snprintf(out, out_size, "%s", sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return 0;
}

int
session_create(int user_id, char token[65], char csrf[65])
{
  sqlite3_stmt *stmt;
  if (random_hex(token, 65) != 0 || random_hex(csrf, 65) != 0)
    return -1;
  if (sqlite3_prepare_v2(db,
        "INSERT INTO sessions(token,user_id,csrf_token) VALUES(?1,?2,?3)",
        -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, user_id);
  sqlite3_bind_text(stmt, 3, csrf, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

int
session_lookup(const char *token, int *user_id, char csrf[65])
{
  sqlite3_stmt *stmt;
  if (!token || !*token || !user_id || sqlite3_prepare_v2(db,
        "SELECT user_id,csrf_token FROM sessions WHERE token=?1", -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    return 0;
  }
  *user_id = sqlite3_column_int(stmt, 0);
  if (csrf)
    snprintf(csrf, 65, "%s", sqlite3_column_text(stmt, 1));
  sqlite3_finalize(stmt);
  return 1;
}

void
session_destroy(const char *token)
{
  sqlite3_stmt *stmt;
  if (!token || !*token || sqlite3_prepare_v2(db,
        "DELETE FROM sessions WHERE token=?1", -1, &stmt, NULL) != SQLITE_OK)
    return;
  sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}
