#ifndef UV_H
#define UV_H

#define __USE_UNIX98
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
  UV_UNKNOWN_HANDLE = 0,

  UV_ASYNC, 
  UV_CHECK, 
  UV_FS_EVENT, 
  UV_FS_POLL, 
  UV_HANDLE, 
  UV_IDLE, 
  UV_NAMED_PIPE, 
  UV_POLL, 
  UV_PREPARE, 
  UV_PROCESS, 
  UV_STREAM, 
  UV_TCP, 
  UV_TIMER, 
  UV_TTY, 
  UV_UDP, 
  UV_SIGNAL,

  UV_FILE,
  UV_HANDLE_TYPE_MAX
} uv_handle_type;

typedef pthread_mutex_t uv_mutex_t;
typedef pthread_rwlock_t uv_rwlock_t;

/* Internal type, do not use. */
struct uv__queue {
  struct uv__queue* next;
  struct uv__queue* prev;
};

struct uv__io_s;
struct uv_loop_s;

typedef void (*uv__io_cb)(struct uv_loop_s* loop,
                          struct uv__io_s* w,
                          unsigned int events);
typedef struct uv__io_s uv__io_t;

struct uv__io_s {
  uv__io_cb cb;
  struct uv__queue pending_queue;
  struct uv__queue watcher_queue;
  unsigned int pevents; /* Pending event mask i.e. mask at next tick. */
  unsigned int events;  /* Current event mask. */
  int fd;
};

/* Handle types. */
typedef struct uv_loop_s uv_loop_t;
typedef struct uv_async_s uv_async_t;
typedef struct uv_handle_s uv_handle_t;
typedef struct uv_signal_s uv_signal_t;

typedef enum {
  UV_RUN_DEFAULT = 0,
  UV_RUN_ONCE,
  UV_RUN_NOWAIT
} uv_run_mode;

uv_loop_t* uv_default_loop(void);
int uv_loop_init(uv_loop_t* loop);
int uv_loop_close(uv_loop_t* loop);

int uv_run(uv_loop_t*, uv_run_mode mode);
void uv_stop(uv_loop_t*);

typedef void (*uv_close_cb)(uv_handle_t* handle);
typedef void (*uv_async_cb)(uv_async_t* handle);
typedef void (*uv_signal_cb)(uv_signal_t* handle, int signum);

struct uv_async_s {
  /* handle fields start */
  void* data;
  uv_loop_t* loop;
  uv_handle_type type;
  uv_close_cb close_cb;
  struct uv__queue handle_queue;
  union {
    int fd;
    void* reserved[4];
  } u;
  uv_handle_t* next_closing;
  unsigned int flags;
  /* handle fields end */

  uv_async_cb async_cb;
  struct uv__queue queue;
  int pending;
};

/* The abstract base class of all handles. */
struct uv_handle_s {
  /* handle fields start */
  void* data;
  uv_loop_t* loop;
  uv_handle_type type;
  uv_close_cb close_cb;
  struct uv__queue handle_queue;
  union {
    int fd;
    void* reserved[4];
  } u;
  uv_handle_t* next_closing;
  unsigned int flags;
  /* handle fields end */
};

struct uv_signal_s {
  /* handle fields start */
  void* data;
  uv_loop_t* loop;
  uv_handle_type type;
  uv_close_cb close_cb;
  struct uv__queue handle_queue;
  union {
    int fd;
    void* reserved[4];
  } u;
  uv_handle_t* next_closing;
  unsigned int flags;
  /* handle fields end */
  
  uv_signal_cb signal_cb;
  int signum;
  
  /* RB_ENTRY(uv_signal_s) tree_entry; */                                     
  struct {
    struct uv_signal_s* rbe_left;
    struct uv_signal_s* rbe_right;
    struct uv_signal_s* rbe_parent;
    int rbe_color;
  } tree_entry;
  /* Use two counters here so we don have to fiddle with atomics. */          
  unsigned int caught_signals;
  unsigned int dispatched_signals;
};

struct uv_loop_s {
  /* User data - use this for whatever. */
  void* data;
  /* Loop reference counting. */
  unsigned int active_handles;
  struct uv__queue handle_queue;
  union {
    void* unused;
    unsigned int count;
  } active_reqs;
  /* Internal storage for future extensions. */
  void* internal_fields;
  /* Internal flag to signal loop stop. */
  unsigned int stop_flag;
  unsigned long flags;
  int backend_fd;
  struct uv__queue pending_queue;
  struct uv__queue watcher_queue;
  uv__io_t** watchers;
  unsigned int nwatchers;
  unsigned int nfds;
  struct uv__queue wq;
  uv_mutex_t wq_mutex;
  uv_async_t wq_async;
  uv_rwlock_t cloexec_lock;
  uv_handle_t* closing_handles;
  struct uv__queue process_handles;
  struct uv__queue prepare_handles;
  struct uv__queue check_handles;
  struct uv__queue idle_handles;
  struct uv__queue async_handles;
  void (*async_unused)(void); 
  uv__io_t async_io_watcher;
  int async_wfd;
  struct {
    void* min;
    unsigned int nelts;
  } timer_heap;
  uint64_t timer_counter;
  uint64_t time;
  int signal_pipefd[2]; 
  uv__io_t signal_io_watcher;
  uv_signal_t child_watcher;
  int emfile_fd;
  uv__io_t inotify_read_watcher;
  void* inotify_watchers;
  int inotify_fd;
};

#endif