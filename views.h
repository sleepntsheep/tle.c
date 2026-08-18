#ifndef TLE_VIEWS_H
#define TLE_VIEWS_H

#include "sds.h"
#include "task.h"

sds view_front_page(sds out, const char *username,
    const struct task_list_item *items, size_t count);
sds view_task_list(sds out, const char *username,
    const struct task_list_item *items, size_t count);
sds view_submission_list(sds out, const char *username,
    const struct submission_row *rows, size_t count, unsigned last_id);
sds view_submission_detail(sds out, const char *username,
    const struct submission_detail *submission);
sds view_admin(sds out, const char *username,
    const struct admin_task_item *items, size_t count);
sds view_task(sds out, const char *username, const struct task *task);
sds view_statement(sds out, const char *username, const struct task *task, const char *markdown);
sds view_pdf_statement(sds out, const char *username, const struct task *task);
sds view_submit(sds out, const char *username, unsigned task_pk);
sds view_register(sds out, const char *username);
sds view_login(sds out, const char *username);
sds view_leaderboard(sds out, const char *username,
    const struct leaderboard_row *rows, size_t count);
sds view_code(sds out, const char *username, unsigned id, const char *code);

#endif
