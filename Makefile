CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11

# На macOS (M3, Homebrew в /opt/homebrew) нужно явно указать пути к readline,
# т.к. системный readline.h в Xcode SDK - урезанная замена без части функций.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -I/opt/homebrew/opt/readline/include
    LDFLAGS += -L/opt/homebrew/opt/readline/lib
endif

LDLIBS = -lreadline

SRCS = main.c memory.c utils.c
OBJS = $(SRCS:.c=.o)
TARGET = mybash

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
