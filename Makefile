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

OBJDIR = obj

SRCS = main.c memory.c utils.c token.c lexer.c node.c parser.c jobs.c builtins.c exec.c
OBJS = $(addprefix $(OBJDIR)/, $(SRCS:.c=.o))
TARGET = mybash

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(OBJS) $(LDLIBS)

# все .o собираются в папку obj/ (она создаётся автоматически, если её нет)
$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET)

.PHONY: all clean
