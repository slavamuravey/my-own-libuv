project_dir := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
libuv_dir := $(project_dir)
src_dir := $(project_dir)/src
include_dir := $(project_dir)/include

CC = gcc
CFLAGS ?= -g -Wall -pedantic -I $(include_dir)
SRCMODULES ?= $(src_dir)/core.c $(src_dir)/loop.c $(src_dir)/uv-common.c 
OBJMODULES = $(SRCMODULES:.c=.o)

%.o: %.c %.h
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: run
run: clean main
	./main

main: main.c $(OBJMODULES)
	$(CC) $(CFLAGS) $^ -o $@

.PHONY: clean
clean:
	rm -f *.o main
