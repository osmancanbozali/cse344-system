CC = gcc
CFLAGS = -Wall -Wextra
TARGET = main
SRC = main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)
	@echo "Compilation complete. After running, wait 15-20 seconds for the daemon to finish. Then check the results.log file for output."

clean:
	rm -f $(TARGET) firstfifo secondfifo results.log
	
.PHONY: all clean