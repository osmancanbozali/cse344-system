CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -lpthread

# Target executable
TARGET = LogAnalyzer

# Source files
SRCS = 220104004011_main.c buffer.c

# Object files
OBJS = $(SRCS:.c=.o)

# Default target
all: $(TARGET)

# Rule to build the executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Rule to build object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean target
clean:
	rm -f $(TARGET) $(OBJS)

# Phony targets
.PHONY: all clean