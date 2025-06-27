#ifndef COMMON_H
#define COMMON_H
#include <semaphore.h>

#define MAX_MSG_LEN 1024
#define DEFAULT_PORT 5000
#define MAX_USERNAME_LEN 16
#define MAX_CLIENTS_GLOBAL 50
#define LOG_FILE_NAME "server.log"

// Room management constants
#define MAX_ROOM_NAME_LEN 32
#define MAX_ROOM_USERS 15
#define MAX_ROOMS 20

// File transfer constants
#define MAX_FILENAME_LEN 256
#define MAX_FILE_SIZE (3 * 1024 * 1024) // 3MB limit
#define MAX_UPLOAD_QUEUE 5 // Maximum concurrent uploads
#define UPLOAD_BUFFER_SIZE 4096
#define UPLOADS_DIR "uploads"

// File transfer message type constants
#define FILE_REQUEST_MSG "FILE_REQUEST:"
#define FILE_RESPONSE_OK "FILE_OK:"
#define FILE_RESPONSE_ERROR "FILE_ERROR:"
#define FILE_NOTIFICATION "FILE_NOTIFY:"

// Server response constants
#define SERVER_RESPONSE_OK "OK:"
#define SERVER_RESPONSE_ERROR "ERROR:"

// Color codes
#define KBLU "\x1B[34m"
#define KMAG "\x1B[35m"
#define KCYN "\x1B[36m"
#define KWHT "\x1B[37m"
#define KNRM "\x1B[0m"
#define KRED "\x1B[31m"
#define KGRN "\x1B[32m"
#define KYEL "\x1B[33m"

#endif // COMMON_H