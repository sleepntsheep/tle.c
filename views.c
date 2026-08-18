#include "views.h"
#include "html.h"
#include "common.h"

static sds
begin(sds out, const char *username, const char *title)
{
  return html_page_header(out, username, title);
}

static sds
end(sds out)
{
  return html_page_footer(out);
}

static sds
task_list_table(sds out, const struct task_list_item *items, size_t count)
{
  out = sdscat(out, "<table><thead><tr><th></th><th>ID</th><th>Name</th></tr></thead><tbody>");
  for (size_t i = 0; i < count; ++i)
  {
    out = sdscatprintf(out, "<tr><td>%s</td><td>%u</td><td><a href=\"?page=task&amp;pk=%u\">",
        items[i].solved ? "✓" : "", items[i].pk, items[i].pk);
    out = html_escape_text(out, items[i].name);
    out = sdscat(out, "</a></td></tr>");
  }
  return sdscat(out, "</tbody></table>");
}

sds
view_front_page(sds out, const char *username,
    const struct task_list_item *items, size_t count)
{
  out = begin(out, username, "tle grader");
  out = sdscat(out, "<h1>tle grader</h1>");
  out = sdscat(out, FRONT_PAGE);
  out = task_list_table(out, items, count);
  out = sdscat(out, "<p><a href=\"?page=tasks\">All tasks &rarr;</a></p>");
  return end(out);
}

sds
view_task_list(sds out, const char *username,
    const struct task_list_item *items, size_t count)
{
  out = begin(out, username, "tasks");
  out = sdscat(out, "<form method=\"get\"><input type=\"hidden\" name=\"page\" value=\"tasks\">"
      "<label>Search <input name=\"q\" placeholder=\"task name\"></label>"
      "<label>Difficulty <select name=\"difficulty\"><option value=\"\">all</option><option>easy</option><option>medium</option><option>hard</option></select></label>"
      "<label>Tag <input name=\"tag\" placeholder=\"graph, dp, math\"></label>"
      "<label>Show <select name=\"state\"><option value=\"\">all tasks</option><option value=\"solved\">solved</option><option value=\"tried\">tried but unsolved</option><option value=\"bookmarked\">bookmarked</option></select></label>"
      "<button type=\"submit\">Filter</button></form>");
  out = sdscat(out, "<table><thead><tr><th></th><th>ID</th><th>Task</th><th>Difficulty</th><th>Topics</th><th>Stats</th><th></th></tr></thead><tbody>");
  for (size_t i = 0; i < count; ++i)
  {
    out = sdscatprintf(out, "<tr><td>%s</td><td>%u</td><td><a href=\"?page=task&amp;pk=%u\">",
        items[i].solved ? "✓" : items[i].tried ? "·" : "", items[i].pk, items[i].pk);
    out = html_escape_text(out, items[i].name);
    out = sdscat(out, "</a></td><td>"); out = html_escape_text(out, items[i].difficulty);
    out = sdscat(out, "</td><td>"); out = html_escape_text(out, items[i].tags);
    out = sdscat(out, "</td><td>"); out = html_escape_text(out, items[i].source);
    out = sdscat(out, "</td><td>");
    if (username && *username)
      out = sdscatprintf(out, "<form class=\"inline-form\" action=\"?page=bookmark&amp;pk=%u\" method=\"post\"><button type=\"submit\">%s</button></form>", items[i].pk, items[i].bookmarked ? "★" : "☆");
    out = sdscat(out, "</td></tr>");
  }
  out = sdscat(out, "</tbody></table>");
  return end(out);
}

sds
view_submission_list(sds out, const char *username,
    const struct submission_row *rows, size_t count, unsigned last_id)
{
  out = begin(out, username, "submissions");
  out = sdscat(out, "<form method=\"get\"><input type=\"hidden\" name=\"page\" value=\"submissions\">"
      "<label>User <input name=\"user\"></label>"
      "<label>Task <input name=\"task\" type=\"number\" min=\"1\"></label>"
      "<label>Verdict <input name=\"verdict\"></label>"
      "<label><input type=\"checkbox\" name=\"mine\"> My submissions</label>"
      "<button type=\"submit\">Filter</button></form>");
  out = sdscat(out, "<table><thead><tr><th>id</th><th>submission time</th>"
                    "<th>username</th><th>task</th><th>verdict</th>"
                    "<th>score</th><th>time used (ms)</th><th>memory used (kib)</th></tr></thead><tbody>");
  for (size_t i = 0; i < count; ++i)
  {
    out = sdscatprintf(out, "<tr><td><a href=\"?page=submission&amp;id=%u\">%u</a></td>",
          rows[i].submission_id, rows[i].submission_id);
    out = sdscat(out, "<td>"); out = html_escape_text(out, rows[i].submission_time);
    out = sdscat(out, "</td><td>"); out = html_escape_text(out, rows[i].username);
    out = sdscat(out, "</td><td>"); out = html_escape_text(out, rows[i].task_name);
    out = sdscat(out, "</td><td>"); out = html_escape_text(out, rows[i].verdict);
    if (rows[i].verdict_message[0] && strcmp(rows[i].verdict_message, rows[i].verdict))
    {
      out = sdscat(out, "<small> ");
      out = html_escape_text(out, rows[i].verdict_message);
      out = sdscat(out, "</small>");
    }
    out = sdscatprintf(out, "</td><td>%.2f</td><td>%u</td><td>%u</td></tr>",
        rows[i].score,
        rows[i].time_used, rows[i].memory_used);
  }
  out = sdscat(out, "</tbody></table><p class=\"actions\">Show ");
  out = sdscat(out, "<a class=\"button\" href=\"?page=submissions&amp;count=20\">20</a> ");
  out = sdscat(out, "<a class=\"button\" href=\"?page=submissions&amp;count=50\">50</a> ");
  out = sdscat(out, "<a class=\"button\" href=\"?page=submissions&amp;count=100\">100</a> lines</p>");
  if (count)
    out = sdscatprintf(out, "<a href=\"?page=submissions&amp;from=%u\">Previous page</a>", last_id);
  return end(out);
}

sds
view_submission_detail(sds out, const char *username,
    const struct submission_detail *submission)
{
  out = begin(out, username, "submission detail");
  out = sdscatprintf(out, "<h1>Submission %u</h1><dl>", submission->submission_id);
  out = sdscat(out, "<dt>User</dt><dd>");
  out = html_escape_text(out, submission->username);
  out = sdscat(out, "</dd><dt>Task</dt><dd>");
  out = html_escape_text(out, submission->task_name);
  out = sdscat(out, "</dd><dt>Verdict</dt><dd>");
  out = html_escape_text(out, submission->verdict);
  if (submission->verdict_message[0] && strcmp(submission->verdict_message, submission->verdict))
  {
    out = sdscat(out, " — ");
    out = html_escape_text(out, submission->verdict_message);
  }
  out = sdscatprintf(out, "</dd><dt>Score</dt><dd>%.2f</dd>"
      "<dt>Time</dt><dd>%u ms</dd><dt>Memory</dt><dd>%u KiB</dd>",
      submission->score, submission->time_used, submission->memory_used);
  if (submission->failed_case >= 0)
    out = sdscatprintf(out, "<dt>Failed case</dt><dd>%d</dd>", submission->failed_case);
  out = sdscat(out, "<dt>Submitted</dt><dd>");
  out = html_escape_text(out, submission->submission_time);
  out = sdscat(out, "</dd></dl>");

  if (submission->is_public)
    out = sdscatprintf(out, "<p class=\"actions\"><a class=\"button\" href=\"?page=view_code&amp;id=%u\">View submitted code</a></p>",
        submission->submission_id);
  out = sdscatprintf(out,
      "<form action=\"?page=admin\" method=\"post\"><input type=\"hidden\" name=\"rejudge\" value=\"%u\"><button type=\"submit\">Admin rejudge</button></form>",
      submission->submission_id);
  if (submission->compiler_output[0])
  {
    out = sdscat(out, "<h2>Compiler output</h2><pre><code>");
    out = html_escape_text(out, submission->compiler_output);
    out = sdscat(out, "</code></pre>");
  }
  return end(out);
}

sds
view_admin(sds out, const char *username,
    const struct admin_task_item *items, size_t count)
{
  out = begin(out, username, "admin page");
  out = sdscat(out, "<table><thead><tr><th>Visibility</th><th>ID</th><th>Name</th></tr></thead><tbody>");
  for (size_t i = 0; i < count; ++i)
  {
    out = sdscatprintf(out, "<tr><td><form action=\"?page=admin\" method=\"post\"><input type=\"hidden\" name=\"toggle_hidden\" value=\"%u\"><button type=\"submit\">%s</button></form></td><td>%u</td><td><a href=\"?page=task&amp;pk=%u\">",
        items[i].pk, items[i].hidden ? "unhide" : "hide", items[i].pk, items[i].pk);
    out = html_escape_text(out, items[i].name);
    out = sdscat(out, "</a></td></tr>");
  }
  out = sdscat(out, "</tbody></table>");
  return end(out);
}

sds
view_task(sds out, const char *username, const struct task *task,
    const char *statement, int statement_pdf)
{
  out = begin(out, username, "task");
  out = sdscat(out, "<h1>Task "); out = sdscatprintf(out, "%u", task->pk);
  out = sdscat(out, "</h1><p>"); out = html_escape_text(out, task->name);
  out = sdscat(out, "<link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.css\">");
  out = sdscatprintf(out, " (%u kilobytes, %u milliseconds)</p>"
      "<p>Submissions: %u · Accepted: %u</p>"
      "<p class=\"actions\"><a class=\"button\" href=\"?page=submit&amp;pk=%u\">Submit code</a></p>",
      task->memory_limit, task->time_limit, task->submission_count,
      task->accepted_count, task->pk);
  out = sdscat(out, "<dl><dt>Difficulty</dt><dd>");
  out = html_escape_text(out, task->difficulty[0] ? task->difficulty : "unknown");
  out = sdscat(out, "</dd><dt>Topics</dt><dd>");
  out = html_escape_text(out, task->tags[0] ? task->tags : "—");
  if (task->source[0]) { out = sdscat(out, "</dd><dt>Source</dt><dd>"); out = html_escape_text(out, task->source); }
  if (task->estimated_minutes) out = sdscatprintf(out, "</dd><dt>Estimated time</dt><dd>%u minutes", task->estimated_minutes);
  out = sdscat(out, "</dd></dl>");
  out = sdscat(out, "<h2>Statement</h2>");
  if (statement_pdf)
    out = sdscatprintf(out, "<iframe class=\"statement-frame\" title=\"Task statement\" src=\"?page=desc_file&amp;pk=%u\"></iframe>", task->pk);
  else
  {
    out = sdscat(out, "<article class=\"statement\">");
    out = html_markdown(out, statement ? statement : "The statement is unavailable.");
    out = sdscat(out, "</article><script defer src=\"https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.js\"></script>"
        "<script defer src=\"https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/contrib/auto-render.min.js\"></script>"
        "<script>window.addEventListener('load',function(){if(typeof renderMathInElement==='function')renderMathInElement(document.querySelector('.statement'),{delimiters:[{left:'$$',right:'$$',display:true},{left:'\\\\[',right:'\\\\]',display:true},{left:'\\\\(',right:'\\\\)',display:false},{left:'$',right:'$',display:false}]});});</script>");
  }
  if (task->sample_input[0])
  {
    out = sdscat(out, "<h2>Sample test</h2><div><strong>Input</strong><pre>");
    out = html_escape_text(out, task->sample_input);
    out = sdscat(out, "</pre>");
    if (task->sample_output[0])
    {
      out = sdscat(out, "<strong>Output</strong><pre>");
      out = html_escape_text(out, task->sample_output);
      out = sdscat(out, "</pre>");
    }
    else if (!strcmp(task->comparison, "special"))
      out = sdscat(out, "<p class=\"hint\">Output is validated by a special checker; any valid answer is accepted.</p>");
    out = sdscat(out, "</div>");
  }
  return end(out);
}

sds
view_statement(sds out, const char *username, const struct task *task, const char *markdown)
{
  out = begin(out, username, "task statement");
  out = sdscat(out, "<link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.css\">");
  out = sdscatprintf(out, "<h1>Task %u: ", task->pk);
  out = html_escape_text(out, task->name);
  out = sdscat(out, "</h1><article class=\"statement\">");
  out = html_markdown(out, markdown);
  out = sdscat(out, "</article><script defer src=\"https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/katex.min.js\"></script>"
      "<script defer src=\"https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/contrib/auto-render.min.js\"></script>"
      "<script>window.addEventListener('load',function(){if(typeof renderMathInElement==='function')renderMathInElement(document.querySelector('.statement'),{delimiters:[{left:'$$',right:'$$',display:true},{left:'\\\\[',right:'\\\\]',display:true},{left:'\\\\(',right:'\\\\)',display:false},{left:'$',right:'$',display:false}]});});</script>");
  return end(out);
}

sds
view_pdf_statement(sds out, const char *username, const struct task *task)
{
  out = begin(out, username, "task statement");
  out = sdscatprintf(out, "<h1>Task %u: ", task->pk);
  out = html_escape_text(out, task->name);
  out = sdscatprintf(out, "</h1><iframe class=\"statement-frame\" title=\"Task statement\" src=\"?page=desc_file&amp;pk=%u\"></iframe>", task->pk);
  return end(out);
}

sds
view_submit(sds out, const char *username, unsigned task_pk)
{
  out = begin(out, username, "submit");
  out = sdscatprintf(out,
      "<h1>Submit task %u</h1><p>The code must not be longer than %u bytes.</p>"
      "<form action=\"?page=submit&amp;pk=%u\" method=\"post\" enctype=\"multipart/form-data\">"
      "<input type=\"hidden\" name=\"task_pk\" value=\"%u\">"
      "<label>Paste code <textarea name=\"code\" rows=\"18\" cols=\"80\" placeholder=\"Paste C or C++ code here\"></textarea></label>"
      "<p class=\"hint\">Or choose a source file instead. Use one submission method at a time.</p>"
      "<label>Code file <input type=\"file\" name=\"file\" accept=\".c,.cpp\"></label>"
      "<label><input type=\"checkbox\" name=\"is_public\" checked> Make code public</label>"
      "<label><input type=\"checkbox\" name=\"is_anonymous\"> Submit anonymously</label>"
      "<button type=\"submit\">Upload</button></form>",
      task_pk, MAX_CODE_BYTES, task_pk, task_pk);
  return end(out);
}

sds
view_register(sds out, const char *username)
{
  out = begin(out, username, "register");
  out = sdscat(out, "<h1>Register</h1><p>Username and password must be less than 32 and 256 bytes.</p>"
      "<form action=\"?page=register\" method=\"post\">"
      "<label>Username <input type=\"text\" name=\"username\" required></label>"
      "<label>Password <input type=\"password\" name=\"password\" required></label>"
      "<button type=\"submit\">Register</button></form>");
  return end(out);
}

sds
view_login(sds out, const char *username)
{
  out = begin(out, username, "login");
  out = sdscat(out, "<h1>Login</h1>"
      "<form action=\"?page=login\" method=\"post\">"
      "<label>Username <input type=\"text\" name=\"username\" required></label>"
      "<label>Password <input type=\"password\" name=\"password\" required></label>"
      "<button type=\"submit\">Login</button></form>");
  return end(out);
}

sds
view_leaderboard(sds out, const char *username,
    const struct leaderboard_row *rows, size_t count)
{
  out = begin(out, username, "leaderboard");
  out = sdscat(out, "<h1>Leaderboard</h1><table><thead><tr>"
      "<th>Rank</th><th>User</th><th>Solved</th><th>Total score</th>"
      "</tr></thead><tbody>");
  for (size_t i = 0; i < count; ++i)
  {
    out = sdscatprintf(out, "<tr><td>%zu</td><td>", i + 1);
    out = html_escape_text(out, rows[i].username);
    out = sdscatprintf(out, "</td><td>%d</td><td>%.2f</td></tr>",
        rows[i].solved, rows[i].total_score);
  }
  out = sdscat(out, "</tbody></table>");
  return end(out);
}

sds
view_code(sds out, const char *username, unsigned id, const char *code)
{
  out = begin(out, username, "submission code");
  out = sdscatprintf(out, "<h1>Submission %u</h1><pre><code>", id);
  out = html_escape_text(out, code);
  out = sdscat(out, "</code></pre>");
  return end(out);
}
