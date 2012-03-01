#include "stddef1.h"
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <malloc.h>
#include <math.h>
#include <ncurses.h>
#include <signal.h>
#include <unistd.h>
#include <curl/curl.h>
#include <ev.h>
#include "xltop.h"
#include "x_node.h"
#include "hash.h"
#include "list.h"
#include "n_buf.h"
#include "screen.h"
#include "string1.h"
#include "trace.h"

struct curl_x {
  CURL *cx_curl;
  char *cx_host;
  long cx_port;
};

struct xl_host {
  struct hlist_node h_hash_node;
  struct xl_job *h_job;
  char h_name[];
};

struct xl_job {
  struct hlist_node j_hash_node;
  struct list_head j_clus_link;
  char *j_owner, *j_title;
  double j_start;
  size_t j_nr_hosts;
  char j_name[];
};

struct xl_clus {
  struct hlist_node c_hash_node;
  struct list_head c_job_list;
  struct ev_periodic c_w;
  char c_name[];
};

static size_t nr_fs;
static LIST_HEAD(fs_list);
double fs_status_interval = 30; /* XXX */

struct xl_fs {
  struct hlist_node f_hash_node;
  struct list_head f_link;
  struct ev_periodic f_w;
  double f_mds_load[3], f_oss_load[3];
  size_t f_nr_mds, f_nr_mdt, f_max_mds_task;
  size_t f_nr_oss, f_nr_ost, f_max_oss_task;
  size_t f_nr_nid;
  char f_name[];
};

struct xl_k {
  char *k_x[2];
  int k_type[2];
  double k_t; /* Timestamp. */
  double k_pending[NR_STATS];
  double k_rate[NR_STATS]; /* EWMA bytes (or reqs) per second. */
  double k_sum[NR_STATS];
};

struct xl_col {
  char *c_name;
  int (*c_get_s)(struct xl_col *c, struct xl_k *k, char **s, int *n);
  int (*c_get_d)(struct xl_col *c, struct xl_k *k, double *d);
  int (*c_get_z)(struct xl_col *c, struct xl_k *k, size_t *z);
  void (*c_print)(int y, int x, struct xl_col *c, struct xl_k *k);
  size_t c_offset;
  double c_scale;
  int c_width, c_right;
};

static struct curl_x curl_x = {
  .cx_host = "localhost", /* XXX */
  .cx_port = 9901, /* XXX */
};

static int show_full_name = 0;
static int show_fs_status = 1;
static int show_stat_sums = 0;

static int scroll_start, scroll_delta;

static char status_bar[256];
static double status_bar_time;

static struct xl_col top_col[24];
static const char *top_sort_key; /* TODO */
static struct xl_k *top_k;
static size_t top_k_limit = 4096;
static size_t top_k_length;
static char *top_query = NULL;
static double top_interval = 10;
static struct ev_timer top_timer_w;
static N_BUF(top_nb);

static struct hash_table xl_hash_table[NR_X_TYPES];

int curl_x_get_url(struct curl_x *cx, char *url, struct n_buf *nb)
{
  FILE *file = NULL;
  int rc = -1;

  n_buf_destroy(nb);

  file = open_memstream(&nb->nb_buf, &nb->nb_size);
  if (file == NULL) {
    ERROR("cannot open memory stream: %m\n");
    goto out;
  }

  curl_easy_reset(cx->cx_curl);
  curl_easy_setopt(cx->cx_curl, CURLOPT_URL, url);
  curl_easy_setopt(cx->cx_curl, CURLOPT_PORT, cx->cx_port);
  curl_easy_setopt(cx->cx_curl, CURLOPT_UPLOAD, 0L);
  curl_easy_setopt(cx->cx_curl, CURLOPT_WRITEDATA, file);

#if DEBUG
  curl_easy_setopt(cx->cx_curl, CURLOPT_VERBOSE, 1L);
#endif

  int curl_rc = curl_easy_perform(cx->cx_curl);
  if (curl_rc != 0) {
    ERROR("cannot GET `%s': %s\n", url, curl_easy_strerror(rc));
    /* Reset curl... */
    goto out;
  }

  if (ferror(file)) {
    ERROR("error writing to memory stream: %m\n");
    goto out;
  }

  rc = 0;

 out:
  if (file != NULL)
    fclose(file);

  nb->nb_end = nb->nb_size;

  return rc;
}

int curl_x_get(struct curl_x *cx, const char *path, const char *qstr,
               struct n_buf *nb)
{
  char *url = NULL;
  int rc = -1;

  url = strf("http://%s/%s%s%s", cx->cx_host, path,
             qstr != NULL ? "?" : "", qstr != NULL ? qstr : "");
  if (url == NULL)
    OOM();

  TRACE("url `%s'\n", url);

  if (curl_x_get_url(cx, url, nb) < 0)
    goto out;

  rc = 0;

 out:
  free(url);

  return rc;
}

typedef int (msg_cb_t)(void *, char *, size_t);

static int curl_x_get_iter(struct curl_x *cx,
                           const char *path, const char *query,
                           msg_cb_t *cb, void *data)
{
  N_BUF(nb);
  char *msg;
  size_t msg_len;
  int rc = -1;

  if (curl_x_get(cx, path, query, &nb) < 0)
    goto out;

  while (n_buf_get_msg(&nb, &msg, &msg_len) == 0) {
    rc = (*cb)(data, msg, msg_len);
    if (rc < 0)
      goto out;
  }

 out:
  n_buf_destroy(&nb);

  return rc;
}

char *query_escape(const char *s)
{
  char *e = malloc(3 * strlen(s) + 1), *p, x[4];
  if (e == NULL)
    return NULL;

  for (p = e; *s != 0; s++) {
    if (isalnum(*s) || *s == '.' || *s == '-' || *s == '~' || *s == '_') {
      *(p++) = *s;
    } else {
      snprintf(x, sizeof(x), "%%%02hhX", (unsigned char) *s);
      *(p++) = x[0];
      *(p++) = x[1];
      *(p++) = x[2];
    }
  }

  *p = 0;

  return e;
}

int query_add(char **s, const char *f, const char *v)
{
  char *f1 = query_escape(f), *v1 = query_escape(v);
  char *s0 = *s, *s1 = NULL;
  int rc = -1;

  if (f1 == NULL || v1 == NULL)
    goto err;

  if (s0 == NULL)
    s1 = strf("%s=%s", f1, v1);
  else
    s1 = strf("%s&%s=%s", s0, f1, v1);

  if (s1 == NULL)
    goto err;

  rc = 0;

 err:

  free(s0);
  *s = s1;

  return rc;
}

int query_addz(char **s, const char *f, size_t n)
{
  char v[3 * sizeof(n) + 1];

  snprintf(v, sizeof(v), "%zu", n);

  return query_add(s, f, v);
}

static int xl_sep(char *s, int *type, char **name)
{
  char *s_type = strsep(&s, ":=");
  int i_type = x_str_type(s_type);

  if (i_type < 0) {
    ERROR("unrecognized type `%s'\n", s_type);
    return -1;
  }

  *type = i_type;
  *name = s;

  return 0;
}

int get_x_nr_hint(int type, size_t *hint)
{
  N_BUF(nb);
  char *path = NULL;
  int rc = -1;
  char *m, *k, *v;
  size_t m_len, n = 0;

  path = strf("%s/_info", x_type_name(type));
  if (path == NULL)
    goto out;

  if (curl_x_get(&curl_x, path, NULL, &nb) < 0)
    goto out;

  while (n_buf_get_msg(&nb, &m, &m_len) == 0) {
    if (split(&m, &k, &v, (char **) NULL) != 2)
      continue;

    if (strcmp(k, "x_nr:") == 0)
      n = MAX(n, (size_t) strtoul(v, NULL, 0));

    if (strcmp(k, "x_nr_hint:") == 0)
      n = MAX(n, (size_t) strtoul(v, NULL, 0));
  }

  *hint = n;
  TRACE("type `%s', *hint %zu\n", x_type_name(type), *hint);

  if (n != 0)
    rc = 0;

 out:
  n_buf_destroy(&nb);
  free(path);

  return rc;
}

int xl_hash_init(int type)
{
  size_t hint = 0;

  if (get_x_nr_hint(type, &hint) < 0)
    return -1;

  if (hash_table_init(&xl_hash_table[type], hint) < 0)
    return -1;

  return 0;
}

#define _xl_lookup(p, i, name, xl_type, m_hash_node, m_name, create)    \
  do {                                                                  \
    struct hash_table *_t = &xl_hash_table[(i)];                        \
    struct hlist_head *head;                                            \
    const char *_name = (name);                                         \
    typeof(xl_type) *_p;                                                \
                                                                        \
    _p = str_table_lookup_entry(_t, _name, &head, xl_type,              \
                                m_hash_node, m_name);                   \
    if (_p == NULL && (create)) {                                       \
      _p = malloc(sizeof(*_p) + strlen(_name) + 1);                     \
      if (_p == NULL)                                                   \
        OOM();                                                          \
      memset(_p, 0, sizeof(*_p));                                       \
      strcpy(_p->m_name, _name);                                        \
      hlist_add_head(&_p->m_hash_node, head);                           \
    }                                                                   \
                                                                        \
    (p) = _p;                                                           \
  } while (0)

#define xl_lookup_host(h, name, create) \
  _xl_lookup((h), X_HOST, (name), struct xl_host, h_hash_node, h_name, (create))

#define xl_lookup_job(j, name, create) \
  _xl_lookup((j), X_JOB, (name), struct xl_job, j_hash_node, j_name, (create))

#define xl_lookup_clus(c, name, create) \
  _xl_lookup((c), X_CLUS, (name), struct xl_clus, c_hash_node, c_name, (create))

#define xl_lookup_fs(f, name, create) \
  _xl_lookup((f), X_FS, (name), struct xl_fs, f_hash_node, f_name, (create))

static int xl_clus_msg_cb(struct xl_clus *c, char *m, size_t m_len)
{
  char *s_host, *s_job, *owner, *title, *s_start, *s_nr_hosts;
  struct xl_host *h;
  struct xl_job *j;

  if (split(&m, &s_host, &s_job, &owner, &title, &s_start, &s_nr_hosts,
            (char **) NULL) != 6)
    return 0;

  xl_lookup_host(h, s_host, 1);
  xl_lookup_job(j, s_job, 1);
  h->h_job = j;

  if (j->j_clus_link.next == NULL) {
    INIT_LIST_HEAD(&j->j_clus_link);
    j->j_owner = strdup(owner);
    j->j_title = strdup(title);
    j->j_start = strtod(s_start, NULL);
    j->j_nr_hosts = strtoul(s_nr_hosts, NULL, 0);
  }

  list_move(&j->j_clus_link, &c->c_job_list);

  return 0;
}

static void xl_clus_cb(EV_P_ struct ev_periodic *w, int revents)
{
  struct xl_clus *c = container_of(w, struct xl_clus, c_w);
  struct xl_job *j, *j_tmp;
  char *path = NULL;
  LIST_HEAD(tmp_list);

  TRACE("clus `%s', now %.0f\n", c->c_name, ev_now(EV_A));

  list_splice_init(&c->c_job_list, &tmp_list);

  path = strf("clus/%s", c->c_name);
  if (path == NULL)
    OOM();

  curl_x_get_iter(&curl_x, path, NULL, (msg_cb_t *) &xl_clus_msg_cb, c);

  free(path);

  list_for_each_entry_safe(j, j_tmp, &tmp_list, j_clus_link) {
    hlist_del(&j->j_hash_node);
    list_del(&j->j_clus_link);
    free(j->j_owner);
    free(j->j_title);
    free(j);
  }
}

int xl_clus_add(EV_P_ const char *name)
{
  N_BUF(nb);
  int rc = -1;
  char *info_path = NULL, *m, *k, *v;
  size_t m_len;
  struct xl_clus *c = NULL;
  double c_int = -1, c_off = -1;

  xl_lookup_clus(c, name, 1);

  if (c->c_hash_node.next != NULL)
    return 0;

  info_path = strf("clus/%s/_info", name);
  if (info_path == NULL)
    OOM();

  if (curl_x_get(&curl_x, info_path, NULL, &nb) < 0)
    goto err;

  while (n_buf_get_msg(&nb, &m, &m_len) == 0) {
    if (split(&m, &k, &v, (char **) NULL) != 2)
      continue;

    if (strcmp(k, "interval:") == 0)
      c_int = strtod(v, NULL);
    else if (strcmp(k, "offset:") == 0)
      c_off = strtod(v, NULL);
  }

  if (c_int < 0 || c_off < 0)
    goto err;

  INIT_LIST_HEAD(&c->c_job_list);
  c_off = fmod(c_off + 1, c_int); /* XXX */
  ev_periodic_init(&c->c_w, &xl_clus_cb, c_off, c_int, NULL);
  ev_periodic_start(EV_A_ &c->c_w);
  ev_feed_event(EV_A_ &c->c_w, 0);

  rc = 0;

  if (0) {
  err:
    if (c != NULL)
      hlist_del(&c->c_hash_node);
    free(c);
  }

  n_buf_destroy(&nb);
  free(info_path);

  return rc;
}

static int xl_clus_init(EV_P)
{
  N_BUF(nb);
  int rc = -1;
  char *m, *name;
  size_t m_len;

  if (xl_hash_init(X_CLUS) < 0)
    goto out;

  if (curl_x_get(&curl_x, "clus", NULL, &nb) < 0)
    goto out;

  while (n_buf_get_msg(&nb, &m, &m_len) == 0) {
    if (split(&m, &name, (char **) NULL) != 1)
      continue;

    if (xl_clus_add(EV_A_ name) < 0)
      goto out;
  }

  rc = 0;

 out:
  n_buf_destroy(&nb);

  return rc;
}

static int xl_fs_msg_cb(struct xl_fs *f, char *msg, size_t msg_len)
{
  char *s_serv;
  struct serv_status ss = {};
  int i;

  if (split(&msg, &s_serv, (char **) NULL) != 1 || msg == NULL)
    return 0;

  if (sscanf(msg, SCN_SERV_STATUS_FMT, SCN_SERV_STATUS_ARG(ss)) !=
      NR_SCN_SERV_STATUS_ARGS)
    return 0;

  TRACE("serv `%s', status "PRI_SERV_STATUS_FMT"\n",
        s_serv, PRI_SERV_STATUS_ARG(ss));

  if (ss.ss_nr_mdt > 0) {
    for (i = 0; i < 3; i++)
      f->f_mds_load[i] = MAX(f->f_mds_load[i], ss.ss_load[i]);
    f->f_nr_mds += 1;
    f->f_max_mds_task = MAX(f->f_max_mds_task, ss.ss_nr_task);
  } else if (ss.ss_nr_ost > 0) {
    for (i = 0; i < 3; i++)
      f->f_oss_load[i] = MAX(f->f_oss_load[i], ss.ss_load[i]);
    f->f_nr_oss += 1;
    f->f_max_oss_task = MAX(f->f_max_oss_task, ss.ss_nr_task);
  }
  f->f_nr_mdt += ss.ss_nr_mdt;
  f->f_nr_ost += ss.ss_nr_ost;
  f->f_nr_nid = MAX(f->f_nr_nid, ss.ss_nr_nid);

  return 0;
}

static void xl_fs_cb(EV_P_ struct ev_periodic *w, int revents)
{
  struct xl_fs *f = container_of(w, struct xl_fs, f_w);
  char *status_path = NULL;

  TRACE("fs `%s', now %.0f\n", f->f_name, ev_now(EV_A));

  status_path = strf("fs/%s/_status", f->f_name);
  if (status_path == NULL)
    OOM();

  memset(f->f_mds_load, 0, sizeof(f->f_mds_load));
  memset(f->f_oss_load, 0, sizeof(f->f_oss_load));
  f->f_nr_mds = 0;
  f->f_nr_mdt = 0;
  f->f_max_mds_task = 0;
  f->f_nr_oss = 0;
  f->f_nr_ost = 0;
  f->f_max_oss_task = 0;
  f->f_nr_nid = 0;

  curl_x_get_iter(&curl_x, status_path, NULL, (msg_cb_t *) &xl_fs_msg_cb, f);

  free(status_path);
}

int xl_fs_add(EV_P_ const char *name)
{
  struct xl_fs *f;

  xl_lookup_fs(f, name, 1);

  if (f->f_hash_node.next != NULL)
    return 0;

  list_add(&f->f_link, &fs_list);
  ev_periodic_init(&f->f_w, &xl_fs_cb, 0, fs_status_interval, NULL);
  ev_periodic_start(EV_A_ &f->f_w);
  ev_feed_event(EV_A_ &f->f_w, 0);
  nr_fs++;

  return 0;
}

static int xl_fs_init(EV_P)
{
  N_BUF(nb);
  int rc = -1;
  char *m, *name;
  size_t m_len;

  if (xl_hash_init(X_FS) < 0)
    goto out;

  if (curl_x_get(&curl_x, "fs", NULL, &nb) < 0)
    goto out;

  while (n_buf_get_msg(&nb, &m, &m_len) == 0) {
    if (split(&m, &name, (char **) NULL) != 1)
      continue;

    if (xl_fs_add(EV_A_ name) < 0)
      goto out;
  }

  rc = 0;

 out:
  n_buf_destroy(&nb);

  return rc;
}

static void top_msg_cb(char *msg, size_t msg_len)
{
  int i;
  char *s[2];
  struct xl_k *k = &top_k[top_k_length];

  if (split(&msg, &s[0], &s[1], (char **) NULL) != 2 || msg == NULL)
    return;

  for (i = 0; i < 2; i++)
    if (xl_sep(s[i], &k->k_type[i], &k->k_x[i]) < 0 || k->k_x[i] == NULL)
      return;

  if (sscanf(msg, "%lf "SCN_K_STATS_FMT, &k->k_t, SCN_K_STATS_ARG(k)) !=
      1 + NR_K_STATS)
    return;

  TRACE("%s %s "PRI_STATS_FMT("%f")"\n",
        k->k_x[0], k->k_x[1], PRI_STATS_ARG(k->k_rate));

  top_k_length++;
}

static void top_timer_cb(EV_P_ ev_timer *w, int revents)
{
  double now = ev_now(EV_A);
  char *msg;
  size_t msg_len;

  TRACE("begin, now %f\n", now);

  top_k_length = 0;
  n_buf_destroy(&top_nb);

  if (curl_x_get(&curl_x, "top", top_query, &top_nb) < 0)
    return;

  while (n_buf_get_msg(&top_nb, &msg, &msg_len) == 0)
    top_msg_cb(msg, msg_len);

  status_bar_time = 0;
  screen_refresh(EV_A);
}

int c_get_x(struct xl_col *c, struct xl_k *k, char **s, int *n)
{
  char *x = k->k_x[c->c_offset];
  int t = k->k_type[c->c_offset];

  *s = x;

  if (show_full_name || (t == X_HOST && isdigit(*x)))
    *n = strlen(x);
  else
    *n = strcspn(x, "@.");

  return 0;
}

struct xl_job *k_get_job(struct xl_k *k)
{
  struct xl_host *h;
  struct xl_job *j = NULL;

  if (k->k_type[0] == X_HOST) {
    xl_lookup_host(h, k->k_x[0], 0);
    if (h != NULL)
      j = h->h_job;
  } else if (k->k_type[0] == X_JOB) {
    xl_lookup_job(j, k->k_x[0], 0);
  }

  return j;
}

int c_get_owner(struct xl_col *c, struct xl_k *k, char **s, int *n)
{
  struct xl_job *j = k_get_job(k);

  if (j == NULL)
    return -1;

  *s = j->j_owner;
  *n = strlen(j->j_owner);

  return 0;
}

int c_get_title(struct xl_col *c, struct xl_k *k, char **s, int *n)
{
  struct xl_job *j = k_get_job(k);

  if (j == NULL)
    return -1;

  *s = j->j_title;
  *n = strlen(j->j_title);

  return 0;
}

int c_get_jobid(struct xl_col *c, struct xl_k *k, char **s, int *n)
{
  struct xl_job *j = k_get_job(k);

  if (j == NULL)
    return -1;

  *s = j->j_name;
  *n = strlen(j->j_name);

  return 0;
}

void c_print_d(int y, int x, struct xl_col *c, struct xl_k *k)
{
  double d = *(double *) (((char *) k) + c->c_offset);

  if (c->c_scale > 0)
    d /= c->c_scale;

  mvprintw(y, x, "%*.3f", c->c_width, d);
}

void c_print(int y, int x, struct xl_col *c, struct xl_k *k)
{
  if (c->c_print != NULL) {
    (*c->c_print)(y, x, c, k);
  } else if (c->c_get_s != NULL) {
    char *s = NULL;
    int n = 0;

    if ((*c->c_get_s)(c, k, &s, &n) < 0 || s == NULL)
      return;

    n = MIN(n, c->c_width);

    mvprintw(y, x, "%-*.*s", c->c_width, n, s);
  } else if (c->c_get_d != NULL) {
    double d;

    if ((*c->c_get_d)(c, k, &d) < 0)
      return;

    if (c->c_scale > 0)
      d /= c->c_scale;

    mvprintw(y, x, "%*.3f", c->c_width, d);
  }
}

void c_print_nr_hosts(int y, int x, struct xl_col *c, struct xl_k *k)
{
  struct xl_host *h;
  struct xl_job *j = NULL;

  if (k->k_type[0] == X_HOST) {
    xl_lookup_host(h, k->k_x[0], 0);
    if (h != NULL)
      j = h->h_job;
  } else if (k->k_type[0] == X_JOB) {
    xl_lookup_job(j, k->k_x[0], 0);
  }

  if (j != NULL)
    mvprintw(y, x, "%*zu", c->c_width, j->j_nr_hosts);
}

#define COL_X(name,which,width) ((struct xl_col) { \
    .c_name = (name), \
    .c_width = (width), \
    .c_get_s = &c_get_x, \
    .c_offset = (which), \
  })

#define COL_HOST  COL_X("HOST",  0, 15)
#define COL_JOB   COL_X("JOB",   0, 15)
#define COL_CLUS  COL_X("CLUS",  0, 15)
#define COL_ALL_0 COL_X("ALL_0", 0,  5)
#define COL_SERV  COL_X("SERV",  1, 15)
#define COL_FS    COL_X("FS",    1, 15)
#define COL_ALL_1 COL_X("ALL_1", 1,  5)

#define COL_D(name,mem,width,scale) ((struct xl_col) { \
    .c_name = (name),                       \
    .c_width = (width),                     \
    .c_print = &c_print_d,                  \
    .c_offset = offsetof(struct xl_k, mem), \
    .c_scale = (scale),                     \
    .c_right = 1,                           \
  })

#define COL_MB(name,mem) COL_D(name, mem, 10, 1048576)

#define COL_WR_MB_RATE COL_MB("WR_MB/S", k_rate[STAT_WR_BYTES])
#define COL_RD_MB_RATE COL_MB("RD_MB/S", k_rate[STAT_RD_BYTES])
#define COL_REQS_RATE  COL_D("REQS/S",   k_rate[STAT_NR_REQS], 10, 1)

#define COL_WR_MB_SUM  COL_MB("WR_MB", k_sum[STAT_WR_BYTES])
#define COL_RD_MB_SUM  COL_MB("RD_MB", k_sum[STAT_RD_BYTES])
#define COL_REQS_SUM   COL_D("REQS",   k_sum[STAT_NR_REQS], 10, 1)

#define COL_JOBID ((struct xl_col) { \
  .c_name = "JOBID", .c_width = 10, .c_get_s = &c_get_jobid })

#define COL_OWNER ((struct xl_col) { \
  .c_name = "OWNER", .c_width = 10, .c_get_s = &c_get_owner })

#define COL_TITLE ((struct xl_col) { \
  .c_name = "NAME", .c_width = 10, .c_get_s = &c_get_title })

#define COL_NR_HOSTS ((struct xl_col) { \
  .c_name = "HOSTS", .c_width = 5, .c_print = &c_print_nr_hosts, .c_right = 1 })

void status_bar_vprintf(EV_P_ const char *fmt, va_list args)
{
  vsnprintf(status_bar, sizeof(status_bar), fmt, args);
  status_bar_time = ev_now(EV_A);
  screen_refresh(EV_A);
}

void status_bar_printf(EV_P_ const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);
  status_bar_vprintf(EV_A_ fmt, args);
  va_end(args);
}

void error_printf(const char *prog, const char *func, int line,
                  const char *fmt, ...)
{
  va_list args;

  va_start(args, fmt);

  if (screen_is_active) {
    status_bar_vprintf(EV_DEFAULT, fmt, args);
  } else {
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, args);
  }
  va_end(args);
}

static void screen_key_cb(EV_P_ int key)
{
  switch (tolower(key)) {
  case ' ':
  case '\n':
    ev_feed_event(EV_A_ &top_timer_w, EV_TIMER);
    break;
  case 'q':
    ev_break(EV_A_ EVBREAK_ALL); /* XXX */
    return;
  case KEY_DOWN:
    scroll_delta += 1;
    break;
  case KEY_HOME:
    scroll_delta = INT_MIN / 2;
    break;
  case KEY_END:
    scroll_delta = INT_MAX / 2;
    break;
  case KEY_UP:
    scroll_delta -= 1;
    break;
  case KEY_NPAGE:
    scroll_delta += LINES;
    break;
  case KEY_PPAGE:
    scroll_delta -= LINES;
    break;
  default:
    if (isascii(key)) {
      status_bar_time = ev_now(EV_A);
      snprintf(status_bar, sizeof(status_bar), "unknown command `%c'", key);
    }
    break;
  }

  screen_refresh(EV_A);
}

static void print_k(int line, struct xl_col *c, struct xl_k *k)
{
  int i;
  for (i = 0; c->c_name != NULL; c++) {
    c_print(line, i, c, k);
    i += c->c_width + 2;
  }
}

static void screen_refresh_cb(EV_P_ int LINES, int COLS)
{
  time_t now = ev_now(EV_A);
  int line = 0, i;
  struct xl_fs *f;
  struct xl_col *c;

  erase();

  if (!show_fs_status)
    goto skip_fs_status;

  mvprintw(line, 0, "%-15s  %6s %6s %6s %6s %6s  %6s %6s %6s %6s %6s  %6s",
           "FILESYSTEM",
           "#MDS/T", "L1", "L5", "L15", "#TASKS",
           "#OSS/T", "L1", "L5", "L15", "#TASKS", "#NIDS");
  mvchgat(line, 0, -1, A_STANDOUT, 3, NULL);
  line++;

  list_for_each_entry(f, &fs_list, f_link) {
    char m_buf[80], o_buf[80];

    snprintf(m_buf, sizeof(m_buf), "%zu/%zu", f->f_nr_mds, f->f_nr_mdt);
    snprintf(o_buf, sizeof(o_buf), "%zu/%zu", f->f_nr_oss, f->f_nr_ost);

    mvprintw(line++, 0,
             "%-15s  %6s %6.2f %6.2f %6.2f %6zu  %6s %6.2f %6.2f %6.2f %6zu  %6zu",
             f->f_name,
             m_buf, f->f_mds_load[0], f->f_mds_load[1], f->f_mds_load[2],
             f->f_max_mds_task,
             o_buf, f->f_oss_load[0], f->f_oss_load[1], f->f_oss_load[2],
             f->f_max_oss_task,
             f->f_nr_nid);
  }

 skip_fs_status:
  for (i = 0, c = top_col; c->c_name != NULL; i += c->c_width + 2, c++)
    mvprintw(line, i,
             c->c_right ? "%*.*s  " : "%-*.*s  ",
             c->c_width, c->c_width, c->c_name);
  mvchgat(line, 0, -1, A_STANDOUT, 2, NULL);
  line++;

  int new_start = scroll_start + scroll_delta;
  int max_start = top_k_length - (LINES - line - 1);

  new_start = MIN(new_start, max_start);
  new_start = MAX(new_start, 0);

  int j = new_start;
  for (; j < (int) top_k_length && line < LINES - 1; j++, line++)
    print_k(line, top_col, &top_k[j]);

  if (new_start != scroll_start || status_bar_time + 4 < now)
    status_bar_printf(EV_A_ "%d-%d out of %zu",
                      new_start + (top_k_length != 0),
                      j, top_k_length);

  mvprintw(LINES - 1, 0, "%.*s", COLS, status_bar);

  int prog_ctime_len = strlen(program_invocation_short_name) + 28;
  if (strlen(status_bar) + prog_ctime_len < COLS)
    mvprintw(LINES - 1, COLS - prog_ctime_len, "%s - %s",
             program_invocation_short_name, ctime(&now));

  mvchgat(LINES - 1, 0, -1, A_STANDOUT, 0, NULL);

  scroll_start = new_start;
  scroll_delta = 0;
}

static void usage(int status)
{
  fprintf(status == 0 ? stdout : stderr,
          "Usage: %s [OPTIONS]...\n"
          /* ... */
          "\nOPTIONS:\n"
          " -c, --conf=FILE\n"
          /* ... */
          ,
          program_invocation_short_name);

  exit(status);
}

static char *make_top_query(int t[2], char *x[2], int d[2], size_t limit)
{
  char *q = NULL, *s[2] = { NULL };

  int i;
  for (i = 0; i < 2; i++) {
    s[i] = strf("%s:%s", x_type_name(t[i]), x[i]);
    if (s[i] == NULL)
      OOM();
  }

  if (query_add(&q, "x0", s[0]) < 0)
    goto err;
  if (query_addz(&q, "d0", d[0]) < 0)
    goto err;

  if (query_add(&q, "x1", s[1]) < 0)
    goto err;
  if (query_addz(&q, "d1", d[1]) < 0)
    goto err;

  if (query_addz(&q, "limit", limit) < 0)
    goto err;

  TRACE("q `%s'\n", q);

  if (0) {
  err:
    free(q);
    q = NULL;
  }

  free(s[0]);
  free(s[1]);

  return q;
}

int main(int argc, char *argv[])
{
  char *o_host = NULL, *o_port = NULL, *conf_path = NULL;

  struct option opts[] = {
    { "conf",        1, NULL, 'c' },
    { "help",        0, NULL, 'h' },
    { "interval",    1, NULL, 'i' },
    { "sort-key",    1, NULL, 'k' },
    { "limit",       1, NULL, 'l' },
    { "remote-port", 1, NULL, 'p' },
    { "remote-host", 1, NULL, 'r' },
    { "sum",         1, NULL, 's' },
    { NULL,          0, NULL,  0  },
  };

  /* Show rate or show sum. */
  /* Sort spec depends on which. */
  /* Limit.  Scrolling. */

  int opt;
  while ((opt = getopt_long(argc, argv, "c:hi:k:l:p:r:s:", opts, 0)) > 0) {
    switch (opt) {
    case 'c':
      conf_path = optarg;
      break;
    case 'h':
      usage(0);
      break;
    case 'i':
      top_interval = strtod(optarg, NULL);
      if (top_interval <= 0)
        FATAL("invalid interval `%s'\n", optarg);
      break;
    case 'k':
      top_sort_key = optarg;
      break;
    case 'l':
      top_k_limit = strtoul(optarg, NULL, 0);
      break;
    case 'p':
      o_port = optarg;
      break;
    case 'r':
      o_host = optarg;
      break;
    case 's':
      show_stat_sums = 1;
      break;
    case '?':
      FATAL("Try `%s --help' for more information.\n", program_invocation_short_name);
    }
  }

  /* r_port = strtol(XLTOP_BIND_PORT, NULL, 0); */

  if (conf_path != NULL)
    /* TODO */;

  if (top_interval <= 0)
    FATAL("invalid interval %f, must be positive\n", top_interval);

  if (top_k_limit <= 0)
    FATAL("invalid limit %zu, must be positive\n", top_k_limit);

  if (o_host != NULL)
    curl_x.cx_host = o_host;

  if (curl_x.cx_host == NULL)
    FATAL("no remote host specified\n");

  if (o_port != NULL)
    curl_x.cx_port = strtol(o_port, NULL, 0);

  if (curl_x.cx_port == 0)
    FATAL("no remote port specified\n");

  /* Handle args. */
  char *x[2] = { "ALL", "ALL" };
  int t[2] = { X_ALL_0, X_ALL_1 };
  int c[2] = { X_JOB, X_FS };

  char *x_set[NR_X_TYPES] = { };
  int t_set[NR_X_TYPES] = { };

  int i;
  for (i = optind; i < argc; i++) {
    int ti;
    char *xi;

    if (xl_sep(argv[i], &ti, &xi) < 0)
      FATAL("unrecognized type `%s'\n", argv[i]);

    TRACE("ti `%s', xi `%s'\n", x_type_name(ti), xi);

    t_set[ti] = 1;
    if (xi != NULL)
      x_set[ti] = xi;
  }

  for (i = X_ALL_0; i >= X_HOST; i--) {
    if (t_set[i])
      c[0] = i;
    if (x_set[i] != NULL) {
      x[0] = x_set[i];
      t[0] = i;
    }
  }

  for (i = X_ALL_1; i >= X_SERV; i--) {
    if (t_set[i])
      c[1] = i;
    if (x_set[i] != NULL) {
      x[1] = x_set[i];
      t[1] = i;
    }
  }

#if 0
  if (x_set[X_HOST] != NULL) {
    c[0] = X_HOST;
    t[0] = X_HOST;
    x[0] = x_set[X_HOST];
  } else if (x_set[X_JOB] != NULL) {
    c[0] = t[0] = X_JOB;
    x[0] = x_set[X_JOB];
    d[0] = t_set[X_HOST] ? 1 : 0;
  } else if (x_set[X_CLUS] != NULL) {
    t[0] = X_CLUS;
    x[0] = x_set[X_CLUS];
    d[0] = t_set[X_HOST] ? 2 : (t_set[X_JOB] ? 1 : 0);
  } else {
    t[0] = X_ALL_0;
    x[0] = "ALL";
    d[0] = t_set[X_HOST] ? 3 : (t_set[X_JOB] ? 2 : (t_set[X_CLUS] ? 1 : 2));
  }

  if (x_set[X_SERV] != NULL) {
    t[1] = X_SERV;
    x[1] = x_set[X_SERV];
    d[1] = 0;
  } else if (x_set[X_FS] != NULL) {
    t[1] = X_FS;
    x[1] = x_set[X_FS];
    d[1] = t_set[X_SERV] ? 1 : 0;
  } else {
    t[1] = X_ALL_1;
    x[1] = "ALL";
    d[1] = t_set[X_SERV] ? 2 : 1; /* Show filesystems. */
  }
#endif

  /* Fully qualify host, serv, job if needed. */
  if (t[0] == X_JOB) {
    char *a = strchr(x[0], '@');
    if (a == NULL) {
      if (x_set[X_CLUS] == NULL)
        FATAL("must specify job as JOBID@CLUS or pass clus=CLUS\n");
      x[0] = strf("%s@%s", x[0], x_set[X_CLUS]);
    }
  }

  int d[2] = { t[0] - c[0], t[1] - c[1] };

  top_query = make_top_query(t, x, d, top_k_limit);
  if (top_query == NULL)
    FATAL("cannot initialize top query: %m\n");

  /* Initialize columns. */

  top_col[0] = c[0] == X_HOST ? COL_HOST : c[0] == X_JOB ? COL_JOB :
    c[0] == X_CLUS ? COL_CLUS: COL_ALL_0;
  top_col[1] = c[1] == X_SERV ? COL_SERV : c[1] == X_FS ? COL_FS : COL_ALL_1;
  top_col[2] = show_stat_sums ? COL_WR_MB_SUM : COL_WR_MB_RATE;
  top_col[3] = show_stat_sums ? COL_RD_MB_SUM : COL_RD_MB_RATE;
  top_col[4] = show_stat_sums ? COL_REQS_SUM : COL_REQS_RATE;

  if (c[0] == X_HOST) {
    top_col[5] = COL_JOBID;
    top_col[6] = COL_OWNER;
    top_col[7] = COL_TITLE;
  } else if (c[0] == X_JOB) {
    top_col[5] = COL_OWNER;
    top_col[6] = COL_TITLE;
    top_col[7] = COL_NR_HOSTS;
    /* TODO Run time. */
  }

  int curl_rc = curl_global_init(CURL_GLOBAL_NOTHING);
  if (curl_rc != 0)
    FATAL("cannot initialize curl: %s\n", curl_easy_strerror(curl_rc));

  curl_x.cx_curl = curl_easy_init();
  if (curl_x.cx_curl == NULL)
    FATAL("cannot initialize curl handle: %m\n");

  top_k = calloc(top_k_limit, sizeof(top_k[0]));
  if (top_k == NULL)
    OOM();

  if (xl_hash_init(X_HOST) < 0)
    FATAL("cannot initialize host table\n");

  if (xl_hash_init(X_JOB) < 0)
    FATAL("cannot initialize job table\n");

  if (xl_fs_init(EV_DEFAULT) < 0)
    FATAL("cannot initialize fs data\n");

  if (xl_clus_init(EV_DEFAULT) < 0)
    FATAL("cannot initialize cluster data\n");

  signal(SIGPIPE, SIG_IGN);

  ev_timer_init(&top_timer_w, &top_timer_cb, 0.1, top_interval);
  ev_timer_start(EV_DEFAULT_ &top_timer_w);

  screen_init(&screen_refresh_cb, 1.0);
  screen_set_key_cb(&screen_key_cb);
  screen_start(EV_DEFAULT);

  ev_run(EV_DEFAULT_ 0);

  screen_stop(EV_DEFAULT);

  if (curl_x.cx_curl != NULL)
    curl_easy_cleanup(curl_x.cx_curl);

  curl_global_cleanup();

  return 0;
}
