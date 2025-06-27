CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L -pthread -Wno-format-truncation
LDFLAGS = -pthread

# Directories
SERVER_DIR = server
CLIENT_DIR = client
COMMON_DIR = common

# Include directories
INCLUDES = -I$(COMMON_DIR) -I$(SERVER_DIR)

# Object files
SERVER_OBJS = $(SERVER_DIR)/chatserver.o
CLIENT_OBJS = $(CLIENT_DIR)/chatclient.o

# Executables
SERVER_TARGET = chatserver
CLIENT_TARGET = chatclient

# Default target
all: $(SERVER_TARGET) $(CLIENT_TARGET)

# Server executable
$(SERVER_TARGET): $(SERVER_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Client executable  
$(CLIENT_TARGET): $(CLIENT_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Server object files
$(SERVER_DIR)/chatserver.o: $(SERVER_DIR)/chatserver.c $(SERVER_DIR)/server_utils.h $(COMMON_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Client object files
$(CLIENT_DIR)/chatclient.o: $(CLIENT_DIR)/chatclient.c $(COMMON_DIR)/common.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean up
clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS) $(SERVER_TARGET) $(CLIENT_TARGET)
	rm -f *.log
	rm -rf uploads receivedFiles

.PHONY: all clean