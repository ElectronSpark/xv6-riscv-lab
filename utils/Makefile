CC = gcc
CFLAGS = -Wall -Wextra -O2
FUSE_CFLAGS = $(shell pkg-config --cflags fuse3)
FUSE_LIBS = $(shell pkg-config --libs fuse3)

SRCS = xv6fuse.c rbtree.c
OBJS = $(SRCS:.c=.o)
TARGET = xv6fuse

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(FUSE_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -c -o $@ $<

xv6fuse.o: xv6fuse.c rbtree.h
rbtree.o: rbtree.c rbtree.h

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
