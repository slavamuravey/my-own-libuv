cmake_minimum_required(VERSION 3.10)
project(my-own-libuv)
include_directories("include/")
add_compile_definitions(_GNU_SOURCE)

set(
    UV_SOURCES
    
    src/async.c        src/inet.c            src/proctitle.c            src/strscpy.c        src/udp.c
    src/core.c         src/linux.c           src/random.c               src/strtok.c         src/uv-common.c
    src/dl.c           src/loop.c            src/random-devurandom.c    src/tcp.c            src/uv-data-getter-setters.c
    src/fs.c           src/loop-watcher.c    src/random-getentropy.c    src/thread.c         src/version.c
    src/fs-poll.c      src/pipe.c            src/random-getrandom.c     src/thread-common.c
    src/getaddrinfo.c  src/poll.c            src/random-sysctl-linux.c  src/threadpool.c
    src/getnameinfo.c  src/process.c         src/signal.c               src/timer.c
    src/idna.c         src/procfs-exepath.c  src/stream.c               src/tty.c
)

add_executable(
    helloworld 
    
    ${UV_SOURCES}
    
    examples/helloworld/main.c
)

add_executable(
    ref-timer 
    
    ${UV_SOURCES}
    
    examples/ref-timer/main.c
)