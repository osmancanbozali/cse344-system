CC = gcc
CFLAGS = -Wall -pthread -o
TARGET = hw3
SRCS = main.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)