#include "common.h"
#include "user.h"
#include <string.h>
#include <microhttpd.h>
#include <unistd.h>
#include <pthread.h>
#include "html.h"
#include "views.h"


/* prototypes */
static void check_features();
static void cleanup(int sig);
static enum MHD_Result basic_response(struct MHD_Connection *, char *, int, enum MHD_ResponseMemoryMode);
static enum MHD_Result basic_response_sds(struct MHD_Connection *, sds, int);
enum MHD_Result post_iterator(void *, enum MHD_ValueKind, const char *, const char *, const char *, const char *, const char *, uint64_t, size_t);
static enum MHD_Result ahc_echo(void *, struct MHD_Connection *, const char *, const char *, const char *, const char *, size_t *, void **);
static enum MHD_Result response_not_found(struct MHD_Connection *);
static enum MHD_Result response_internal_server_error(struct MHD_Connection *);
static void add_safety_headers(struct MHD_Response *);

static const char *
base_url(void)
{
  const char *configured = getenv("TLE_BASE_URL");
  return configured && *configured ? configured : BASE_URL;
}

static int
same_origin_request(struct MHD_Connection *connection)
{
  const char *origin = MHD_lookup_connection_value(connection,
      MHD_HEADER_KIND, "Origin");
  const char *referer = MHD_lookup_connection_value(connection,
      MHD_HEADER_KIND, "Referer");
  const char *value = origin ? origin : referer;
  if (!value)
    return 0;
  const char *base = base_url();
  const char *base_authority = strstr(base, "://");
  const char *value_authority = strstr(value, "://");
  if (!base_authority || !value_authority) return 0;
  base_authority += 3;
  value_authority += 3;
  size_t base_len = strcspn(base_authority, "/");
  size_t value_len = strcspn(value_authority, "/");
  return base_len == value_len && !strncmp(base_authority, value_authority, base_len) &&
      !strncmp(base, value, (size_t)(base_authority - base));
}

static int
secure_cookie_enabled(void)
{
  return !strncmp(base_url(), "https://", 8);
}

static int
csrf_valid(int session_authenticated, const char *session_csrf,
    const char *submitted_csrf)
{
  return session_authenticated && session_csrf && submitted_csrf &&
      strlen(session_csrf) == 64 && strlen(submitted_csrf) == 64 &&
      !strcmp(session_csrf, submitted_csrf);
}

static int
cookie_value(const char *cookie, const char *name, char *out, size_t out_size)
{
  size_t name_len = strlen(name);
  const char *p = cookie;
  if (!cookie || !out || out_size == 0)
    return 0;
  while (*p)
  {
    while (*p == ' ' || *p == ';') ++p;
    if (!strncmp(p, name, name_len) && p[name_len] == '=')
    {
      const char *value = p + name_len + 1;
      const char *end = strchr(value, ';');
      size_t length = end ? (size_t)(end - value) : strlen(value);
      if (length == 0 || length >= out_size)
        return 0;
      memcpy(out, value, length);
      out[length] = 0;
      return 1;
    }
    p = strchr(p, ';');
    if (!p) break;
  }
  return 0;
}

/* types */
struct connection_info
{
  struct MHD_PostProcessor *pp;

  struct
  {
    sds code;
    int task_pk;
    int judge_id;
    int is_public;
    int is_anonymous;
    int file_field_seen;
    int code_field_seen;
  } submission;

  char csrf[65];

  char username[32];
  char password[256];
  char admin_action[16];
  unsigned admin_id;
};

/* global variables */
struct MHD_Daemon *d;
static char *tls_cert_material;
static char *tls_key_material;
static pthread_mutex_t anonymous_submit_mutex = PTHREAD_MUTEX_INITIALIZER;
struct anonymous_rate_entry
{
  char address[INET6_ADDRSTRLEN];
  time_t last_submission;
};
static struct anonymous_rate_entry anonymous_rates[256];

static void
client_address(struct MHD_Connection *connection, char address[INET6_ADDRSTRLEN])
{
  const union MHD_ConnectionInfo *info = MHD_get_connection_info(connection,
      MHD_CONNECTION_INFO_CLIENT_ADDRESS);
  address[0] = 0;
  if (!info || !info->client_addr)
  {
    snprintf(address, INET6_ADDRSTRLEN, "unknown");
    return;
  }
  if (!inet_ntop(info->client_addr->sa_family, info->client_addr->sa_family == AF_INET ?
        (const void *)&((const struct sockaddr_in *)info->client_addr)->sin_addr :
        (const void *)&((const struct sockaddr_in6 *)info->client_addr)->sin6_addr,
        address, INET6_ADDRSTRLEN))
    snprintf(address, INET6_ADDRSTRLEN, "unknown");
}

static int
anonymous_submission_allowed(struct MHD_Connection *connection)
{
  time_t now = time(NULL);
  char address[INET6_ADDRSTRLEN];
  int allowed;
  client_address(connection, address);
  pthread_mutex_lock(&anonymous_submit_mutex);
  size_t slot = 0;
  for (size_t i = 0; i < countof(anonymous_rates); ++i)
  {
    if (!strcmp(anonymous_rates[i].address, address))
    {
      slot = i;
      break;
    }
    if (!anonymous_rates[i].address[0])
      slot = i;
  }
  allowed = now >= anonymous_rates[slot].last_submission + 5;
  if (allowed)
  {
    snprintf(anonymous_rates[slot].address, sizeof anonymous_rates[slot].address, "%s", address);
    anonymous_rates[slot].last_submission = now;
  }
  pthread_mutex_unlock(&anonymous_submit_mutex);
  return allowed;
}

static void
load_sample_tests(struct task *task)
{
  char input_path[1024], output_path[1024];
  int input_size = 0, output_size = 0;
  char *input, *output;
  task->sample_input[0] = 0;
  task->sample_output[0] = 0;
  if (snprintf(input_path, sizeof input_path, "%s/%s/tests/1.in", TASKS_PATH,
        task->name) >= (int)sizeof input_path ||
      snprintf(output_path, sizeof output_path, "%s/%s/tests/1.sol", TASKS_PATH,
        task->name) >= (int)sizeof output_path)
    return;
  input = read_file(input_path, &input_size);
  output = read_file(output_path, &output_size);
  if (input)
    snprintf(task->sample_input, sizeof task->sample_input, "%.*s",
        input_size > (int)sizeof task->sample_input - 1 ? (int)sizeof task->sample_input - 1 : input_size, input);
  if (output)
    snprintf(task->sample_output, sizeof task->sample_output, "%.*s",
        output_size > (int)sizeof task->sample_output - 1 ? (int)sizeof task->sample_output - 1 : output_size, output);
  free(input);
  free(output);
}

/* definition */
int
main(int argc, char **argv)
{
  int port;
  struct sockaddr_in bind_addr;
  const char *bind_address;

  if (argc >= 2 && !strcmp(argv[1], "--check-config"))
  {
    printf("db=%s\ntasks=%s\nport=%d\nbase_url=%s\n",
        DB_PATH, TASKS_PATH, DEFAULT_WEB_PORT, base_url());
    return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "--check-tasks"))
  {
    db_init();
    read_tasks();
    db_cleanup();
    puts("task assets passed");
    return 0;
  }

  const int cleanup_signals[] = {SIGINT, SIGTERM};

  for (int i = 0; i < countof(cleanup_signals); ++i)
    if (SIG_ERR == signal(cleanup_signals[i], cleanup))
      die("error registering signal handler for SIGTTRM");

  port = argc >= 2 ? atoi(argv[1]) : DEFAULT_WEB_PORT;

  bind_address = getenv("TLE_BIND_ADDRESS");
  if (!bind_address || !*bind_address)
    bind_address = "127.0.0.1";
  memset(&bind_addr, 0, sizeof bind_addr);
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, bind_address, &bind_addr.sin_addr) != 1)
    die("invalid TLE_BIND_ADDRESS");

  info("tle grader starting");

  check_features();
  db_init();
  db_lock();
  read_tasks();
  db_unlock();

  const char *tls_cert_path = getenv("TLE_TLS_CERT");
  const char *tls_key_path = getenv("TLE_TLS_KEY");
  if (tls_cert_path && *tls_cert_path && tls_key_path && *tls_key_path)
  {
    tls_cert_material = read_file(tls_cert_path, NULL);
    tls_key_material = read_file(tls_key_path, NULL);
    if (!tls_cert_material || !tls_key_material)
      die("failed reading TLS certificate or key");
    d = MHD_start_daemon(
      MHD_USE_AUTO | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG | MHD_USE_TLS,
      port, NULL, NULL, &ahc_echo, NULL,
      MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&bind_addr,
      MHD_OPTION_HTTPS_MEM_CERT, tls_cert_material,
      MHD_OPTION_HTTPS_MEM_KEY, tls_key_material,
      MHD_OPTION_PER_IP_CONNECTION_LIMIT, 20u,
      MHD_OPTION_CONNECTION_TIMEOUT, 10u, MHD_OPTION_END);
  }
  else
  {
    d = MHD_start_daemon(
      MHD_USE_AUTO | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
      port, NULL, NULL, &ahc_echo, NULL,
      MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&bind_addr,
      MHD_OPTION_PER_IP_CONNECTION_LIMIT, 20u,
      MHD_OPTION_CONNECTION_TIMEOUT, 10u, MHD_OPTION_END);
  }

  //if (MHD_NO == MHD_run(d))
  //  die("failed  MHD_run");
  info(" running on port %d", port);

  if (d == NULL)
    die("d is null");

  pause();

  die("unreachable!");
}

enum MHD_Result
post_iterator(void *cls, enum MHD_ValueKind kind,
    const char *key, const char *filename, const char *content_type,
    const char *transfer_encoding, const char *data, uint64_t off, size_t size)
{
  struct connection_info *conn;

  (void)off;
  (void)kind;
  (void)transfer_encoding;
  (void)filename;
  (void)content_type;

  conn = cls;
  if (0 == strcmp("task_pk", key))
  {
    char *endptr;
    long num;
    num = strtol(data, &endptr, 10);
    conn->submission.task_pk = num;
    return MHD_YES;
  }
  else if (0 == strcmp("file", key))
  {
    if (size > 0) conn->submission.file_field_seen = 1;
    size_t current_size = conn->submission.code ? sdslen(conn->submission.code) : 0;
    if (current_size > MAX_CODE_BYTES || size > MAX_CODE_BYTES - current_size)
      return MHD_NO;
    if (conn->submission.code == NULL)
      conn->submission.code = sdsempty();
    conn->submission.code = sdscatlen(conn->submission.code, data, size);
    return MHD_YES;
  }
  else if (0 == strcmp("code", key))
  {
    if (size > 0) conn->submission.code_field_seen = 1;
    size_t current_size = conn->submission.code ? sdslen(conn->submission.code) : 0;
    if (current_size > MAX_CODE_BYTES || size > MAX_CODE_BYTES - current_size)
      return MHD_NO;
    if (conn->submission.code == NULL)
      conn->submission.code = sdsempty();
    conn->submission.code = sdscatlen(conn->submission.code, data, size);
    return MHD_YES;
  }
  else if (0 == strcmp("csrf", key))
  {
    if (off > sizeof conn->csrf - 1 || size > sizeof conn->csrf - 1 - (size_t)off)
      return MHD_NO;
    memcpy(conn->csrf + off, data, size);
    conn->csrf[off + size] = 0;
    return MHD_YES;
  }
  else if (0 == strcmp("is_public", key))
  {
    conn->submission.is_public = 1;
    if (data && 0 != strcmp(data, "on"))
      conn->submission.is_public = 0;
    return MHD_YES;
  }
  else if (0 == strcmp("is_anonymous", key))
  {
    conn->submission.is_anonymous = 0;
    if (data && 0 == strcmp(data, "on"))
      conn->submission.is_anonymous = 1;
    return MHD_YES;
  }
  else if (0 == strcmp("username", key))
  {
    if (off > 31 || size > 31 - (size_t)off)
      return MHD_NO;
    memcpy(conn->username + off, data, size);
    conn->username[off + size] = 0;
    return MHD_YES;
  }
  else if (0 == strcmp("password", key))
  {
    if (off > 255 || size > 255 - (size_t)off)
      return MHD_NO;
    memcpy(conn->password + off, data, size);
    conn->password[off + size] = 0;
    return MHD_YES;
  }
  else if (!strcmp("rejudge", key) || !strcmp("toggle_hidden", key))
  {
    char *endptr;
    unsigned long value = strtoul(data, &endptr, 10);
    if (endptr == data || *endptr || value > UINT_MAX)
      return MHD_NO;
    snprintf(conn->admin_action, sizeof conn->admin_action, "%s", key);
    conn->admin_id = (unsigned)value;
    return MHD_YES;
  }
  else if (0 == strcmp("DONE", key))
    return MHD_YES;

  return MHD_NO;
}

static enum MHD_Result
ahc_echo(void * cls,
    struct MHD_Connection * connection,
    const char * url,
    const char * method,
    const char * version,
    const char * upload_data,
    size_t * upload_data_size,
    void ** ptr) {
  struct connection_info *conn;
  char username[32], password[256];
  char session_token[65], csrf_token[65];
  int auth_fail, user_id, auth_result, session_authenticated;

  user_id = -1;
  session_authenticated = 0;
  session_token[0] = 0;
  csrf_token[0] = 0;
  conn = *ptr;

  info("[%s] at %s", method, url);
  (void)password;
  (void)url;
  (void)cls;
  (void)version;

  if (! conn)
  {
    conn = calloc(1, sizeof *conn);
    *ptr = conn;

    if (0 == strcmp(method, "POST"))
    {
      conn->pp = MHD_create_post_processor(connection, 1024, &post_iterator, conn);
      if (NULL == conn->pp)
      {
        free(conn);
        return MHD_NO;
      }
    }
    return MHD_YES;
  }


  {
    char *username_, *password_;
    const char *cookie = MHD_lookup_connection_value(connection,
        MHD_HEADER_KIND, "Cookie");
    password_ = NULL;
    username_ = MHD_basic_auth_get_username_password(connection, &password_);
    db_lock();
    auth_result = 0;
    auth_fail = 1;
    username[0] = 0;
    password[0] = 0;

    if (cookie_value(cookie, "tle_session", session_token, sizeof session_token))
    {
      if (session_lookup(session_token, &user_id, csrf_token) == 1 &&
          user_fetch_username_by_id(user_id, username, sizeof username) == 0)
      {
        auth_fail = 0;
        session_authenticated = 1;
      }
    }

    if (!session_authenticated)
    {
      auth_result = username_ && password_ ? user_valid_auth(username_, password_) : 0;
      auth_fail = auth_result <= 0;
    }
    if (password_ && strlen(password_) < sizeof password)
      snprintf(password, sizeof password, "%s", password_);
    else if (password_)
      auth_fail = 1;
    if (!session_authenticated && !auth_fail)
    {
      if (username_ && strlen(username_) < sizeof username)
        snprintf(username, sizeof username, "%s", username_);
      else
        auth_fail = 1;
    }
    else if (!session_authenticated)
    {
      user_id = -1;
    }
    MHD_free(username_);
    MHD_free(password_);
    if (!session_authenticated && username[0])
      user_id = user_fetch_id_by_username(username);
    db_unlock();
  }

  if (0 == strcmp(method, "POST"))
  {
    struct MHD_Response *response;
    int ret;

    const char *page = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "page");

    if (NULL != upload_data_size && *upload_data_size > 0)
    {
      MHD_post_process(conn->pp, upload_data, *upload_data_size);
      *upload_data_size = 0;
      return MHD_YES;
    }
    MHD_destroy_post_processor(conn->pp);

    if (0 == strcmp(page, "submit"))
    {
      conn->pp = NULL;
      if (session_authenticated && !csrf_valid(session_authenticated, csrf_token, conn->csrf))
        return basic_response(connection, "csrf check failed", MHD_HTTP_FORBIDDEN, MHD_RESPMEM_PERSISTENT);
      if (conn->submission.file_field_seen && conn->submission.code_field_seen)
        return basic_response(connection, "choose either pasted code or a file", MHD_HTTP_BAD_REQUEST, MHD_RESPMEM_PERSISTENT);
      if (NULL == conn->submission.code || sdslen(conn->submission.code) == 0)
        return basic_response(connection, "code can't be empty", MHD_HTTP_BAD_REQUEST, MHD_RESPMEM_PERSISTENT);

      if (conn->submission.is_anonymous)
      {
        if (!anonymous_submission_allowed(connection))
          return basic_response(connection, "anonymous submission rate limit exceeded",
              MHD_HTTP_TOO_MANY_REQUESTS, MHD_RESPMEM_PERSISTENT);
        user_id = -1;
      }

      if (auth_fail && !conn->submission.is_anonymous)
      {
        char DENIED[] = "access denied";
        response = MHD_create_response_from_buffer(strlen(DENIED), DENIED, MHD_RESPMEM_PERSISTENT);
        ret = MHD_queue_basic_auth_fail_response(connection, "realm", response);
        MHD_destroy_response(response);
      }
      else
      {
        db_lock();
        int submission_id = enqueue_submission(conn->submission.is_anonymous ? -1 : user_id,
              conn->submission.task_pk, conn->submission.code,
              conn->submission.is_public);
        db_unlock();
        if (submission_id < 0)
          return response_internal_server_error(connection);

        response = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
        char location[512];
        snprintf(location, sizeof location, "%s?page=submissions", base_url());
        MHD_add_response_header(response, "Location", location);
        ret = MHD_queue_response(connection, MHD_HTTP_FOUND, response);
        MHD_destroy_response(response);
      }
      return ret;
    }
    else if (0 == strcmp(page, "bookmark"))
    {
      const char *pk_text = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "pk");
      unsigned pk;
      if (!csrf_valid(session_authenticated, csrf_token, conn->csrf) || !same_origin_request(connection) || !pk_text)
        return basic_response(connection, "bookmark request denied", MHD_HTTP_FORBIDDEN, MHD_RESPMEM_PERSISTENT);
      pk = strtoul(pk_text, NULL, 10);
      db_lock();
      int bookmark_result = toggle_bookmark(user_id, pk);
      db_unlock();
      if (bookmark_result < 0) return response_internal_server_error(connection);
      response = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
      MHD_add_response_header(response, "Location", base_url());
      ret = MHD_queue_response(connection, MHD_HTTP_FOUND, response);
      MHD_destroy_response(response);
      return ret;
    }
    else if (0 == strcmp(page, "register"))
    {
      db_lock();
      int register_result = user_register(conn->username, conn->password);
      db_unlock();
      switch (register_result)
      {
        case -2:
          return basic_response(connection, "<h1>username already exists!</h1>",
              MHD_HTTP_BAD_REQUEST, MHD_RESPMEM_PERSISTENT);
        case 0:
          return basic_response(connection, "<h1>created account succesfully</h1>",
              MHD_HTTP_OK, MHD_RESPMEM_PERSISTENT);
        case -1:
          return basic_response(connection,
              "<h1>error: make sure username is less than 32 char and password is less than 256 chars </h1>",
              MHD_HTTP_BAD_REQUEST, MHD_RESPMEM_PERSISTENT);
      }
    }
    else if (0 == strcmp(page, "login"))
    {
      char token[65], csrf[65];
      int login_id = -1;
      db_lock();
      if (user_valid_auth(conn->username, conn->password) == 1)
        login_id = user_fetch_id_by_username(conn->username);
      if (login_id >= 0 && session_create(login_id, token, csrf) == 0)
      {
        response = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Location", base_url());
        char cookie[160];
        snprintf(cookie, sizeof cookie, "tle_session=%s; Path=/; HttpOnly; SameSite=Lax%s",
            token, secure_cookie_enabled() ? "; Secure" : "");
        MHD_add_response_header(response, "Set-Cookie", cookie);
        db_unlock();
        ret = MHD_queue_response(connection, MHD_HTTP_FOUND, response);
        MHD_destroy_response(response);
        return ret;
      }
      db_unlock();
      return basic_response(connection, "login failed", MHD_HTTP_UNAUTHORIZED,
          MHD_RESPMEM_PERSISTENT);
    }
    else if (0 == strcmp(page, "admin"))
    {
      const char *admin_password = getenv("TLE_ADMIN_PASSWORD");
      if (!admin_password || !*admin_password || strcmp(password, admin_password))
        return basic_response(connection, "access denied", MHD_HTTP_UNAUTHORIZED,
            MHD_RESPMEM_PERSISTENT);
      if (!same_origin_request(connection))
        return basic_response(connection, "csrf check failed", MHD_HTTP_FORBIDDEN,
            MHD_RESPMEM_PERSISTENT);
      if (!conn->admin_action[0])
        return response_not_found(connection);
      unsigned id = conn->admin_id;
      db_lock();
      int rejudge_result = !strcmp(conn->admin_action, "rejudge") ?
          requeue_submission(id) : task_toggle_visibility(id);
      db_unlock();
      return rejudge_result < 0 ? response_internal_server_error(connection) :
          basic_response(connection, !strcmp(conn->admin_action, "rejudge") ?
              (rejudge_result ? "requeued" : "submission not found") : "toggled",
              !strcmp(conn->admin_action, "rejudge") && !rejudge_result ? MHD_HTTP_NOT_FOUND : MHD_HTTP_OK,
              MHD_RESPMEM_PERSISTENT);
    }
    else
      return MHD_NO;
  }

  else if (0 == strcmp(method, "GET"))
  {
    const char *page;
    page = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "page");
    sds html;

    if (!strcmp(url, "/favicon.svg"))
    {
      struct MHD_Response *favicon = MHD_create_response_from_buffer(
          strlen(html_favicon_svg), (void *)html_favicon_svg, MHD_RESPMEM_PERSISTENT);
      int ret;
      add_safety_headers(favicon);
      MHD_add_response_header(favicon, "Content-Type", "image/svg+xml");
      ret = MHD_queue_response(connection, MHD_HTTP_OK, favicon);
      MHD_destroy_response(favicon);
      return ret;
    }

    if (0 != *upload_data_size)
      return MHD_NO; /* upload data in a GET!? */
    *ptr = NULL; /* clear context pointer */

    if (NULL == page || 0 == strcmp(page, "frontpage"))
    {
      struct task_list_item *items = NULL;
      size_t item_count = 0;

      if (auth_fail)
        user_id = -1;

      db_lock();
      int fetch_result = fetch_tasks(&items, &item_count, user_id, "1");
      db_unlock();
      if (fetch_result != 0)
        return response_internal_server_error(connection);
      html = view_front_page(sdsempty(), username, items, item_count);
      free_tasks(items);
    }
    else if (0 == strcmp(page, "submissions"))
    {
      const char *from, *count, *user_filter, *task_filter, *verdict_filter, *mine;
      unsigned from_number = UINT_MAX, count_number = 20, last_id = 0;
      unsigned task_number = 0;
      struct submission_row *rows = NULL;
      size_t row_count = 0;

      from = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "from");
      count = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "count");
      user_filter = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "user");
      task_filter = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "task");
      verdict_filter = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "verdict");
      mine = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "mine");

      if (mine && auth_fail)
        return response_not_found(connection);
      if (mine)
        user_filter = username;
      if (task_filter && *task_filter)
      {
        char *endptr;
        errno = 0;
        unsigned long parsed = strtoul(task_filter, &endptr, 10);
        if (errno || endptr == task_filter || *endptr || parsed == 0 || parsed > UINT_MAX)
          return response_not_found(connection);
        task_number = (unsigned)parsed;
      }
      if (verdict_filter && *verdict_filter &&
          strcmp(verdict_filter, "accepted") && strcmp(verdict_filter, "wrong_answer") &&
          strcmp(verdict_filter, "time_limit") && strcmp(verdict_filter, "memory_limit") &&
          strcmp(verdict_filter, "runtime_error") && strcmp(verdict_filter, "compilation_error") &&
          strcmp(verdict_filter, "internal_error") && strcmp(verdict_filter, "queued") &&
          strcmp(verdict_filter, "grading"))
        return response_not_found(connection);

      if (count)
      {
        if (strcmp("20", count) && strcmp("50", count) && strcmp("100", count))
          return response_not_found(connection);
        count_number = strtoul(count, NULL, 10);
      }

      if (from)
      {
        char *endptr;

        errno = 0;

        strtoul(from, &endptr, 10);

        if (errno || endptr == from
            || *endptr != 0)
        {
          return response_not_found(connection);
        }
        from_number = strtoul(from, NULL, 10);
      }

      db_lock();
      int fetch_result = fetch_submissions(&rows, &row_count, from_number,
          count_number, &last_id, user_filter, task_number, verdict_filter);
      db_unlock();
      if (fetch_result != 0)
        return response_internal_server_error(connection);
      html = view_submission_list(sdsempty(), username, rows, row_count, last_id);
      free_submissions(rows);
    }
    else if (0 == strcmp(page, "leaderboard"))
    {
      struct leaderboard_row *rows = NULL;
      size_t row_count = 0;
      db_lock();
      int fetch_result = fetch_leaderboard(&rows, &row_count);
      db_unlock();
      if (fetch_result != 0)
        return response_internal_server_error(connection);
      html = view_leaderboard(sdsempty(), username, rows, row_count);
      free_leaderboard(rows);
    }
    else if (0 == strcmp(page, "submission"))
    {
      const char *id_ = MHD_lookup_connection_value(connection,
          MHD_GET_ARGUMENT_KIND, "id");
      char *endptr;
      unsigned id;
      struct submission_detail detail;

      if (!id_ || !*id_)
        return response_not_found(connection);
      errno = 0;
      id = strtoul(id_, &endptr, 10);
      if (errno || endptr == id_ || *endptr != '\0' || id == 0)
        return response_not_found(connection);

      db_lock();
      int fetch_result = fetch_submission_detail(id, &detail);
      db_unlock();
      if (fetch_result != 0)
        return response_not_found(connection);
      html = view_submission_detail(sdsempty(), username, &detail);
    }
    else if (0 == strcmp(page, "view_code"))
    {
      unsigned id;
      sds code;
      const char *id_;

      id_ = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "id");
      if (!id_)
        return MHD_NO;
      id = strtoul(id_, NULL, 10);

      db_lock();
      code = fetch_submission_code_by_id(id);
      db_unlock();
      if (!code)
        return basic_response(connection, "<h1>code is not public or missing(sad)</h1>", MHD_HTTP_NOT_FOUND, MHD_RESPMEM_PERSISTENT);

      html = view_code(sdsempty(), username, id, code);
      sdsfree(code);

    }
    else if (0 == strcmp(page, "tasks"))
    {
      const char *show_solved;
      const char *query = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "q");
      const char *difficulty = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "difficulty");
      const char *tag = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "tag");
      const char *state = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "state");

      if (auth_fail)
        user_id = -1;

      show_solved = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "show_solved");

      if (show_solved)
      {
        if (strcmp(show_solved, "0") && strcmp(show_solved, "1"))
          return response_not_found(connection);
      }
      if (state && strcmp(state, "") && strcmp(state, "solved") && strcmp(state, "tried") && strcmp(state, "bookmarked"))
        return response_not_found(connection);

      struct task_list_item *items = NULL;
      size_t item_count = 0;
      db_lock();
      int fetch_result = fetch_tasks_filtered(&items, &item_count, user_id, show_solved,
          query, difficulty, tag, state);
      db_unlock();
      if (fetch_result != 0)
        return response_internal_server_error(connection);
      html = view_task_list(sdsempty(), username, items, item_count,
          session_authenticated ? csrf_token : NULL);
      free_tasks(items);
    }
    else if (0 == strcmp(page, "task"))
    {
      unsigned pk;
      const char *pk_;
      struct task task;

      pk_ = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "pk");
      if (!pk_)
        return MHD_NO;
      pk = strtoul(pk_, NULL, 10);

      db_lock();
      int task_result = fetch_task_by_pk(pk, &task);
      db_unlock();
      if (0 != task_result)
        return MHD_NO;

      load_sample_tests(&task);
      {
        sds description = NULL;
        int statement_pdf = !strcmp(strrchr(task.desc, '.') ? strrchr(task.desc, '.') : "", ".pdf");
        sds path = sdscatprintf(sdsempty(), "./tasks/%s/%s", task.name, task.desc);
        if (!task.desc[0] || strchr(task.desc, '/') || strchr(task.desc, '\\') ||
            !strcmp(task.desc, ".") || !strcmp(task.desc, ".."))
        {
          sdsfree(path);
          return response_not_found(connection);
        }
        if (!statement_pdf)
        {
          int description_size;
          char *description_data = read_file_binary(path, &description_size);
          if (description_data)
          {
            description = sdsnewlen(description_data, description_size);
            free(description_data);
          }
        }
        html = view_task(sdsempty(), username, &task, description, statement_pdf);
        sdsfree(description);
        sdsfree(path);
      }
    }
    else if (0 == strcmp(page, "desc") || 0 == strcmp(page, "desc_file"))
    {
      unsigned pk;
      const char *pk_;
      int file_size;
      int ret;
      int raw_file = 0 == strcmp(page, "desc_file");
      char *file_content; 
      sds path;
      struct task task;

      struct MHD_Response *response;

      pk_ = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "pk");
      if (!pk_)
        return MHD_NO;
      pk = strtoul(pk_, NULL, 10);

      db_lock();
      int task_result = fetch_task_by_pk(pk, &task);
      db_unlock();
      if (0 != task_result)
        return MHD_NO;

      path = sdscatprintf(sdsempty(), "./tasks/%s/%s", task.name, task.desc);

      if (!task.desc[0] || strchr(task.desc, '/') || strchr(task.desc, '\\') ||
          !strcmp(task.desc, ".") || !strcmp(task.desc, ".."))
      {
        sdsfree(path);
        return response_not_found(connection);
      }

      if (NULL == (file_content = read_file_binary(path, &file_size)))
        return response_internal_server_error(connection);

      if (!raw_file)
      {
        const char *extension = strrchr(task.desc, '.');
        if (extension && !strcmp(extension, ".pdf"))
          html = view_pdf_statement(sdsempty(), username, &task);
        else
        {
          sds markdown = sdsnewlen(file_content, file_size);
          html = view_statement(sdsempty(), username, &task, markdown);
          sdsfree(markdown);
        }
        free(file_content);
        sdsfree(path);
        return basic_response_sds(connection, html, MHD_HTTP_OK);
      }

      response = MHD_create_response_from_buffer(file_size, file_content, MHD_RESPMEM_MUST_FREE);

      add_safety_headers(response);
      MHD_add_response_header(response, "Content-Type", get_content_type(path));
      MHD_add_response_header(response, "Content-Disposition", "inline");
      sdsfree(path);
      ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
      MHD_destroy_response(response);
      return ret;
    }
    else if (0 == strcmp(page, "submit"))
    {
      unsigned pk;
      const char *pk_;

      pk_ = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "pk");
      if (!pk_)
        return MHD_NO;
      pk = strtoul(pk_, NULL, 10);

      db_lock();
      int task_validity = check_task_pk_validity(pk);
      db_unlock();
      switch (task_validity)
      {
        case 0:
          return response_not_found(connection);
        case -1:
          return response_internal_server_error(connection);
      }

      html = view_submit(sdsempty(), username, pk,
          session_authenticated ? csrf_token : NULL);
    }
    else if (0 == strcmp(page, "register"))
    {
      html = view_register(sdsempty(), username);
    }
    else if (0 == strcmp(page, "login"))
    {
      html = view_login(sdsempty(), username);
    }
    else if (0 == strcmp(page, "logout"))
    {
      if (session_token[0])
      {
        db_lock();
        session_destroy(session_token);
        db_unlock();
      }
      struct MHD_Response *response = MHD_create_response_from_buffer(0, NULL,
          MHD_RESPMEM_PERSISTENT);
      MHD_add_response_header(response, "Location", base_url());
      MHD_add_response_header(response, "Set-Cookie", tls_cert_material ?
          "tle_session=; Max-Age=0; Path=/; HttpOnly; SameSite=Lax; Secure" :
          "tle_session=; Max-Age=0; Path=/; HttpOnly; SameSite=Lax");
      int ret = MHD_queue_response(connection, MHD_HTTP_FOUND, response);
      MHD_destroy_response(response);
      return ret;
    }
    else if (0 == strcmp(page, "admin"))
    {
      const char *admin_password = getenv("TLE_ADMIN_PASSWORD");
      if (!admin_password || !*admin_password || strcmp(password, admin_password))
      {
        char DENIED[] = "access denied";
        struct MHD_Response *response;
        int ret;

        response = MHD_create_response_from_buffer(strlen(DENIED), DENIED, MHD_RESPMEM_PERSISTENT);
        ret = MHD_queue_basic_auth_fail_response(connection, "realm", response);
        MHD_destroy_response(response);

        return ret;

        return basic_response(connection, "401 Unauthorized", MHD_HTTP_OK, MHD_RESPMEM_PERSISTENT);
      }

      struct admin_task_item *items = NULL;
      size_t item_count = 0;
      db_lock();
      int fetch_result = fetch_admin_tasks(&items, &item_count);
      db_unlock();
      if (fetch_result != 0)
        return response_internal_server_error(connection);
      html = view_admin(sdsempty(), username, items, item_count);
      free_admin_tasks(items);
    }
    else
      return response_not_found(connection);

    return basic_response_sds(connection, html, MHD_HTTP_OK);
  }
  return MHD_NO;
}

static void
cleanup(int sig)
{
  (void)sig;
  info("cleaning up...");
  MHD_stop_daemon(d);
  info("stopped mhd daemon");
  db_cleanup();
  free(tls_cert_material);
  free(tls_key_material);
  info("closed database");
  exit(EXIT_SUCCESS);
}

static void
add_safety_headers(struct MHD_Response *response)
{
  MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
  MHD_add_response_header(response, "X-Frame-Options", "SAMEORIGIN");
  /*
   * MHD_add_response_header(response, "Content-Security-Policy", "default-src '*'; script-src 'self'; style-src '*';");
  MHD_add_response_header(response, "Strict-Transport-Security", "max-age=31536000; includeSubDomains; preload");
  */
  MHD_add_response_header(response, "Referrer-Policy", "no-referrer");
}

static enum MHD_Result
basic_response(struct MHD_Connection *connection, char *msg, int response_type, enum MHD_ResponseMemoryMode mode)
{
  struct MHD_Response * response;
  int ret;
  
  response = MHD_create_response_from_buffer(strlen(msg), msg, mode);
  add_safety_headers(response);
  MHD_add_response_header(response, "Content-Type", "text/html; charset=utf-8");

  ret = MHD_queue_response(connection, response_type, response);
  MHD_destroy_response(response);
  return ret;
}

static enum MHD_Result
basic_response_sds(struct MHD_Connection *connection, sds msg, int response_type)
{
  struct MHD_Response * response;
  int ret;
  response = MHD_create_response_from_buffer(sdslen(msg), msg, MHD_RESPMEM_MUST_COPY);

  add_safety_headers(response);
  MHD_add_response_header(response, "Content-Type", "text/html; charset=utf-8");

  sdsfree(msg);
  ret = MHD_queue_response(connection, response_type, response);
  MHD_destroy_response(response);
  return ret;
}
static enum MHD_Result response_internal_server_error(struct MHD_Connection *c)
{
  return basic_response(c, "internal server error", MHD_HTTP_INTERNAL_SERVER_ERROR, MHD_RESPMEM_PERSISTENT);
}

static enum MHD_Result response_not_found(struct MHD_Connection *c)
{
  return basic_response(c, "404!!!!", MHD_HTTP_NOT_FOUND, MHD_RESPMEM_PERSISTENT);
}

static void check_features()
{
  expect(MHD_is_feature_supported(MHD_FEATURE_BASIC_AUTH));
  expect(MHD_is_feature_supported(MHD_FEATURE_POSTPROCESSOR));
}
