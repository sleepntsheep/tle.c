#ifndef TLE_DB_H
#define TLE_DB_H

#include <sqlite3.h>

extern sqlite3 *db;

void
db_init(void);

void
db_cleanup(void);

void
db_lock(void);

void
db_unlock(void);

#endif
