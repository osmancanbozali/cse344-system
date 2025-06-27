#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H

#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include "common.h"

// Client struct
typedef struct {
    int sockfd;
    char username[MAX_USERNAME_LEN + 1];
    char ip_addr[INET_ADDRSTRLEN];
    pthread_t thread_id;
    int active;
    char current_room_name[MAX_ROOM_NAME_LEN + 1];
} client_t;

// Room struct
typedef struct {
    char name[MAX_ROOM_NAME_LEN + 1];
    int member_client_indices[MAX_ROOM_USERS];
    int num_users;
    pthread_mutex_t room_mutex;
    int active;
} room_t;

// File transfer status enum
typedef enum {
    TRANSFER_PENDING,
    TRANSFER_PROCESSING,
    TRANSFER_COMPLETED,
    TRANSFER_FAILED
} transfer_status_t;

// File transfer struct
typedef struct {
    char filename[MAX_FILENAME_LEN];
    char sender_username[MAX_USERNAME_LEN + 1];
    char receiver_username[MAX_USERNAME_LEN + 1];
    char server_filepath[512];
    int sender_idx;
    int receiver_idx;
    size_t file_size;
    time_t request_time;
    time_t start_time;
    time_t completion_time;
    transfer_status_t status;
    int processor_thread_id;
    char error_message[256];
} file_transfer_t;

typedef struct {
    file_transfer_t transfers[MAX_UPLOAD_QUEUE * 3]; // array for queued transfers
    int front;
    int rear;
    int count;
    int total_processed;
    int total_failed;
    pthread_mutex_t queue_mutex;
    sem_t queue_semaphore;
    pthread_cond_t queue_cond;
    pthread_cond_t queue_not_full;
} file_queue_t;

// Global variables
extern client_t clients[MAX_CLIENTS_GLOBAL];
extern pthread_mutex_t clients_mutex;

extern room_t rooms[MAX_ROOMS];
extern pthread_mutex_t rooms_list_mutex;

extern FILE *log_file;
extern pthread_mutex_t log_mutex;

// File transfer global variables
extern file_queue_t upload_queue;
extern pthread_t file_processor_threads[MAX_UPLOAD_QUEUE];
extern volatile sig_atomic_t server_running;

// Logging functions
void init_logging();
void log_message(const char *format, ...);
void close_logging();

// Room management functions
void initialize_rooms();
int find_room_idx_by_name(const char* room_name);
int is_room_name_valid(const char* room_name);

// File transfer functions
void init_file_transfer_system();
void cleanup_file_transfer_system();
int is_valid_file_type(const char* filename);
int validate_file_metadata(const char* filename, size_t file_size);
void handle_sendfile_command(int sender_idx, const char* args);
void handle_file_metadata(int sender_idx, const char* metadata);
void* file_processor_thread(void* arg);
int enqueue_file_transfer(file_transfer_t* transfer);
void notify_transfer_completion(file_transfer_t* transfer);
void create_uploads_directory();
char* generate_unique_filepath(const char* receiver_username, const char* filename);

// File transfer protocol functions
int receive_file_from_client(int client_idx, file_transfer_t* transfer);
int send_file_to_client(int client_idx, file_transfer_t* transfer);

// Utility functions for file transfer
void get_file_transfer_stats(int* active, int* queued, int* total_processed, int* total_failed);
void parse_sendfile_args(const char* args, char* filename, char* username, size_t* file_size);

// Enhanced queue management functions for wait duration tracking
void notify_queue_status(int sender_idx, const char* filename, int queue_position, double estimated_wait_time);
double calculate_estimated_wait_time(int queue_position);
void log_file_wait_duration(file_transfer_t* transfer);

#endif // SERVER_UTILS_H