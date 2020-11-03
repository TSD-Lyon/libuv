/* Copyright libuv project and contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <sys/un.h>

#ifndef IOURING_SQ_SIZE
# define IOURING_SQ_SIZE 4096
#endif

#define UV__UDP_DGRAM_MAXSIZE (64 * 1024)


static int uv__io_uring_submit(struct uv__io_uring_data* io_uring);
static struct io_uring_sqe*
  uv__io_uring_get_sqe(struct uv__io_uring_data* io_uring);
static void uv__uring_udp_sendmsg_done(uv_udp_send_t* req, int status);
static void uv__uring_udp_recvmsg_done(uv_udp_t* handle, int status);


struct uv__io_uring_data* uv__get_io_uring(uv_loop_t* loop) {
  return (struct uv__io_uring_data*)loop->io_uring;
}


int uv__uring_platform_loop_init(uv_loop_t* loop) {
  int r;
  struct uv__io_uring_data* io_uring;

  loop->backend_fd = -1;
  loop->inotify_fd = -1;
  loop->inotify_watchers = NULL;
  loop->io_uring = NULL;

  io_uring = uv__malloc(sizeof(*io_uring));
  if (io_uring == NULL)
    return UV_ENOMEM;

  r = io_uring_queue_init(IOURING_SQ_SIZE, &io_uring->ring, 0);
  if (r != 0) {
    uv__free(io_uring);
    return UV__ERR(errno);
  }

  io_uring->sync_limit = 40;
  loop->io_uring = io_uring;

  return 0;
}


void uv__uring_platform_loop_delete(uv_loop_t* loop) {
  struct uv__io_uring_data* io_uring;

  io_uring = uv__get_io_uring(loop);
  if (io_uring != NULL) {
    io_uring_queue_exit(&io_uring->ring);
    uv__free(io_uring);
    loop->io_uring = NULL;
  }

  if (loop->inotify_fd == -1) return;
  uv__io_stop(loop, &loop->inotify_read_watcher, POLLIN);
  uv__close(loop->inotify_fd);
  loop->inotify_fd = -1;
}


void uv__uring_platform_invalidate_fd(uv_loop_t* loop, int fd) {
  // remove stale events for that fd
  struct uv__io_uring_data* io_uring;
  struct io_uring_sqe* sqe;

  assert(loop->watchers != NULL);
  assert(fd >= 0);

  io_uring = uv__get_io_uring(loop);
  if (io_uring != NULL && loop->watchers[fd]) {
    sqe = uv__io_uring_get_sqe(io_uring);
    assert(sqe != NULL);
    io_uring_prep_poll_remove(sqe, (void*)loop->watchers[fd]);
    sqe->user_data = 0;
    assert(0 <= uv__io_uring_submit(io_uring));
  }
}


int uv__uring_io_check_fd(uv_loop_t* loop, int fd) {
  struct pollfd p[1];
  int rv;

  p[0].fd = fd;
  p[0].events = POLLIN;

  do
    rv = poll(p, 1, 0);
  while (rv == -1 && (errno == EINTR || errno == EAGAIN));

  if (rv == -1)
    return UV__ERR(errno);

  if (p[0].revents & POLLNVAL)
    return UV_EINVAL;

  return 0;
}


void uv__uring_io_poll(uv_loop_t* loop, int timeout) {
  struct uv__io_uring_data* io_uring;
  struct io_uring* ring;
  struct io_uring_cqe* cqe;
  struct io_uring_sqe* sqe;
  unsigned head;
  int count;
  int real_timeout;
  QUEUE* q;
  uv__io_t* w;
  sigset_t sigset;
  uint64_t sigmask;
  uint64_t base;
  int have_signals;
  int nevents;
  int r;
  uint32_t events;
  struct __kernel_timespec ts;
  int user_timeout;
  int reset_timeout;

  if (loop->nfds == 0) {
    assert(QUEUE_EMPTY(&loop->watcher_queue));
    return;
  }

  io_uring = uv__get_io_uring(loop);
  ring = &io_uring->ring;
  while (!QUEUE_EMPTY(&loop->watcher_queue)) {
    q = QUEUE_HEAD(&loop->watcher_queue);
    QUEUE_REMOVE(q);
    QUEUE_INIT(q);

    w = QUEUE_DATA(q, uv__io_t, watcher_queue);
    assert(w->pevents != 0);
    assert(w->fd >= 0);
    assert(w->fd < (int) loop->nwatchers);

    sqe = uv__io_uring_get_sqe(io_uring);
    assert(sqe != NULL);

    io_uring_prep_poll_add(sqe, w->fd, w->pevents);
    sqe->user_data = (uint64_t)w;
    if (io_uring_sq_ready(&io_uring->ring) > io_uring->sync_limit)
      sqe->flags = IOSQE_ASYNC;

    w->events = w->pevents;
  }

  r = uv__io_uring_submit(io_uring);
  assert(r >= 0);

  // As the io_uring_submit may take a while!
  SAVE_ERRNO(uv__update_time(loop));

  sigmask = 0;
  if (loop->flags & UV_LOOP_BLOCK_SIGPROF) {
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGPROF);
    sigmask |= 1 << (SIGPROF - 1);
  }

  assert(timeout >= -1);
  base = loop->time;
  real_timeout = timeout;

  have_signals = 0;
  nevents = 0;

  if (uv__get_internal_fields(loop)->flags & UV_METRICS_IDLE_TIME) {
    reset_timeout = 1;
    user_timeout = timeout;
    timeout = 0;
  } else {
    reset_timeout = 0;
    user_timeout = 0;
  }

  for (;;) {
    /* Only need to set the provider_entry_time if timeout != 0. The function
     * will return early if the loop isn't configured with UV_METRICS_IDLE_TIME.
     */
    if (timeout != 0)
      uv__metrics_set_provider_entry_time(loop);

    /*
     * If there're already cqe's to be consumed, no need to wait for them
     */
    if (io_uring_cq_ready(ring) == 0) {
      if (sigmask != 0)
        if (pthread_sigmask(SIG_BLOCK, &sigset, NULL))
          abort();

      if (timeout > 0) {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (timeout % 1000) * 1000000;
      }

      do {
        r = io_uring_wait_cqes(ring,
                              &cqe,
                              timeout == 0 ? 0 : 1,
                              timeout > 0 ? &ts : NULL,
                              sigmask != 0 ? &sigset : NULL);
      } while (r == -EINTR);
      assert(r == 0 || r == -EAGAIN || r == -ETIME);
      timeout = real_timeout;

      if (sigmask != 0)
        if (pthread_sigmask(SIG_UNBLOCK, &sigset, NULL))
          abort();

      if (r == EAGAIN) {
        assert(timeout == 0);
        if (reset_timeout != 0) {
          timeout = user_timeout;
          reset_timeout = 0;
        }

        if (timeout == 0)
          return;

        /* We may have been inside the system call for longer than |timeout|
        * milliseconds so we need to update the timestamp to avoid drift.
        */
        goto update_timeout;
      }

      /* Update loop->time unconditionally. It's tempting to skip the update when
      * timeout == 0 (i.e. non-blocking poll) but there is no guarantee that the
      * operating system didn't reschedule our process while in the syscall.
      */
      SAVE_ERRNO(uv__update_time(loop));

      if (r == -ETIME) {
        /*
        * No events available. It returned because of a timeout.
        */

        if (reset_timeout != 0) {
          timeout = user_timeout;
          reset_timeout = 0;
        }

        if (timeout == -1)
          continue;

        if (timeout == 0)
          return;

        /* We may have been inside the system call for longer than |timeout|
        * milliseconds so we need to update the timestamp to avoid drift.
        */
        goto update_timeout;
      }
    }

    count = 0;
    io_uring_for_each_cqe(ring, head, cqe) {
      count++;
      /* Ignore timeouts and cancelled requests */
      if (cqe->user_data == LIBURING_UDATA_TIMEOUT ||
          cqe->user_data == 0) {
        continue;
      }

      int* op_type = (int*)cqe->user_data;
      if (*op_type == UV__URING_UDP_SENDMSG) {
        uv_udp_send_t* req = container_of((uintptr_t*)cqe->user_data,
                                          uv_udp_send_t,
                                          un.uring_req_type);
        uv__metrics_update_idle_time(loop);
        uv__uring_udp_sendmsg_done(req, cqe->res);
        nevents++;
        continue;
      }

      if (*op_type == UV__URING_UDP_RECVMSG) {
        uv_udp_t* handle = container_of((uintptr_t*)cqe->user_data,
                                        uv_udp_t,
                                        un.uring_req_type);
        uv__metrics_update_idle_time(loop);
        uv__uring_udp_recvmsg_done(handle, cqe->res);
        nevents++;
        continue;
      }

      w = (uv__io_t*)cqe->user_data;
      events = cqe->res;

      if (w->fd == -1 || loop->watchers[w->fd] == NULL) {
        /* don't arm again if already closed */
        continue;
      }

      /* arm the watcher again as IORING_OP_POLL_ADD works as EPOLLONESHOT */
      if (!w->oneshot) {
        w->events = 0;
        uv__io_start(loop, w, w->pevents);
        w->events = w->pevents;
      }


      /* Give users only events they're interested in. Prevents spurious
       * callbacks when previous callback invocation in this loop has stopped
       * the current watcher. Also, filters out events that users has not
       * requested us to watch.
       */
      events &= w->pevents | POLLERR | POLLHUP;

      if (events != 0) {
        /* Run signal watchers last.  This also affects child process watchers
         * because those are implemented in terms of signal watchers.
         */
        if (w == &loop->signal_io_watcher) {
          have_signals = 1;
        } else {
          uv__metrics_update_idle_time(loop);
          w->cb(loop, w, events);
        }

        nevents++;
      }
    }

    io_uring_cq_advance(ring, count);

    if (reset_timeout != 0) {
      timeout = user_timeout;
      reset_timeout = 0;
    }

    if (have_signals != 0) {
      uv__metrics_update_idle_time(loop);
      loop->signal_io_watcher.cb(loop, &loop->signal_io_watcher, POLLIN);
      return;  /* Event loop should cycle now so don't poll again. */
    }

    if (nevents != 0)
      return;

    if (timeout == 0)
      return;

    if (timeout == -1)
      continue;

update_timeout:
    assert(timeout > 0);
    real_timeout -= (loop->time - base);
    if (real_timeout <= 0)
      return;

    timeout = real_timeout;
  }
}


int uv__io_uring_submit(struct uv__io_uring_data* io_uring) {
  int r;

  if (io_uring_sq_ready(&io_uring->ring)) {
    r = io_uring_submit(&io_uring->ring);
    if (r < 0) {
      /* Can't have more reqs pending. Wait for more completions */
      if (r == UV_EBUSY)
        return 0;

      return r;
    }

    return r;
  }

  return 0;
}


struct io_uring_sqe* uv__io_uring_get_sqe(struct uv__io_uring_data* io_uring) {
  struct io_uring_sqe* sqe;

  sqe = io_uring_get_sqe(&io_uring->ring);
  if (sqe == NULL) {
    // We're full! Submit and try again
    assert(0 <= uv__io_uring_submit(io_uring));
    sqe = io_uring_get_sqe(&io_uring->ring);
  }

  return sqe;
}

void uv__uring_udp_sendmsg(uv_udp_t* handle) {
  uv_udp_send_t* req;
  QUEUE* q;
  struct uv__io_uring_data* io_uring;
  struct io_uring_sqe* sqe;

  while (!QUEUE_EMPTY(&handle->write_queue)) {
    q = QUEUE_HEAD(&handle->write_queue);
    assert(q != NULL);

    req = QUEUE_DATA(q, uv_udp_send_t, queue);
    assert(req != NULL);

    memset(&req->h, 0, sizeof req->h);
    if (req->addr.ss_family == AF_UNSPEC) {
      req->h.msg_name = NULL;
      req->h.msg_namelen = 0;
    } else {
      req->h.msg_name = &req->addr;
      if (req->addr.ss_family == AF_INET6)
        req->h.msg_namelen = sizeof(struct sockaddr_in6);
      else if (req->addr.ss_family == AF_INET)
        req->h.msg_namelen = sizeof(struct sockaddr_in);
      else if (req->addr.ss_family == AF_UNIX)
        req->h.msg_namelen = sizeof(struct sockaddr_un);
      else {
        assert(0 && "unsupported address family");
        abort();
      }
    }
    req->h.msg_iov = (struct iovec*) req->bufs;
    req->h.msg_iovlen = req->nbufs;

    io_uring = handle->loop->io_uring;
    sqe = uv__io_uring_get_sqe(io_uring);
    assert(sqe != NULL);

    io_uring_prep_sendmsg(sqe, handle->io_watcher.fd, &req->h, 0);
    req->un.uring_req_type = UV__URING_UDP_SENDMSG;
    sqe->user_data = (uint64_t)&req->un.uring_req_type;
    if (io_uring_sq_ready(&io_uring->ring) > io_uring->sync_limit)
      sqe->flags = IOSQE_ASYNC;

    QUEUE_REMOVE(&req->queue);
    QUEUE_INSERT_TAIL(&handle->write_pending_queue, &req->queue);
  }
}


void uv__uring_udp_sendmsg_done(uv_udp_send_t* req, int status) {
  uv_udp_t* handle;

  handle = req->handle;
  if (status == UV_EAGAIN ||
      status == UV__ERR(EWOULDBLOCK) ||
      status == UV_ENOBUFS) {
    QUEUE_REMOVE(&req->queue);
    QUEUE_INSERT_TAIL(&handle->write_queue, &req->queue);
    return;
  }

  req->status = status;

  /* Sending a datagram is an atomic operation: either all data
   * is written or nothing is (and EMSGSIZE is raised). That is
   * why we don't handle partial writes. Just pop the request
   * off the write queue and onto the completed queue, done.
   */
  QUEUE_REMOVE(&req->queue);
  QUEUE_INSERT_TAIL(&handle->write_completed_queue, &req->queue);
  uv__io_feed(handle->loop, &handle->io_watcher);
}


void uv__uring_udp_recvmsg(uv_udp_t* handle) {
  struct uv__io_uring_data* io_uring;
  struct io_uring_sqe* sqe;

  if (handle->flags & UV_HANDLE_READ_PENDING)
    return;

  assert(handle->recv_cb != NULL);
  assert(handle->alloc_cb != NULL);

  io_uring = handle->loop->io_uring;

  handle->recv_buf = uv_buf_init(NULL, 0);
  handle->alloc_cb((uv_handle_t*) handle,
                   UV__UDP_DGRAM_MAXSIZE,
                   &handle->recv_buf);
  if (handle->recv_buf.base == NULL || handle->recv_buf.len == 0) {
    uv__uring_udp_recvmsg_done(handle, UV_ENOBUFS);
    return;
  }

  assert(handle->recv_buf.base != NULL);

  memset(&handle->h, 0, sizeof(handle->h));
  handle->h.msg_name = &handle->peer;
  handle->h.msg_namelen = sizeof(handle->peer);
  handle->h.msg_iov = (void*) &handle->recv_buf;
  handle->h.msg_iovlen = 1;

  handle->flags |= UV_HANDLE_READ_PENDING;

  sqe = uv__io_uring_get_sqe(io_uring);
  assert(sqe != NULL);

  io_uring_prep_recvmsg(sqe, handle->io_watcher.fd, &handle->h, 0);
  handle->un.uring_req_type = UV__URING_UDP_RECVMSG;
  sqe->user_data = (uint64_t)&handle->un.uring_req_type;
  if (io_uring_sq_ready(&io_uring->ring) > io_uring->sync_limit)
    sqe->flags = IOSQE_ASYNC;
}


void uv__uring_udp_recvmsg_done(uv_udp_t* handle, int status) {
  int flags;

  if (uv__is_closing(handle)) {
    return;
  }

  if (status == UV_EBADF) {
    status = UV_ECANCELED;
  }

  handle->flags &= ~UV_HANDLE_READ_PENDING;
  if (status < 0) {
    if (status == UV_EAGAIN || status == UV__ERR(EWOULDBLOCK)) {
      handle->recv_cb(handle, 0, &handle->recv_buf, NULL, 0);
      if (uv__io_active(&handle->io_watcher, POLLIN) && !uv__is_closing(handle))
        uv__uring_udp_recvmsg(handle);
    } else {
      handle->recv_cb(handle, status, &handle->recv_buf, NULL, 0);
    }
  } else {
    flags = 0;
    if (handle->h.msg_flags & MSG_TRUNC)
      flags |= UV_UDP_PARTIAL;

    handle->recv_cb(handle,
                    status,
                    &handle->recv_buf,
                    (const struct sockaddr*)&handle->peer,
                    flags);
    if (uv__io_active(&handle->io_watcher, POLLIN) && !uv__is_closing(handle))
      uv__uring_udp_recvmsg(handle);
  }
}
