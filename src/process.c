/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

extern char **environ;

#include <grp.h>

#ifdef UV_HAVE_KQUEUE
#include <sys/event.h>
#else
#define UV_USE_SIGCHLD
#endif


#ifdef UV_USE_SIGCHLD
static void uv__chld(uv_signal_t* handle, int signum) {
  assert(signum == SIGCHLD);
  uv__wait_children(handle->loop);
}


int uv__process_init(uv_loop_t* loop) {
  int err;

  err = uv_signal_init(loop, &loop->child_watcher);
  if (err) {
    return err;
  }
  uv__handle_unref(&loop->child_watcher);
  loop->child_watcher.flags |= UV_HANDLE_INTERNAL;
  return 0;
}


#else
int uv__process_init(uv_loop_t* loop) {
  memset(&loop->child_watcher, 0, sizeof(loop->child_watcher));
  return 0;
}
#endif


void uv__wait_children(uv_loop_t* loop) {
  uv_process_t* process;
  int exit_status;
  int term_signal;
  int status;
  int options;
  pid_t pid;
  struct uv__queue pending;
  struct uv__queue* q;
  struct uv__queue* h;

  uv__queue_init(&pending);

  h = &loop->process_handles;
  q = uv__queue_head(h);
  while (q != h) {
    process = uv__queue_data(q, uv_process_t, queue);
    q = uv__queue_next(q);

#ifndef UV_USE_SIGCHLD
    if ((process->flags & UV_HANDLE_REAP) == 0)
      continue;
    options = 0;
    process->flags &= ~UV_HANDLE_REAP;
    loop->nfds--;
#else
    options = WNOHANG;
#endif

    do {
      pid = waitpid(process->pid, &status, options);
    } while (pid == -1 && errno == EINTR);

#ifdef UV_USE_SIGCHLD
    if (pid == 0) /* Not yet exited */
      continue;
#endif

    if (pid == -1) {
      if (errno != ECHILD) {
        abort();
      }
      /* The child died, and we missed it. This probably means someone else
       * stole the waitpid from us. Handle this by not handling it at all. */
      continue;
    }

    assert(pid == process->pid);
    process->status = status;
    uv__queue_remove(&process->queue);
    uv__queue_insert_tail(&pending, &process->queue);
  }

  h = &pending;
  q = uv__queue_head(h);
  while (q != h) {
    process = uv__queue_data(q, uv_process_t, queue);
    q = uv__queue_next(q);

    uv__queue_remove(&process->queue);
    uv__queue_init(&process->queue);
    uv__handle_stop(process);

    if (process->exit_cb == NULL) {
      continue;
    }

    exit_status = 0;
    if (WIFEXITED(process->status)) {
      exit_status = WEXITSTATUS(process->status);
    }

    term_signal = 0;
    if (WIFSIGNALED(process->status)) {
      term_signal = WTERMSIG(process->status);
    }

    process->exit_cb(process, exit_status, term_signal);
  }
  assert(uv__queue_empty(&pending));
}

/*
 * Used for initializing stdio streams like options.stdin_stream. Returns
 * zero on success. See also the cleanup section in uv_spawn().
 */
#if !((TARGET_OS_TV || TARGET_OS_WATCH))
/* execvp is marked __WATCHOS_PROHIBITED __TVOS_PROHIBITED, so must be
 * avoided. Since this isn't called on those targets, the function
 * doesn't even need to be defined for them.
 */
static int uv__process_init_stdio(uv_stdio_container_t* container, int fds[2]) {
  int mask;
  int fd;

  mask = UV_IGNORE | UV_CREATE_PIPE | UV_INHERIT_FD | UV_INHERIT_STREAM;

  switch (container->flags & mask) {
  case UV_IGNORE:
    return 0;

  case UV_CREATE_PIPE:
    assert(container->data.stream != NULL);
    if (container->data.stream->type != UV_NAMED_PIPE) {
      return UV_EINVAL;
    } else {
      return uv_socketpair(SOCK_STREAM, 0, fds, 0, 0);
    }

  case UV_INHERIT_FD:
  case UV_INHERIT_STREAM:
    if (container->flags & UV_INHERIT_FD) {
      fd = container->data.fd;
    } else {
      fd = uv__stream_fd(container->data.stream);
    }

    if (fd == -1) {
      return UV_EINVAL;
    }

    fds[1] = fd;
    return 0;

  default:
    assert(0 && "Unexpected flags");
    return UV_EINVAL;
  }
}


static int uv__process_open_stream(uv_stdio_container_t* container, int pipefds[2]) {
  int flags;
  int err;

  if (!(container->flags & UV_CREATE_PIPE) || pipefds[0] < 0) {
    return 0;
  }

  err = uv__close(pipefds[1]);
  if (err != 0) {
    abort();
  }

  pipefds[1] = -1;
  uv__nonblock(pipefds[0], 1);

  flags = 0;
  if (container->flags & UV_WRITABLE_PIPE) {
    flags |= UV_HANDLE_READABLE;
  }
  if (container->flags & UV_READABLE_PIPE) {
    flags |= UV_HANDLE_WRITABLE;
  }

  return uv__stream_open(container->data.stream, pipefds[0], flags);
}


static void uv__process_close_stream(uv_stdio_container_t* container) {
  if (!(container->flags & UV_CREATE_PIPE)) return;
  uv__stream_close(container->data.stream);
}


static void uv__write_int(int fd, int val) {
  ssize_t n;

  do {
    n = write(fd, &val, sizeof(val));
  } while (n == -1 && errno == EINTR);

  /* The write might have failed (e.g. if the parent process has died),
   * but we have nothing left but to _exit ourself now too. */
  _exit(127);
}


static void uv__write_errno(int error_fd) {
  uv__write_int(error_fd, UV__ERR(errno));
}


static void uv__process_child_init(const uv_process_options_t* options,
                                   int stdio_count,
                                   int (*pipes)[2],
                                   int error_fd) {
  sigset_t signewset;
  int close_fd;
  int use_fd;
  int fd;
  int n;

  /* Reset signal disposition first. Use a hard-coded limit because NSIG is not
   * fixed on Linux: it's either 32, 34 or 64, depending on whether RT signals
   * are enabled. We are not allowed to touch RT signal handlers, glibc uses
   * them internally.
   */
  for (n = 1; n < 32; n += 1) {
    if (n == SIGKILL || n == SIGSTOP) {
      continue;  /* Can't be changed. */
    }

    if (SIG_ERR != signal(n, SIG_DFL)) {
      continue;
    }

    uv__write_errno(error_fd);
  }

  if (options->flags & UV_PROCESS_DETACHED) {
    setsid();
  }

  /* First duplicate low numbered fds, since it's not safe to duplicate them,
   * they could get replaced. Example: swapping stdout and stderr; without
   * this fd 2 (stderr) would be duplicated into fd 1, thus making both
   * stdout and stderr go to the same fd, which was not the intention. */
  for (fd = 0; fd < stdio_count; fd++) {
    use_fd = pipes[fd][1];
    if (use_fd < 0 || use_fd >= fd) {
      continue;
    }
#ifdef F_DUPFD_CLOEXEC /* POSIX 2008 */
    pipes[fd][1] = fcntl(use_fd, F_DUPFD_CLOEXEC, stdio_count);
#else
    pipes[fd][1] = fcntl(use_fd, F_DUPFD, stdio_count);
#endif
    if (pipes[fd][1] == -1) {
      uv__write_errno(error_fd);
    }
#ifndef F_DUPFD_CLOEXEC /* POSIX 2008 */
    n = uv__cloexec(pipes[fd][1], 1);
    if (n) {
      uv__write_int(error_fd, n);
    }
#endif
  }

  for (fd = 0; fd < stdio_count; fd++) {
    close_fd = -1;
    use_fd = pipes[fd][1];

    if (use_fd < 0) {
      if (fd >= 3) {
        continue;
      } else {
        /* Redirect stdin, stdout and stderr to /dev/null even if UV_IGNORE is
         * set. */
        uv__close_nocheckstdio(fd); /* Free up fd, if it happens to be open. */
        use_fd = open("/dev/null", fd == 0 ? O_RDONLY : O_RDWR);
        close_fd = use_fd;

        if (use_fd < 0) {
          uv__write_errno(error_fd);
        }
      }
    }

    if (fd == use_fd) {
      if (close_fd == -1) {
        n = uv__cloexec(use_fd, 0);
        if (n) {
          uv__write_int(error_fd, n);
        }
      }
    } else {
      fd = dup2(use_fd, fd);
    }

    if (fd == -1) {
      uv__write_errno(error_fd);
    }

    if (fd <= 2 && close_fd == -1) {
      uv__nonblock_fcntl(fd, 0);
    }

    if (close_fd >= stdio_count) {
      uv__close(close_fd);
    }
  }

  if (options->cwd != NULL && chdir(options->cwd)) {
    uv__write_errno(error_fd);
  }

  if (options->flags & (UV_PROCESS_SETUID | UV_PROCESS_SETGID)) {
    /* When dropping privileges from root, the `setgroups` call will
     * remove any extraneous groups. If we don't call this, then
     * even though our uid has dropped, we may still have groups
     * that enable us to do super-user things. This will fail if we
     * aren't root, so don't bother checking the return value, this
     * is just done as an optimistic privilege dropping function.
     */
    SAVE_ERRNO(setgroups(0, NULL));
  }

  if ((options->flags & UV_PROCESS_SETGID) && setgid(options->gid)) {
    uv__write_errno(error_fd);
  }

  if ((options->flags & UV_PROCESS_SETUID) && setuid(options->uid)) {
    uv__write_errno(error_fd);
  }

  if (options->env != NULL) {
    environ = options->env;
  }

  /* Reset signal mask just before exec. */
  sigemptyset(&signewset);
  if (sigprocmask(SIG_SETMASK, &signewset, NULL) != 0) {
    abort();
  }

  execvp(options->file, options->args);

  uv__write_errno(error_fd);
}


static int uv__spawn_and_init_child_fork(const uv_process_options_t* options,
                                         int stdio_count,
                                         int (*pipes)[2],
                                         int error_fd,
                                         pid_t* pid) {
  sigset_t signewset;
  sigset_t sigoldset;

  /* Start the child with most signals blocked, to avoid any issues before we
   * can reset them, but allow program failures to exit (and not hang). */
  sigfillset(&signewset);
  sigdelset(&signewset, SIGKILL);
  sigdelset(&signewset, SIGSTOP);
  sigdelset(&signewset, SIGTRAP);
  sigdelset(&signewset, SIGSEGV);
  sigdelset(&signewset, SIGBUS);
  sigdelset(&signewset, SIGILL);
  sigdelset(&signewset, SIGSYS);
  sigdelset(&signewset, SIGABRT);
  if (pthread_sigmask(SIG_BLOCK, &signewset, &sigoldset) != 0) {
    abort();
  }

  *pid = fork();

  if (*pid == 0) {
    /* Fork succeeded, in the child process */
    uv__process_child_init(options, stdio_count, pipes, error_fd);
    abort();
  }

  if (pthread_sigmask(SIG_SETMASK, &sigoldset, NULL) != 0) {
    abort();
  }

  if (*pid == -1) {
    /* Failed to fork */
    return UV__ERR(errno);
  }

  /* Fork succeeded, in the parent process */
  return 0;
}

static int uv__spawn_and_init_child(
    uv_loop_t* loop,
    const uv_process_options_t* options,
    int stdio_count,
    int (*pipes)[2],
    pid_t* pid) {
  int signal_pipe[2] = { -1, -1 };
  int status;
  int err;
  int exec_errorno;
  ssize_t r;

  /* This pipe is used by the parent to wait until
   * the child has called `execve()`. We need this
   * to avoid the following race condition:
   *
   *    if ((pid = fork()) > 0) {
   *      kill(pid, SIGTERM);
   *    }
   *    else if (pid == 0) {
   *      execve("/bin/cat", argp, envp);
   *    }
   *
   * The parent sends a signal immediately after forking.
   * Since the child may not have called `execve()` yet,
   * there is no telling what process receives the signal,
   * our fork or /bin/cat.
   *
   * To avoid ambiguity, we create a pipe with both ends
   * marked close-on-exec. Then, after the call to `fork()`,
   * the parent polls the read end until it EOFs or errors with EPIPE.
   */
  err = uv__make_pipe(signal_pipe, 0);
  if (err) {
    return err;
  }

  /* Acquire write lock to prevent opening new fds in worker threads */
  uv_rwlock_wrlock(&loop->cloexec_lock);

  err = uv__spawn_and_init_child_fork(options, stdio_count, pipes, signal_pipe[1], pid);

  /* Release lock in parent process */
  uv_rwlock_wrunlock(&loop->cloexec_lock);

  uv__close(signal_pipe[1]);

  if (err == 0) {
    do {
      r = read(signal_pipe[0], &exec_errorno, sizeof(exec_errorno));
    } while (r == -1 && errno == EINTR);

    if (r == 0) {
      ; /* okay, EOF */
    } else if (r == sizeof(exec_errorno)) {
      do {
        err = waitpid(*pid, &status, 0); /* okay, read errorno */
      } while (err == -1 && errno == EINTR);
      assert(err == *pid);
      err = exec_errorno;
    } else if (r == -1 && errno == EPIPE) {
      /* Something unknown happened to our child before spawn */
      do {
        err = waitpid(*pid, &status, 0); /* okay, got EPIPE */
      } while (err == -1 && errno == EINTR);
      assert(err == *pid);
      err = UV_EPIPE;
    } else {
      abort();
    }
  }

  uv__close_nocheckstdio(signal_pipe[0]);

  return err;
}
#endif /* ISN'T TARGET_OS_TV || TARGET_OS_WATCH */

int uv_spawn(uv_loop_t* loop, uv_process_t* process, const uv_process_options_t* options) {
  int pipes_storage[8][2];
  int (*pipes)[2];
  int stdio_count;
  pid_t pid;
  int err;
  int exec_errorno;
  int i;

  assert(options->file != NULL);
  assert(!(options->flags & ~(UV_PROCESS_DETACHED |
                              UV_PROCESS_SETGID |
                              UV_PROCESS_SETUID |
                              UV_PROCESS_WINDOWS_FILE_PATH_EXACT_NAME |
                              UV_PROCESS_WINDOWS_HIDE |
                              UV_PROCESS_WINDOWS_HIDE_CONSOLE |
                              UV_PROCESS_WINDOWS_HIDE_GUI |
                              UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS)));

  uv__handle_init(loop, (uv_handle_t*)process, UV_PROCESS);
  uv__queue_init(&process->queue);
  process->status = 0;

  stdio_count = options->stdio_count;
  if (stdio_count < 3) {
    stdio_count = 3;
  }

  err = UV_ENOMEM;
  pipes = pipes_storage;
  if (stdio_count > (int) ARRAY_SIZE(pipes_storage)) {
    pipes = uv__malloc(stdio_count * sizeof(*pipes));
  }

  if (pipes == NULL) {
    goto error;
  }

  for (i = 0; i < stdio_count; i++) {
    pipes[i][0] = -1;
    pipes[i][1] = -1;
  }

  for (i = 0; i < options->stdio_count; i++) {
    err = uv__process_init_stdio(options->stdio + i, pipes[i]);
    if (err) {
      goto error;
    }
  }

#ifdef UV_USE_SIGCHLD
  uv_signal_start(&loop->child_watcher, uv__chld, SIGCHLD);
#endif

  /* Spawn the child */
  exec_errorno = uv__spawn_and_init_child(loop, options, stdio_count, pipes, &pid);

#if 0
  /* This runs into a nodejs issue (it expects initialized streams, even if the
   * exec failed).
   * See https://github.com/libuv/libuv/pull/3107#issuecomment-782482608 */
  if (exec_errorno != 0)
      goto error;
#endif

  /* Activate this handle if exec() happened successfully, even if we later
   * fail to open a stdio handle. This ensures we can eventually reap the child
   * with waitpid. */
  if (exec_errorno == 0) {
#ifndef UV_USE_SIGCHLD
    struct kevent event;
    EV_SET(&event, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, 0);
    if (kevent(loop->backend_fd, &event, 1, NULL, 0, NULL)) {
      if (errno != ESRCH)
        abort();
      /* Process already exited. Call waitpid on the next loop iteration. */
      process->flags |= UV_HANDLE_REAP;
      loop->flags |= UV_LOOP_REAP_CHILDREN;
    }
    /* This prevents uv__io_poll() from bailing out prematurely, being unaware
     * that we added an event here for it to react to. We will decrement this
     * again after the waitpid call succeeds. */
    loop->nfds++;
#endif

    process->pid = pid;
    process->exit_cb = options->exit_cb;
    uv__queue_insert_tail(&loop->process_handles, &process->queue);
    uv__handle_start(process);
  }

  for (i = 0; i < options->stdio_count; i++) {
    err = uv__process_open_stream(options->stdio + i, pipes[i]);
    if (err == 0) {
      continue;
    }

    while (i--) {
      uv__process_close_stream(options->stdio + i);
    }

    goto error;
  }

  if (pipes != pipes_storage) {
    uv__free(pipes);
  }

  return exec_errorno;

error:
  if (pipes != NULL) {
    for (i = 0; i < stdio_count; i++) {
      if (i < options->stdio_count) {
        if (options->stdio[i].flags & (UV_INHERIT_FD | UV_INHERIT_STREAM)) {
          continue;
        }
      }
      if (pipes[i][0] != -1) {
        uv__close_nocheckstdio(pipes[i][0]);
      }
      if (pipes[i][1] != -1) {
        uv__close_nocheckstdio(pipes[i][1]);
      }
    }

    if (pipes != pipes_storage) {
      uv__free(pipes);
    }
  }

  return err;
}


int uv_process_kill(uv_process_t* process, int signum) {
  return uv_kill(process->pid, signum);
}


int uv_kill(int pid, int signum) {
  if (kill(pid, signum)) {
    return UV__ERR(errno);
  } else {
    return 0;
  }
}


void uv__process_close(uv_process_t* handle) {
  uv__queue_remove(&handle->queue);
  uv__handle_stop(handle);
#ifdef UV_USE_SIGCHLD
  if (uv__queue_empty(&handle->loop->process_handles)) {
    uv_signal_stop(&handle->loop->child_watcher);
  }
#endif
}
