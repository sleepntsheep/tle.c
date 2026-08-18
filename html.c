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
      ":root{color-scheme:light;--ink:#111;--muted:#666;--line:#cfcfcf;--paper:#fff;--wash:#f6f6f3}"
      "*{box-sizing:border-box}"
      "body{font-family:sans-serif;max-width:64rem;margin:0 auto;padding:0 1.25rem;line-height:1.5;color:var(--ink);background:var(--wash)}"
      "header{padding:1.25rem 0;border-bottom:1px solid var(--ink);margin-bottom:2rem}"
      "nav ul{list-style:none;display:flex;flex-wrap:wrap;align-items:center;gap:.5rem;margin:0;padding:0}"
      "nav li:first-child{margin-right:auto;font-weight:600}"
      "nav a,.button{display:inline-block;padding:.35rem .65rem;border:1px solid var(--ink);border-radius:0;color:var(--ink);background:var(--paper);text-decoration:none}"
      "nav a:hover,.button:hover,button:hover{color:var(--paper);background:var(--ink)}"
      "a{color:var(--ink);text-decoration-thickness:1px;text-underline-offset:.15em}"
      "h1{font-size:1.7rem;line-height:1.2;margin:0 0 1.25rem}h2{font-size:1.15rem;margin-top:2rem}"
      "p{margin:0 0 1rem}.actions{display:flex;flex-wrap:wrap;gap:.5rem;margin:1.25rem 0}"
      ".hint{padding:.75rem 1rem;border-left:2px solid var(--ink);color:var(--muted);background:#fafafa}"
      "table{width:100%;border-collapse:collapse;background:var(--paper);margin:1.25rem 0 1.5rem}"
      "th,td{padding:.65rem .75rem;text-align:left;border-bottom:1px solid var(--line);vertical-align:top}"
      "th{font-size:.8rem;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);font-weight:600}"
      "tbody tr:hover{background:#fafafa}"
      "form{display:flex;flex-wrap:wrap;align-items:flex-end;gap:.85rem;padding:1rem;margin:1.25rem 0;background:var(--paper);border:1px solid var(--line)}"
      "label{display:flex;flex-direction:column;gap:.25rem;font-size:.9rem}"
      "label:has(input[type=checkbox]){flex-direction:row;align-items:center;margin-bottom:.45rem}"
      "input{font:inherit;min-height:2.2rem;padding:.35rem .5rem;border:1px solid var(--ink);border-radius:0;background:var(--paper);color:var(--ink)}"
      "input[type=checkbox]{min-height:0;accent-color:var(--ink)}"
      "button{font:inherit;min-height:2.2rem;padding:.35rem .75rem;border:1px solid var(--ink);border-radius:0;background:var(--ink);color:var(--paper);cursor:pointer}"
      "pre{overflow:auto;padding:1rem;border:1px solid var(--line);background:var(--paper)}"
      "dl{display:grid;grid-template-columns:max-content 1fr;gap:.45rem 1.5rem;padding:1rem;background:var(--paper);border:1px solid var(--line)}"
      "dt{font-weight:600;color:var(--muted)}dd{margin:0}"
      "footer{margin-top:3rem;padding:1rem 0;border-top:1px solid var(--line);color:var(--muted);font-size:.85rem}"
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
