CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -I./mongoose-src
LDFLAGS ?=
LDLIBS ?= -lpthread

MONGOOSE_SRCS := $(wildcard mongoose-src/*.c)

TARGET ?= sys_monitor_daemon

SRCS := $(wildcard *.c) $(MONGOOSE_SRCS)
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
