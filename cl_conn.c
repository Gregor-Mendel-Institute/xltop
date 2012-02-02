#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "cl_types.h"
#include "string1.h"
#include "cl_conn.h"
#include "trace.h"

#define CL_NR_ERRS (CL_ERR_MAX_PLUS_1 - CL_ERR_OK)

static const char *cl_err_str[CL_NR_ERRS] = {
#define I(x) [(x) - CL_ERR_OK]
  I(CL_ERR_OK) = "success",
  I(CL_ERR_ENDED) = "connection closed",
  I(CL_ERR_MOVED) = "connection moved",
  I(CL_ERR_INTERNAL) = "internal error",
  I(CL_ERR_NO_AUTH) = "operation not permitted",
  I(CL_ERR_NO_CLUS) = "unknown cluster",
  I(CL_ERR_NO_CTL) = "invalid operation",
  I(CL_ERR_NO_FS) = "unknown filesystem",
  I(CL_ERR_NO_HOST) = "unknown host",
  I(CL_ERR_NO_JOB) = "unknown job",
  I(CL_ERR_NO_MEM) = "cannot allocate memory",
  I(CL_ERR_NO_SERV) = "unknown server",
  I(CL_ERR_NO_USER) = "unknown user",
  I(CL_ERR_NO_X) = "unknown entity",
  I(CL_ERR_NR_ARGS) = "incorrect number of arguments",
  I(CL_ERR_WHICH) = "invalid pair",
#undef I
};

int is_cl_err(int n)
{
  return CL_ERR_OK <= n && n < CL_ERR_MAX_PLUS_1;
}

const char *cl_strerror(int n)
{
#if DEBUG != 0
  size_t i;
  for (i = 0; i < CL_NR_ERRS; i++) {
    if (cl_err_str[i] == NULL)
      FATAL("cl_err_str[%zu] is NULL\n", i);
  }
#endif

  return is_cl_err(n) ? cl_err_str[n - CL_ERR_OK] : strerror(n);
}

/* TODO If DEBUG set then check that ctls are sorted in cl_conn_init(). */
/* TODO Remove cc_name.  Make cl_conn_set() return void. */
/* TODO Remove events argument from cl_conn_set(). */

static void cl_conn_timer_cb(EV_P_ ev_timer *w, int revents);
static void cl_conn_io_cb(EV_P_ ev_io *w, int revents);
static void cl_conn_up(EV_P_ struct cl_conn *cc, int err);

int cl_conn_init(struct cl_conn *cc, const struct cl_conn_ops *ops)
{
  memset(cc, 0, sizeof(*cc));
  cc->cc_ops = ops;

  ev_init(&cc->cc_timer_w, &cl_conn_timer_cb);
  cc->cc_timer_w.repeat = ops->cc_timeout;

  ev_init(&cc->cc_io_w, &cl_conn_io_cb);
  cc->cc_io_w.fd = -1;

  if (n_buf_init(&cc->cc_rd_buf, ops->cc_rd_buf_size) < 0)
    goto err;
  if (n_buf_init(&cc->cc_wr_buf, ops->cc_wr_buf_size) < 0)
    goto err;

  return 0;

 err:
  n_buf_destroy(&cc->cc_rd_buf);
  n_buf_destroy(&cc->cc_wr_buf);

  return -1;
}

int cl_conn_set(struct cl_conn *cc, int fd, int events, const char *name)
{
  TRACE("cl_conn `%s' SET fd %d, name `%s'\n", cl_conn_name(cc), fd, name);

  free(cc->cc_name);
  cc->cc_name = strdup(name);
  if (cc->cc_name == NULL)
    return -1;

  ev_io_set(&cc->cc_io_w, fd, events);

  return 0;
}

void cl_conn_start(EV_P_ struct cl_conn *cc)
{
  TRACE("cl_conn `%s' START\n", cl_conn_name(cc));

  cc->cc_rd_eof = 0;
  ev_timer_start(EV_A_ &cc->cc_timer_w);
  ev_io_start(EV_A_ &cc->cc_io_w);
  cl_conn_up(EV_A_ cc, 0);
}

void cl_conn_stop(EV_P_ struct cl_conn *cc)
{
  TRACE("cl_conn `%s' STOP\n", cl_conn_name(cc));

  ev_timer_stop(EV_A_ &cc->cc_timer_w);
  ev_io_stop(EV_A_ &cc->cc_io_w);
}

void cl_conn_close(EV_P_ struct cl_conn *cc)
{
  cl_conn_stop(EV_A_ cc);

  if (!(cc->cc_io_w.fd < 0))
    close(cc->cc_io_w.fd);
  cc->cc_io_w.fd = -1;
}

int cl_conn_move(EV_P_ struct cl_conn *cc, struct cl_conn *src)
{
  cl_conn_close(EV_A_ cc);
  cl_conn_stop(EV_A_ src);

  if (n_buf_copy(&cc->cc_rd_buf, &src->cc_rd_buf) < 0)
    return -1;

  if (n_buf_copy(&cc->cc_wr_buf, &src->cc_wr_buf) < 0)
    return -1;

  if (cl_conn_set(cc, src->cc_io_w.fd, EV_READ|EV_WRITE, src->cc_name) < 0)
    return -1;

  src->cc_io_w.fd = -1;

  cl_conn_start(EV_A_ cc);

  ev_feed_event(EV_A_ &cc->cc_io_w, EV_READ|EV_WRITE);

  return 0;
}

void cl_conn_destroy(struct cl_conn *cc)
{
  if (ev_is_active(&cc->cc_timer_w) || ev_is_active(&cc->cc_io_w))
    FATAL("destroying cl_conn `%s' with active watchers\n", cl_conn_name(cc));

  TRACE("cl_conn destroy `%s'\n", cl_conn_name(cc));

  if (cc->cc_io_w.fd >= 0)
    close(cc->cc_io_w.fd);
  cc->cc_io_w.fd = -1;

  n_buf_destroy(&cc->cc_rd_buf);
  n_buf_destroy(&cc->cc_wr_buf);

  free(cc->cc_name);
  cc->cc_name = NULL;
}

static int cl_conn_ctl_cmp(const char *name, const struct cl_conn_ctl *ctl)
{
  return strcmp(name, ctl->cc_ctl_name);
}

static int cl_conn_ctl_msg(EV_P_ struct cl_conn *cc, char *msg)
{
  char *name = NULL, *tid;
  int (**ctl_cb)(EV_P_ struct cl_conn *, struct ctl_data *);
  struct ctl_data cd;
  int cl_err;

  memset(&cd, 0, sizeof(cd));

  if (split(&msg, &name, &tid, (char *) NULL) != 2) {
    cl_err = CL_ERR_NR_ARGS;
    goto err;
  }

  cd.cd_name = name;
  ctl_cb = bsearch(name, cc->cc_ops->cc_ctl, cc->cc_ops->cc_nr_ctl,
                   sizeof(cc->cc_ops->cc_ctl[0]),
                   (int (*)(const void *, const void *)) &cl_conn_ctl_cmp);

  if (ctl_cb == NULL) {
    TRACE("cl_conn `%s', no call back for ctl_name `%s'\n",
          cl_conn_name(cc), name);
    cl_err = CL_ERR_NO_CTL;
    goto err;
  }

  cd.cd_tid = strtoull(tid, NULL, 16);
  cd.cd_args = (msg != NULL) ? msg : tid + strlen(tid);
  cl_err = (**ctl_cb)(EV_A_ cc, &cd);

 err:
  if (cl_err != CL_ERR_ENDED && cl_err != CL_ERR_MOVED)
    cl_conn_writef(EV_A_ cc, "%c%s %"PRI_TID" %d %s\n",
                   CL_CONN_CTL_CHAR, name != NULL ? name : "NONE",
                   cd.cd_tid, cl_err, cl_strerror(cl_err));

  return cl_err;
}

static int cl_conn_rd(EV_P_ struct cl_conn *cc)
{
  struct n_buf *nb = &cc->cc_rd_buf;
  int eof = 0, err = 0;

  TRACE("cl_conn `%s' RD nb_len %zu, nb `%.8s'\n", cl_conn_name(cc),
        nb->nb_end - nb->nb_start, nb->nb_buf + nb->nb_start);

  n_buf_fill(nb, cc->cc_io_w.fd, &eof, &err);

  TRACE("cl_conn `%s' RD nb_len %zu, eof %d, err %d\n", cl_conn_name(cc),
        nb->nb_end - nb->nb_start, eof, err);

  if (eof)
    cc->cc_rd_eof = 1;

  char *msg;
  size_t msg_len;

  while (err == 0 && n_buf_get_msg(nb, &msg, &msg_len) == 0) {
    if (*msg == CL_CONN_CTL_CHAR)
      err = cl_conn_ctl_msg(EV_A_ cc, msg + 1);
    else if (cc->cc_ops->cc_msg_cb != NULL)
      err = (*cc->cc_ops->cc_msg_cb)(EV_A_ cc, msg);
    else
      /* Do nothing. */;
  }

  return err;
}

static int cl_conn_wr(EV_P_ struct cl_conn *cc)
{
  struct n_buf *nb = &cc->cc_wr_buf;
  int err = 0;
  ssize_t rc;

  TRACE("cl_conn `%s', nb_start %zu, nb_end %zu, nb_size %zu\n",
        cl_conn_name(cc), nb->nb_start, nb->nb_end, nb->nb_size);

  if (n_buf_is_empty(nb))
    return 0;

  errno = 0;
  rc = write(cc->cc_io_w.fd, nb->nb_buf + nb->nb_start,
             nb->nb_end - nb->nb_start);

  TRACE("cl_conn `%s', fd %d, rc %zd, errno %d\n",
        cl_conn_name(cc), cc->cc_io_w.fd, rc, errno);

  err = (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : errno;

  if (rc > 0)
    nb->nb_start += rc;

  return err;
}

static void cl_conn_end(EV_P_ struct cl_conn *cc, int err)
{
  TRACE("cl_conn `%s' END err %d %s\n", cl_conn_name(cc), err, cl_strerror(err));

  if (err == CL_ERR_ENDED || err == CL_ERR_MOVED)
    err = 0;

  if (cc->cc_ops->cc_end_cb != NULL) {
    (*cc->cc_ops->cc_end_cb)(EV_A_ cc, err);
    return;
  }

  /* Try to return error to peer. */
  if (cc->cc_io_w.fd >= 0 && err != 0) {
    char err_buf[80];
    int err_len;

    err_len = snprintf(err_buf, sizeof(err_buf), "%cerror %d %s\n",
                       CL_CONN_CTL_CHAR, err, cl_strerror(err));

    if (0 < err_len && err_len < sizeof(err_buf))
      write(cc->cc_io_w.fd, err_buf, err_len);
  }

  cl_conn_stop(EV_A_ cc);
  cl_conn_destroy(cc);
  free(cc);
}

static void cl_conn_timer_cb(EV_P_ ev_timer *w, int revents)
{
  struct cl_conn *cc = container_of(w, struct cl_conn, cc_timer_w);

  TRACE("cl_conn `%s' TIMER revents %d\n", cl_conn_name(cc), revents);

  cl_conn_end(EV_A_ cc, ETIMEDOUT);
}

static void cl_conn_up(EV_P_ struct cl_conn *cc, int err)
{
  int events;

  TRACE("cl_conn `%s' UP err %d\n", cl_conn_name(cc), err);

  if (err != 0) {
    cl_conn_end(EV_A_ cc, err);
    return;
  }

  events = 0;

  if (!cc->cc_rd_eof)
    events |= EV_READ;

  if (!n_buf_is_empty(&cc->cc_wr_buf))
    events |= EV_WRITE;

  TRACE("cl_conn `%s', old events %d, new events %d\n",
        cl_conn_name(cc), cc->cc_io_w.events, events);

  if (events == 0) {
    cl_conn_end(EV_A_ cc, 0);
    return;
  }

  ev_timer_again(EV_A_ &cc->cc_timer_w);
  if (events != cc->cc_io_w.events) {
    ev_io_stop(EV_A_ &cc->cc_io_w);
    ev_io_set(&cc->cc_io_w, cc->cc_io_w.fd, events);
    ev_io_start(EV_A_ &cc->cc_io_w);
  }
}

static void cl_conn_io_cb(EV_P_ ev_io *w, int revents)
{
  struct cl_conn *cc = container_of(w, struct cl_conn, cc_io_w);
  int cl_err = 0;

  TRACE("cl_conn `%s', IO revents %d\n", cl_conn_name(cc), revents);

  if (revents & EV_ERROR) {
    ERROR("cl_conn `%s', EV_ERROR: %m\n", cl_conn_name(cc));
    cl_err = CL_ERR_INTERNAL;
  }

  if (revents & EV_READ) {
    cl_err = cl_conn_rd(EV_A_ cc);
    if (cl_err != 0)
      goto out;
  }

  if (revents & EV_WRITE) {
    cl_err = cl_conn_wr(EV_A_ cc);
    if (cl_err != 0)
      goto out;
  }

 out:
  cl_conn_up(EV_A_ cc, cl_err);
}

int cl_conn_writef(EV_P_ struct cl_conn *cc, const char *fmt, ...)
{
  struct n_buf *nb = &cc->cc_wr_buf;
  int len, err = 0;
  va_list args;
  va_start(args, fmt);

  n_buf_pullup(nb);

  errno = 0;
  len = vsnprintf(nb->nb_buf + nb->nb_end, nb->nb_size - nb->nb_end, fmt, args);
  if (len < 0)
    err = CL_ERR_INTERNAL;
  else if (nb->nb_size - nb->nb_end < len)
    err = ENOBUFS;
  else
    nb->nb_end += len;

  va_end(args);

  /* XXX Don't pass err to cl_conn_up() so that cc doesn't get
     destroyed. */
  cl_conn_up(EV_A_ cc, 0);

  return err;
}
