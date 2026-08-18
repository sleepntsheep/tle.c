#ifndef TLE_USER_H
#define TLE_USER_H

#include <stddef.h>

int user_exists(const char *);
int user_valid_auth(char *username, char *password);
int user_register(const char *, const char *);
int user_fetch_id_by_username(const char *);
int user_fetch_username_by_id(int, char *, size_t);
int session_create(int, char[65], char[65]);
int session_lookup(const char *, int *, char[65]);
void session_destroy(const char *);

#endif
