/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
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

/* This file contains both the uv__async internal infrastructure and the
 * user-facing uv_async_t functions.
 */

#include "uv.h"
#include "internal.h"
#include "atomic-ops.h"

#include <errno.h>
#include <stdio.h>  /* snprintf() */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>  /* sched_yield() */

#ifdef __linux__
#include <sys/eventfd.h>
#endif

static void uv__async_send(uv_loop_t* loop);
static int uv__async_start(uv_loop_t* loop);


int uv_async_init(uv_loop_t* loop, uv_async_t* handle, uv_async_cb async_cb) {
  int err;

  // 给 libuv 注册一个用于异步通信的 io 观察者
  err = uv__async_start(loop);
  if (err)
    return err;

  // 设置相关字段，给 libuv 插入一个 async_handle
  uv__handle_init(loop, (uv_handle_t*)handle, UV_ASYNC);
  handle->async_cb = async_cb;
  // 标记是否有任务完成了
  handle->pending = 0;

  // 插入 async 队列，poll io 阶段判断是否有任务与完成
  QUEUE_INSERT_TAIL(&loop->async_handles, &handle->queue);
  // 激活 handle 为 active 状态
  uv__handle_start(handle);

  return 0;
}


int uv_async_send(uv_async_t* handle) {
  /* Do a cheap read first. */
  if (ACCESS_ONCE(int, handle->pending) != 0)
    return 0;

  /* Tell the other thread we're busy with the handle. */
  if (cmpxchgi(&handle->pending, 0, 1) != 0)
    return 0;

  /* Wake up the other thread's event loop. */
  uv__async_send(handle->loop);

  /* Tell the other thread we're done. */
  if (cmpxchgi(&handle->pending, 1, 2) != 1)
    abort();

  return 0;
}


/*
判断哪些 async 被触发了。pending 在 uv_async_send
里设置成 1，如果 pending 等于 1，则清 0，返回 1.如果
pending 等于 0，则返回 0
*/

/* Only call this from the event loop thread. */
static int uv__async_spin(uv_async_t* handle) {
  int i;
  int rc;

  for (;;) {
    /* 997 is not completely chosen at random. It's a prime number, acyclical
     * by nature, and should therefore hopefully dampen sympathetic resonance.
     */
    for (i = 0; i < 997; i++) {
      /* rc=0 -- handle is not pending.
       * rc=1 -- handle is pending, other thread is still working with it.
       * rc=2 -- handle is pending, other thread is done.
       */
      rc = cmpxchgi(&handle->pending, 2, 0);

      if (rc != 1)
        return rc;

      /* Other thread is busy with this handle, spin until it's done. */
      cpu_relax();
    }

    /* Yield the CPU. We may have preempted the other thread while it's
     * inside the critical section and if it's running on the same CPU
     * as us, we'll just burn CPU cycles until the end of our time slice.
     */
    sched_yield();
  }
}


void uv__async_close(uv_async_t* handle) {
  uv__async_spin(handle);
  QUEUE_REMOVE(&handle->queue);
  uv__handle_stop(handle);
}

/*
uv__async_io 会遍历 loop->async_handles 队里中所有的 uv_async_t。然后判断该
uv_async_t 是否有事件触发（通过 uv_async_t->pending 字段）。如果有的话，则执
行该 uv_async_t 对应的回调
*/
static void uv__async_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  char buf[1024];
  ssize_t r;
  QUEUE queue;
  QUEUE* q;
  uv_async_t* h;

  // 用于异步通信的 io 观察者
  assert(w == &loop->async_io_watcher);

  for (;;) {
  	// 判断通信内容
    r = read(w->fd, buf, sizeof(buf));

    // 如果数据大于 buf 的长度，接着读，清空这一轮写入的数据
    if (r == sizeof(buf))
      continue;

    // 不等于-1，说明读成功，失败的时候返回-1，errno 是错误码
    if (r != -1)
      break;

    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;

	// 被信号中断，继续读
    if (errno == EINTR)
      continue;
    // 出错，发送 abort 信号
    abort();
  }

  // 把 async_handles 队列里的所有节点都移到 queue 变量中
  QUEUE_MOVE(&loop->async_handles, &queue);
  // 逐个取出节点
  while (!QUEUE_EMPTY(&queue)) {
    q = QUEUE_HEAD(&queue);
	// 根据结构体字段获取结构体首地址
    h = QUEUE_DATA(q, uv_async_t, queue);

	// 从队列中移除该节点
    QUEUE_REMOVE(q);
	// 重新插入 async_handles 队列，等待下次事件
    QUEUE_INSERT_TAIL(&loop->async_handles, q);

    if (0 == uv__async_spin(h))
      continue;  /* Not pending. */

    if (h->async_cb == NULL)
      continue;

	// 执行上层回调
    h->async_cb(h);
  }
}

// 通知主线程有任务完成
static void uv__async_send(uv_loop_t* loop) {
  const void* buf;
  ssize_t len;
  int fd;
  int r;
  
/*
	设置 async handle 的 pending 标记
	如果 pending 是 0，则设置为 1，返回 0，如果是 1 则返回 1，
	所以同一个 handle 如果多次调用该函数是会被合并的
*/

  buf = "";
  len = 1;
  // 用于异步通信的管道的写端
  fd = loop->async_wfd;

  // 说明用的是 eventfd 而不是管道
#if defined(__linux__)
  if (fd == -1) {
    static const uint64_t val = 1;
    buf = &val;
    len = sizeof(val);
    fd = loop->async_io_watcher.fd;  /* eventfd */
  }
#endif

  // 通知读端
  do
    r = write(fd, buf, len);
  while (r == -1 && errno == EINTR);

  if (r == len)
    return;

  if (r == -1)
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;

  abort();
}

//初始化异步通信的 io 观察者
static int uv__async_start(uv_loop_t* loop) {
  int pipefd[2];
  int err;

/*
	因为 libuv 在初始化的时候会主动注册一个
	用于主线程和子线程通信的 async handle。
	从而初始化了 async_io_watcher。所以如果后续
	再注册 async handle，则不需要处理了。
 
	父子线程通信时，libuv 是优先使用 eventfd，如果不支持会回退到匿名管道。
 	如果是匿名管道
 	fd 是管道的读端，loop->async_wfd 是管道的写端
 	如果是 eventfd
 	fd 是读端也是写端。async_wfd 是-1 

 	所以这里判断 loop->async_io_watcher.fd 而不是 async_wfd 的值
 */

  if (loop->async_io_watcher.fd != -1)
    return 0;

  // 获取一个用于进程间通信的 fd
#ifdef __linux__
  err = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (err < 0)
    return UV__ERR(errno);

  pipefd[0] = err;
  pipefd[1] = -1;
#else
  err = uv__make_pipe(pipefd, UV__F_NONBLOCK);
  if (err < 0)
    return err;
#endif

  // 初始化 io 观察者 async_io_watcher
  uv__io_init(&loop->async_io_watcher, uv__async_io, pipefd[0]);
  // 注册 io 观察者到 loop 里，并注册需要监听的事件 POLLIN，读
  uv__io_start(loop, &loop->async_io_watcher, POLLIN);
  // 用于主线程和子线程通信的 fd，管道的写端，子线程使用
  loop->async_wfd = pipefd[1];

  return 0;
}


int uv__async_fork(uv_loop_t* loop) {
  if (loop->async_io_watcher.fd == -1) /* never started */
    return 0;

  uv__async_stop(loop);

  return uv__async_start(loop);
}


void uv__async_stop(uv_loop_t* loop) {
  if (loop->async_io_watcher.fd == -1)
    return;

  if (loop->async_wfd != -1) {
    if (loop->async_wfd != loop->async_io_watcher.fd)
      uv__close(loop->async_wfd);
    loop->async_wfd = -1;
  }

  uv__io_stop(loop, &loop->async_io_watcher, POLLIN);
  uv__close(loop->async_io_watcher.fd);
  loop->async_io_watcher.fd = -1;
}
