#include "html.h"
#include "common.h"

sds
html_escape_text(sds out, const char *text)
{
  if (!text)
    return out;

  for (const char *p = text; *p; ++p)
  {
    switch (*p)
    {
      case '&': out = sdscat(out, "&amp;"); break;
      case '<': out = sdscat(out, "&lt;"); break;
      case '>': out = sdscat(out, "&gt;"); break;
      case '"': out = sdscat(out, "&quot;"); break;
      case '\'': out = sdscat(out, "&#x27;"); break;
      default: out = sdscatlen(out, p, 1); break;
    }
  }
  return out;
}

sds
html_page_header(sds out, const char *username, const char *title)
{
  out = sdscat(out, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">");
  out = sdscat(out, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
  out = sdscat(out, "<title>");
  out = html_escape_text(out, title ? title : "tle grader");
  out = sdscat(out, "</title><style>"
      "body{font-family:sans-serif;max-width:60rem;margin:2rem auto;padding:0 1rem;line-height:1.5}"
      "table{border-collapse:collapse}th,td{padding:.25rem .75rem;text-align:left;border-bottom:1px solid #ddd}"
      "nav ul{list-style:none;display:flex;gap:1rem;padding:0}"
      "a{color:#0366d6}"
      "</style></head><body>");
  out = sdscat(out, "<header><nav><ul>");
  if (username && *username)
  {
    out = sdscat(out, "<li>hello ");
    out = html_escape_text(out, username);
    out = sdscat(out, "</li>");
  }
  out = sdscat(out,
      "<li><a href=\"/\">frontpage</a></li>"
      "<li><a href=\"?page=tasks\">tasks</a></li>"
      "<li><a href=\"?page=submissions\">submissions</a></li>"
      "<li><a href=\"?page=leaderboard\">leaderboard</a></li>"
      "<li><a href=\"?page=register\">register</a></li>"
      "<li><a href=\"?page=login\">login</a></li>"
      "<li><a href=\"?page=logout\">logout</a></li>"
      "</ul></nav></header><main>");
  return out;
}

sds
html_page_footer(sds out)
{
  time_t rawtime;
  struct tm *local;
  char buffer[128];

  out = sdscat(out, "</main><footer><p>");
  time(&rawtime);
  local = localtime(&rawtime);
  if (local && strftime(buffer, sizeof buffer, "server time — %x %I:%M%p", local))
    out = sdscat(out, buffer);
  out = sdscat(out,
      "</p><p>For problems or task submissions, contact the administrator.</p>"
      "</footer></body></html>");
  return out;
}
