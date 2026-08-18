#ifndef TLE_HTML_H
#define TLE_HTML_H

#include "sds.h"

sds html_escape_text(sds out, const char *text);
sds html_page_header(sds out, const char *username, const char *title);
sds html_page_footer(sds out);

#endif
