CC = gcc

# Некоторые более старые версии gcc/clang ещё не знают официальное имя "c23"
# (стандарт утверждён в 2024) и требуют временное имя "c2x" - определяем
# автоматически, что поддерживает конкретный компилятор.
STD := $(shell echo "" | $(CC) -std=c23 -xc -E - >/dev/null 2>&1 && echo c23 || echo c2x)
CFLAGS = -Wall -Wextra -std=$(STD)

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

# сборка с диагностическими средствами (требование ТЗ): AddressSanitizer + UBSan + отладочная информация
debug: CFLAGS += -fsanitize=address,undefined -g -O0
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean $(TARGET)

clean:
	rm -rf $(OBJDIR) $(TARGET)

.PHONY: all debug clean
