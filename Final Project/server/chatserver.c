#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include "common.h"
#include "server_utils.h"

// Global variables
client_t clients[MAX_CLIENTS_GLOBAL];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

room_t rooms[MAX_ROOMS];
pthread_mutex_t rooms_list_mutex = PTHREAD_MUTEX_INITIALIZER;

FILE *log_file = NULL;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

volatile sig_atomic_t server_running = 1;

file_queue_t upload_queue;
pthread_t file_processor_threads[MAX_UPLOAD_QUEUE];

// FOrward declarations
void initialize_clients();
int find_free_client_slot();
void initialize_rooms();
void handle_join_room(int client_idx, const char* room_name_arg);
void handle_leave_room(int client_idx, int notify_client);
void handle_broadcast_message(int client_idx, const char* message);
void handle_whisper_message(int sender_idx, const char* target_username, const char* message);

// Function to get current timestamp in a formatted string
void get_timestamp(char *buffer, size_t len) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, len, "%Y-%m-%d %H:%M:%S", t);
}

void init_logging() {
    pthread_mutex_lock(&log_mutex);
    log_file = fopen(LOG_FILE_NAME, "a");
    if (log_file == NULL) {
        perror("Failed to open log file");
    }
    pthread_mutex_unlock(&log_mutex);
    if (log_file) {
        char timestamp[20];
        get_timestamp(timestamp, sizeof(timestamp));
        fprintf(log_file, "%s - [INFO] Server logging initialized.\n", timestamp);
        fflush(log_file);
    } else {
        printf("[INFO] Could not open log file %s. Console logging only.\n", LOG_FILE_NAME);
    }
}

// Log message with timestamp
void log_message(const char *format, ...) {
    char log_buffer[MAX_MSG_LEN + 200];
    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);

    char timestamp[20];
    get_timestamp(timestamp, sizeof(timestamp));

    printf("%s\n", log_buffer);
    fflush(stdout);

    // Write to log file with timestamp
    if (log_file != NULL) {
        pthread_mutex_lock(&log_mutex);
        fprintf(log_file, "%s - %s\n", timestamp, log_buffer);
        fflush(log_file);
        pthread_mutex_unlock(&log_mutex);
    }
}

void close_logging() {
    if (log_file) {
        log_message("[INFO] Server logging shutting down");
    }
    pthread_mutex_lock(&log_mutex);
    if (log_file != NULL) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}

// signal handler
void sigint_handler(int signum) {
    if (signum == SIGINT) {
        char msg[] = "\n[SHUTDOWN] SIGINT received. Shutting down server gracefully...\n";
        write(STDOUT_FILENO, msg, sizeof(msg) -1);
        server_running = 0;
    }
}

void error_exit(const char *msg) {
    log_message("[ERROR] %s: %s", msg, strerror(errno));
    perror(msg);
    close_logging();
    exit(EXIT_FAILURE);
}


// Create uploads directory
void create_uploads_directory() {
    struct stat st = {0};
    if (stat(UPLOADS_DIR, &st) == -1) {
        if (mkdir(UPLOADS_DIR, 0755) == 0) {
            log_message("[INFO] Created uploads directory: %s", UPLOADS_DIR);
        } else {
            log_message("[ERROR] Failed to create uploads directory: %s", strerror(errno));
        }
    }
}

// Initialize file transfer system
void init_file_transfer_system() {
    // Initialize queue structure
    upload_queue.front = 0;
    upload_queue.rear = 0;
    upload_queue.count = 0;
    upload_queue.total_processed = 0;
    upload_queue.total_failed = 0;
    
    if (pthread_mutex_init(&upload_queue.queue_mutex, NULL) != 0) {
        log_message("[ERROR] Failed to initialize file queue mutex: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    if (sem_init(&upload_queue.queue_semaphore, 0, MAX_UPLOAD_QUEUE) != 0) {
        log_message("[ERROR] Failed to initialize file queue semaphore: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    if (pthread_cond_init(&upload_queue.queue_cond, NULL) != 0) {
        log_message("[ERROR] Failed to initialize file queue condition variable: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    if (pthread_cond_init(&upload_queue.queue_not_full, NULL) != 0) {
        log_message("[ERROR] Failed to initialize queue not full condition: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    create_uploads_directory();
    
    // Create file processor threads
    for (int i = 0; i < MAX_UPLOAD_QUEUE; i++) {
        if (pthread_create(&file_processor_threads[i], NULL, file_processor_thread, (void*)(intptr_t)i) != 0) {
            log_message("[ERROR] Failed to create file processor thread %d: %s", i, strerror(errno));
            exit(EXIT_FAILURE);
        }
        pthread_detach(file_processor_threads[i]);
    }
    
    log_message("[INFO] File transfer system initialized with %d processor threads", MAX_UPLOAD_QUEUE);
}

// Check file extension
int is_valid_file_type(const char* filename) {
    if (filename == NULL || strlen(filename) == 0) return 0;
    
    const char* allowed_extensions[] = {".txt", ".pdf", ".jpg", ".png"};
    int num_extensions = sizeof(allowed_extensions) / sizeof(allowed_extensions[0]);
    
    const char* dot = strrchr(filename, '.');
    if (dot == NULL) return 0;
    
    for (int i = 0; i < num_extensions; i++) {
        if (strcasecmp(dot, allowed_extensions[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

// Validate file metadata
int validate_file_metadata(const char* filename, size_t file_size) {
    if (!is_valid_file_type(filename)) {
        return 0; // Invalid file type
    }
    
    if (file_size > MAX_FILE_SIZE) {
        return 0; // File over size limit
    }
    
    if (file_size == 0) {
        return 0; // Empty file
    }
    
    return 1; // Valid
}

// Generate unique file path
char* generate_unique_filepath(const char* receiver_username, const char* filename) {
    static char filepath[2048];  // Significantly increased buffer size
    snprintf(filepath, sizeof(filepath), "%s/%s_%s", UPLOADS_DIR, receiver_username, filename);
    
    // Check whether file exists and generate unique name if needed
    struct stat st;
    if (stat(filepath, &st) == 0) {
        char base_path[2048];
        snprintf(base_path, sizeof(base_path), "%s/%s_%s", UPLOADS_DIR, receiver_username, filename);
        
        char* dot = strrchr(base_path, '.');
        char extension[256] = "";
        char base_without_ext[1792];
        
        if (dot != NULL) {
            strncpy(extension, dot, sizeof(extension) - 1);
            extension[sizeof(extension) - 1] = '\0';
            
            // Create base path without extension
            size_t base_len = dot - base_path;
            if (base_len >= sizeof(base_without_ext)) {
                base_len = sizeof(base_without_ext) - 1;
            }
            strncpy(base_without_ext, base_path, base_len);
            base_without_ext[base_len] = '\0';
        } else {
            strncpy(base_without_ext, base_path, sizeof(base_without_ext) - 1);
            base_without_ext[sizeof(base_without_ext) - 1] = '\0';
        }
        
        int counter = 1;
        do {
            if (strlen(extension) > 0) {
                int needed_size = snprintf(NULL, 0, "%s_%d%s", base_without_ext, counter, extension);
                if (needed_size < (int)sizeof(filepath)) {
                    snprintf(filepath, sizeof(filepath), "%s_%d%s", base_without_ext, counter, extension);
                } else {
                    snprintf(filepath, sizeof(filepath), "%s/%s_%d", UPLOADS_DIR, receiver_username, counter);
                }
            } else {
                snprintf(filepath, sizeof(filepath), "%s_%d", base_without_ext, counter);
            }
            counter++;
        } while (stat(filepath, &st) == 0 && counter < 1000);
        
        log_message("[FILE] Conflict: '%s' received more than once → renamed '%s_%d%s'", filename, filename, counter-1, extension);
    }
    
    return filepath;
}

// Parse sendfile command arguments
void parse_sendfile_args(const char* args, char* filename, char* username, size_t* file_size) {
    filename[0] = '\0';
    username[0] = '\0';
    *file_size = 0;
    
    if (args == NULL) return;
    while (*args == ' ') args++;
    
    const char* space1 = strchr(args, ' ');
    if (space1 == NULL) return; // Invalid format
    
    size_t filename_len = space1 - args;
    if (filename_len >= MAX_FILENAME_LEN) filename_len = MAX_FILENAME_LEN - 1;
    strncpy(filename, args, filename_len);
    filename[filename_len] = '\0';
    
    space1++;
    while (*space1 == ' ') space1++;
    
    const char* space2 = strchr(space1, ' ');
    if (space2 == NULL) {
        strncpy(username, space1, MAX_USERNAME_LEN);
        username[MAX_USERNAME_LEN] = '\0';
        
        int len = strlen(username);
        while (len > 0 && username[len-1] == ' ') {
            username[--len] = '\0';
        }
        return;
    }
    
    size_t username_len = space2 - space1;
    if (username_len > MAX_USERNAME_LEN) username_len = MAX_USERNAME_LEN;
    strncpy(username, space1, username_len);
    username[username_len] = '\0';
    
    space2++;
    while (*space2 == ' ') space2++;
    
    *file_size = (size_t)strtoul(space2, NULL, 10);
}

// Add file transfer to queue
int enqueue_file_transfer(file_transfer_t* transfer) {
    pthread_mutex_lock(&upload_queue.queue_mutex);
    
    // Wait if queue is full
    while (upload_queue.count >= (MAX_UPLOAD_QUEUE * 3) && server_running) {
        pthread_cond_wait(&upload_queue.queue_not_full, &upload_queue.queue_mutex);
    }
    
    if (!server_running) {
        pthread_mutex_unlock(&upload_queue.queue_mutex);
        return 0;
    }
    
    // Add transfer to queue
    upload_queue.transfers[upload_queue.rear] = *transfer;
    upload_queue.rear = (upload_queue.rear + 1) % (MAX_UPLOAD_QUEUE * 3);
    upload_queue.count++;
    
    // Signal waiting processor threads
    pthread_cond_signal(&upload_queue.queue_cond);
    pthread_mutex_unlock(&upload_queue.queue_mutex);
    
    return 1;
}

// Get file transfer statistics
void get_file_transfer_stats(int* active, int* queued, int* total_processed, int* total_failed) {
    pthread_mutex_lock(&upload_queue.queue_mutex);
    *queued = upload_queue.count;
    *total_processed = upload_queue.total_processed;
    *total_failed = upload_queue.total_failed;
    pthread_mutex_unlock(&upload_queue.queue_mutex);
    
    int sem_value;
    sem_getvalue(&upload_queue.queue_semaphore, &sem_value);
    *active = MAX_UPLOAD_QUEUE - sem_value;
}

// Calculate estimated wait time based on queue position and average processing time
double calculate_estimated_wait_time(int queue_position) {
    double avg_processing_time = 3.5;
    
    int sem_value;
    sem_getvalue(&upload_queue.queue_semaphore, &sem_value);
    int available_slots = sem_value;
    
    if (available_slots > 0) {
        return 0.0;
    }
    
    double estimated_wait = ((double)queue_position / (double)MAX_UPLOAD_QUEUE) * avg_processing_time;
    
    return estimated_wait;
}

// function to notify queue status
void notify_queue_status(int sender_idx, const char* filename, int queue_position, double estimated_wait_time) {
    char status_msg[MAX_MSG_LEN];
    char wait_msg[MAX_MSG_LEN / 2];
    
    if (estimated_wait_time <= 0.1) {
        snprintf(wait_msg, sizeof(wait_msg), "Processing will begin immediately.");
    } else if (estimated_wait_time < 60.0) {
        snprintf(wait_msg, sizeof(wait_msg), "Estimated wait time: %.1f seconds.", estimated_wait_time);
    } else {
        snprintf(wait_msg, sizeof(wait_msg), "Estimated wait time: %.1f minutes.", estimated_wait_time / 60.0);
    }
    
    snprintf(status_msg, sizeof(status_msg), 
             "%sFile '%s' queued for transfer (position %d in queue). %s", 
             SERVER_RESPONSE_OK, filename, queue_position, wait_msg);
    
    pthread_mutex_lock(&clients_mutex);
    if (sender_idx >= 0 && sender_idx < MAX_CLIENTS_GLOBAL && clients[sender_idx].active) {
        send(clients[sender_idx].sockfd, status_msg, strlen(status_msg), 0);
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Log file wait duration
void log_file_wait_duration(file_transfer_t* transfer) {
    double wait_duration = difftime(transfer->start_time, transfer->request_time);
    
    log_message("[FILE] '%s' from user '%s' started upload after %.0f seconds in queue", transfer->filename, transfer->sender_username, wait_duration);
    
    char wait_notification[MAX_MSG_LEN];
    if (wait_duration < 1.0) {
        snprintf(wait_notification, sizeof(wait_notification), 
                "%sFile '%s' processing started immediately.", 
                SERVER_RESPONSE_OK, transfer->filename);
    } else {
        snprintf(wait_notification, sizeof(wait_notification), 
                "%sFile '%s' processing started after %.0f seconds in queue.", 
                SERVER_RESPONSE_OK, transfer->filename, wait_duration);
    }
    
    pthread_mutex_lock(&clients_mutex);
    if (transfer->sender_idx >= 0 && transfer->sender_idx < MAX_CLIENTS_GLOBAL && 
        clients[transfer->sender_idx].active) {
        send(clients[transfer->sender_idx].sockfd, wait_notification, strlen(wait_notification), 0);
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Notify completion of file transfer
void notify_transfer_completion(file_transfer_t* transfer) {
    char sender_msg[MAX_MSG_LEN];
    char receiver_msg[MAX_MSG_LEN];
    
    if (transfer->status == TRANSFER_COMPLETED) {
        // Notify sender
        snprintf(sender_msg, sizeof(sender_msg), 
                "%sFile '%s' sent successfully to '%s' (processed in %.1f seconds).", 
                SERVER_RESPONSE_OK, transfer->filename, transfer->receiver_username,
                difftime(transfer->completion_time, transfer->start_time));
        
        // Notify receiver
        snprintf(receiver_msg, sizeof(receiver_msg), 
                "%sYou received file '%s' from '%s' (size: %zu bytes)", 
                FILE_NOTIFICATION, transfer->filename, transfer->sender_username, transfer->file_size);
    } else {
        // Notify sender of failure
        snprintf(sender_msg, sizeof(sender_msg), 
                "%sFile transfer failed: %s", 
                SERVER_RESPONSE_ERROR, transfer->error_message);
        
        receiver_msg[0] = '\0';
    }
    
    // Send notifications
    pthread_mutex_lock(&clients_mutex);
    if (transfer->sender_idx >= 0 && transfer->sender_idx < MAX_CLIENTS_GLOBAL && 
        clients[transfer->sender_idx].active) {
        send(clients[transfer->sender_idx].sockfd, sender_msg, strlen(sender_msg), 0);
    }
    
    if (receiver_msg[0] != '\0' && transfer->receiver_idx >= 0 && 
        transfer->receiver_idx < MAX_CLIENTS_GLOBAL && clients[transfer->receiver_idx].active) {
        send(clients[transfer->receiver_idx].sockfd, receiver_msg, strlen(receiver_msg), 0);
    }
    pthread_mutex_unlock(&clients_mutex);
}

// File processor thread function
void* file_processor_thread(void* arg) {
    int thread_id = (int)(intptr_t)arg;
    
    while (server_running) {
        // Wait for semaphore 
        if (sem_wait(&upload_queue.queue_semaphore) != 0) {
            if (errno == EINTR && !server_running) break;
            continue;
        }
        
        if (!server_running) break;
        
        file_transfer_t transfer;
        int has_transfer = 0;
        
        // Dequeue a file transfer
        pthread_mutex_lock(&upload_queue.queue_mutex);
        while (upload_queue.count == 0 && server_running) {
            pthread_cond_wait(&upload_queue.queue_cond, &upload_queue.queue_mutex);
        }
        
        if (upload_queue.count > 0 && server_running) {
            transfer = upload_queue.transfers[upload_queue.front];
            upload_queue.front = (upload_queue.front + 1) % (MAX_UPLOAD_QUEUE * 3);
            upload_queue.count--;
            has_transfer = 1;
            
            pthread_cond_signal(&upload_queue.queue_not_full);
        }
        pthread_mutex_unlock(&upload_queue.queue_mutex);
        
        if (!has_transfer || !server_running) {
            sem_post(&upload_queue.queue_semaphore);
            continue;
        }
        
        // Process the file transfer
        transfer.status = TRANSFER_PROCESSING;
        transfer.start_time = time(NULL);
        transfer.processor_thread_id = thread_id;
        
        log_file_wait_duration(&transfer);
        
        // Simulate file processing time
        int processing_time = 1 + (transfer.file_size / (512 * 1024));
        if (processing_time > 8) processing_time = 8; // Cap at 8 seconds
        
        sleep(processing_time);
        
        if (!server_running) {
            transfer.status = TRANSFER_FAILED;
            snprintf(transfer.error_message, sizeof(transfer.error_message), "Server shutdown during transfer");
        } else {
            transfer.status = TRANSFER_COMPLETED;
            
            char* filepath = generate_unique_filepath(transfer.receiver_username, transfer.filename);
            
            FILE* simulated_file = fopen(filepath, "w");
            if (simulated_file) {
                // Write file metadata and simulation content
                time_t current_time = time(NULL);
                char* time_str = ctime(&current_time);
                time_str[strlen(time_str) - 1] = '\0';
                
                fprintf(simulated_file, "=== SIMULATED FILE TRANSFER ===\n");
                fprintf(simulated_file, "Original Filename: %s\n", transfer.filename);
                fprintf(simulated_file, "Sender: %s\n", transfer.sender_username);
                fprintf(simulated_file, "Receiver: %s\n", transfer.receiver_username);
                fprintf(simulated_file, "File Size: %zu bytes (%.2f KB)\n", transfer.file_size, (float)transfer.file_size / 1024.0);
                fprintf(simulated_file, "Transfer Started: %s", ctime(&transfer.start_time));
                fprintf(simulated_file, "Transfer Completed: %s\n", time_str);
                fprintf(simulated_file, "Processing Time: %.1f seconds\n", difftime(current_time, transfer.start_time));
                fprintf(simulated_file, "==============================\n\n");
                
                sleep(1); // Bunu degistirerek queue kismini test ediyorum.
                
                fclose(simulated_file);
                
                strncpy(transfer.server_filepath, filepath, sizeof(transfer.server_filepath) - 1);
                transfer.server_filepath[sizeof(transfer.server_filepath) - 1] = '\0';
            } else {
                transfer.status = TRANSFER_FAILED;
                snprintf(transfer.error_message, sizeof(transfer.error_message), "Failed to create file: %s", strerror(errno));
            }
        }
        
        transfer.completion_time = time(NULL);
        
        // Update statistics
        pthread_mutex_lock(&upload_queue.queue_mutex);
        if (transfer.status == TRANSFER_COMPLETED) {
            upload_queue.total_processed++;
        } else {
            upload_queue.total_failed++;
        }
        pthread_mutex_unlock(&upload_queue.queue_mutex);
        
        // Notify completion
        notify_transfer_completion(&transfer);
        
        sem_post(&upload_queue.queue_semaphore);
    }
    
    return NULL;
}

// Handle sendfile command
void handle_sendfile_command(int sender_idx, const char* args) {
    client_t *sender = &clients[sender_idx];
    char response_msg[MAX_MSG_LEN];
    char filename[MAX_FILENAME_LEN];
    char target_username[MAX_USERNAME_LEN + 1];
    size_t file_size;
    
    parse_sendfile_args(args, filename, target_username, &file_size);
    
    if (strlen(filename) == 0 || strlen(target_username) == 0) {
        snprintf(response_msg, sizeof(response_msg), "%sUsage: /sendfile <filename> <username>", SERVER_RESPONSE_ERROR);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }
    
    if (file_size == 0) {
        snprintf(response_msg, sizeof(response_msg), "%sFile size information required. Please use updated client.", SERVER_RESPONSE_ERROR);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }
    
    // Find target user
    int target_idx = -1;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS_GLOBAL; i++) {
        if (clients[i].active && strcmp(clients[i].username, target_username) == 0) {
            target_idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    if (target_idx == -1) {
        snprintf(response_msg, sizeof(response_msg), "%sUser '%s' not found or offline.", SERVER_RESPONSE_ERROR, target_username);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }
    
    if (target_idx == sender_idx) {
        snprintf(response_msg, sizeof(response_msg), "%sCannot send file to yourself.", SERVER_RESPONSE_ERROR);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }
    
    
    // Check file type first
    if (!is_valid_file_type(filename)) {
        snprintf(response_msg, sizeof(response_msg), "%sFile type not allowed. Supported: .txt, .pdf, .jpg, .png", SERVER_RESPONSE_ERROR);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }
    
    // Check file size
    if (file_size > MAX_FILE_SIZE) {
        snprintf(response_msg, sizeof(response_msg), "%sFile too large. Maximum size: %.1f MB", SERVER_RESPONSE_ERROR, (float)MAX_FILE_SIZE / (1024*1024));
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        
        log_message("[ERROR] File '%s' from user '%s' exceeds size limit", filename, sender->username);
        return;
    }
    
    // Check for empty file
    if (file_size == 0) {
        snprintf(response_msg, sizeof(response_msg), "%sEmpty files are not allowed.", SERVER_RESPONSE_ERROR);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }
    
    // create file transfer request
    file_transfer_t transfer;
    memset(&transfer, 0, sizeof(transfer));
    
    strncpy(transfer.filename, filename, MAX_FILENAME_LEN - 1);
    transfer.filename[MAX_FILENAME_LEN - 1] = '\0';
    strncpy(transfer.sender_username, sender->username, MAX_USERNAME_LEN);
    transfer.sender_username[MAX_USERNAME_LEN] = '\0';
    strncpy(transfer.receiver_username, target_username, MAX_USERNAME_LEN);
    transfer.receiver_username[MAX_USERNAME_LEN] = '\0';
    
    transfer.sender_idx = sender_idx;
    transfer.receiver_idx = target_idx;
    transfer.file_size = file_size;
    transfer.request_time = time(NULL);
    transfer.status = TRANSFER_PENDING;
    transfer.processor_thread_id = -1;
    
    // Add to queue
    if (enqueue_file_transfer(&transfer)) {
        int active, queued, total_processed, total_failed;
        get_file_transfer_stats(&active, &queued, &total_processed, &total_failed);
        
        // Calculate estimated wait time and notify client
        double estimated_wait_time = calculate_estimated_wait_time(queued);
        notify_queue_status(sender_idx, filename, queued, estimated_wait_time);
        
        log_message("[FILE-QUEUE] Upload '%s' from %s added to queue. Queue size: %d",
                    filename, sender->username, queued);
    } else {
        snprintf(response_msg, sizeof(response_msg), "%sFile transfer queue is full or server shutting down. Try again later.", SERVER_RESPONSE_ERROR);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
    }
}

// Cleanup file transfer system
void cleanup_file_transfer_system() {
    log_message("[INFO] Cleaning up file transfer system");
    
    // Wake up all waiting threads
    for (int i = 0; i < MAX_UPLOAD_QUEUE; i++) {
        sem_post(&upload_queue.queue_semaphore);
    }
    pthread_cond_broadcast(&upload_queue.queue_cond);
    pthread_cond_broadcast(&upload_queue.queue_not_full);
    
    sleep(2);
    
    // Clean up synchronization objects
    pthread_mutex_destroy(&upload_queue.queue_mutex);
    sem_destroy(&upload_queue.queue_semaphore);
    pthread_cond_destroy(&upload_queue.queue_cond);
    pthread_cond_destroy(&upload_queue.queue_not_full);
}


void initialize_clients() {
    for (int i = 0; i < MAX_CLIENTS_GLOBAL; i++) {
        clients[i].sockfd = -1;
        clients[i].active = 0;
        memset(clients[i].username, 0, sizeof(clients[i].username));
        memset(clients[i].ip_addr, 0, sizeof(clients[i].ip_addr));
        memset(clients[i].current_room_name, 0, sizeof(clients[i].current_room_name));
    }
}

int find_free_client_slot() {
    for (int i = 0; i < MAX_CLIENTS_GLOBAL; i++) {
        if (clients[i].active == 0) {
            return i;
        }
    }
    return -1;
}

int is_username_valid_and_unique(const char *username) {
    if (username == NULL || strlen(username) == 0 || strlen(username) > MAX_USERNAME_LEN) {
        return 0;
    }
    for (int i = 0; username[i] != '\0'; i++) {
        if (!isalnum((unsigned char)username[i])) {
            return 0;
        }
    }
    for (int i = 0; i < MAX_CLIENTS_GLOBAL; i++) {
        if (clients[i].active && clients[i].username[0] != '\0' && strcmp(clients[i].username, username) == 0) {
            return 0;
        }
    }
    return 1;
}

void initialize_rooms() {
    pthread_mutex_lock(&rooms_list_mutex);
    for (int i = 0; i < MAX_ROOMS; i++) {
        rooms[i].active = 0;
        rooms[i].num_users = 0;
        memset(rooms[i].name, 0, sizeof(rooms[i].name));
        for(int j=0; j < MAX_ROOM_USERS; ++j) {
            rooms[i].member_client_indices[j] = -1;
        }
        if (pthread_mutex_init(&rooms[i].room_mutex, NULL) != 0) {
            log_message("[ERROR] Failed to initialize mutex for room %d: %s", i, strerror(errno));
        }
    }
    pthread_mutex_unlock(&rooms_list_mutex);
}

int find_room_idx_by_name(const char* room_name) {
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].active && strcmp(rooms[i].name, room_name) == 0) {
            return i;
        }
    }
    return -1;
}

int is_room_name_valid(const char* room_name) {
    if (room_name == NULL || strlen(room_name) == 0 || strlen(room_name) > MAX_ROOM_NAME_LEN) {
        return 0;
    }
    for (int i = 0; room_name[i] != '\0'; i++) {
        if (!isalnum((unsigned char)room_name[i])) {
            return 0;
        }
    }
    return 1;
}

void cleanup_client(int client_idx) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS_GLOBAL) return;

    int needs_room_leave = 0;
    pthread_mutex_lock(&clients_mutex);
    if (clients[client_idx].active && clients[client_idx].current_room_name[0] != '\0') {
        needs_room_leave = 1;
    }
    pthread_mutex_unlock(&clients_mutex);

    if (needs_room_leave) {
        handle_leave_room(client_idx, 0);
    }

    pthread_mutex_lock(&clients_mutex);
    if (clients[client_idx].active) {
        client_t *client = &clients[client_idx];
        const char* log_username = client->username[0] ? client->username : "unknown";

        log_message("[DISCONNECT] user '%s' lost connection. Cleaned up resources", log_username);
        
        if (client->sockfd != -1) {
            shutdown(client->sockfd, SHUT_RDWR);
            close(client->sockfd);
            client->sockfd = -1;
        }
        client->active = 0;
        memset(client->username, 0, sizeof(client->username));
        memset(client->current_room_name, 0, sizeof(client->current_room_name));
    }
    pthread_mutex_unlock(&clients_mutex);
}

void handle_whisper_message(int sender_idx, const char* target_username, const char* message) {
    client_t *sender = &clients[sender_idx];
    char response_msg[MAX_MSG_LEN];
    
    if (target_username == NULL || strlen(target_username) == 0) {
        snprintf(response_msg, MAX_MSG_LEN, "%sInvalid target username for whisper.", SERVER_RESPONSE_ERROR);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }
    
    if (message == NULL || strlen(message) == 0) {
        snprintf(response_msg, MAX_MSG_LEN, "%sCannot send empty whisper message.", SERVER_RESPONSE_ERROR);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }
    
    int target_idx = -1;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS_GLOBAL; i++) {
        if (clients[i].active && strcmp(clients[i].username, target_username) == 0) {
            target_idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    if (target_idx == -1) {
        snprintf(response_msg, MAX_MSG_LEN, "%sUser '%s' not found or offline.", SERVER_RESPONSE_ERROR, target_username);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }
    
    if (target_idx == sender_idx) {
        snprintf(response_msg, MAX_MSG_LEN, "%sCannot whisper to yourself.", SERVER_RESPONSE_ERROR);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }
    
    char whisper_msg[MAX_MSG_LEN + MAX_USERNAME_LEN + 20];
    snprintf(whisper_msg, sizeof(whisper_msg), "[WHISPER from %s]: %s", sender->username, message);
    
    
    pthread_mutex_lock(&clients_mutex);
    int target_sockfd = clients[target_idx].active ? clients[target_idx].sockfd : -1;
    pthread_mutex_unlock(&clients_mutex);
    
    if (target_sockfd != -1) {
        if (send(target_sockfd, whisper_msg, strlen(whisper_msg), 0) > 0) {
            snprintf(response_msg, MAX_MSG_LEN, "%sWhisper sent to '%s'.", SERVER_RESPONSE_OK, target_username);
            send(sender->sockfd, response_msg, strlen(response_msg), 0);
            
            log_message("[WHISPER] %s -> %s: %s", sender->username, target_username, message);
        } else {
            snprintf(response_msg, MAX_MSG_LEN, "%sFailed to deliver whisper to '%s'.", SERVER_RESPONSE_ERROR, target_username);
            send(sender->sockfd, response_msg, strlen(response_msg), 0);
        }
    } else {
        snprintf(response_msg, MAX_MSG_LEN, "%sUser '%s' is no longer connected.", SERVER_RESPONSE_ERROR, target_username);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
    }
}

void handle_join_room(int client_idx, const char* room_name_arg) {
    client_t *client = &clients[client_idx];
    char response_msg[MAX_MSG_LEN];

    if (!is_room_name_valid(room_name_arg)) {
        snprintf(response_msg, MAX_MSG_LEN, "%sInvalid room name. Max %d chars, alphanumeric only.", SERVER_RESPONSE_ERROR, MAX_ROOM_NAME_LEN);
        send(client->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }

    if (client->current_room_name[0] != '\0') {
        if (strcmp(client->current_room_name, room_name_arg) == 0) {
            snprintf(response_msg, MAX_MSG_LEN, "%sYou are already in room '%s'.", SERVER_RESPONSE_ERROR, room_name_arg);
            send(client->sockfd, response_msg, strlen(response_msg), 0);
            return;
        }
        
        char old_room[MAX_ROOM_NAME_LEN + 1];
        strncpy(old_room, client->current_room_name, MAX_ROOM_NAME_LEN);
        old_room[MAX_ROOM_NAME_LEN] = '\0';
        
        handle_leave_room(client_idx, 0); 
        
        log_message("[ROOM] user '%s' left room '%s', joined '%s'", client->username, old_room, room_name_arg);
    }

    int room_idx = -1;
    int new_room_created = 0;

    pthread_mutex_lock(&rooms_list_mutex);
    room_idx = find_room_idx_by_name(room_name_arg);

    if (room_idx == -1) { 
        int free_room_slot = -1;
        for (int i = 0; i < MAX_ROOMS; i++) {
            if (!rooms[i].active) {
                free_room_slot = i;
                break;
            }
        }
        if (free_room_slot == -1) {
            pthread_mutex_unlock(&rooms_list_mutex);
            snprintf(response_msg, MAX_MSG_LEN, "%sServer has reached maximum room capacity (%d). Cannot create '%s'.", SERVER_RESPONSE_ERROR, MAX_ROOMS, room_name_arg);
            send(client->sockfd, response_msg, strlen(response_msg), 0);
            return;
        }
        room_idx = free_room_slot;
        rooms[room_idx].active = 1;
        strncpy(rooms[room_idx].name, room_name_arg, MAX_ROOM_NAME_LEN);
        rooms[room_idx].name[MAX_ROOM_NAME_LEN] = '\0';
        rooms[room_idx].num_users = 0;
        for(int i=0; i<MAX_ROOM_USERS; ++i) rooms[room_idx].member_client_indices[i] = -1;
        new_room_created = 1;
    }
    pthread_mutex_unlock(&rooms_list_mutex);

    room_t *target_room = &rooms[room_idx];
    pthread_mutex_lock(&target_room->room_mutex);
    if (target_room->num_users >= MAX_ROOM_USERS) {
        pthread_mutex_unlock(&target_room->room_mutex);
        snprintf(response_msg, MAX_MSG_LEN, "%sRoom '%s' is full (max %d users).", SERVER_RESPONSE_ERROR, room_name_arg, MAX_ROOM_USERS);
        send(client->sockfd, response_msg, strlen(response_msg), 0);
        if (new_room_created) {
             pthread_mutex_lock(&rooms_list_mutex);
             if (rooms[room_idx].active && rooms[room_idx].num_users == 0) {
                rooms[room_idx].active = 0;
                memset(rooms[room_idx].name, 0, sizeof(rooms[room_idx].name));
             }
             pthread_mutex_unlock(&rooms_list_mutex);
        }
        return;
    }

    int added_to_slot = -1;
    for(int i=0; i<MAX_ROOM_USERS; ++i) {
        if(target_room->member_client_indices[i] == -1) {
            target_room->member_client_indices[i] = client_idx;
            added_to_slot = i;
            break;
        }
    }
    if (added_to_slot == -1) { 
         pthread_mutex_unlock(&target_room->room_mutex);
         snprintf(response_msg, MAX_MSG_LEN, "%sInternal server error joining room '%s'.", SERVER_RESPONSE_ERROR, room_name_arg);
         send(client->sockfd, response_msg, strlen(response_msg), 0);
         return;
    }

    target_room->num_users++;
    strncpy(client->current_room_name, target_room->name, MAX_ROOM_NAME_LEN);
    client->current_room_name[MAX_ROOM_NAME_LEN] = '\0';

    char notification[MAX_MSG_LEN];
    snprintf(notification, MAX_MSG_LEN, "[%s][SERVER] User '%s' has joined the room.", target_room->name, client->username);
    for (int i = 0; i < MAX_ROOM_USERS; i++) {
        int member_idx = target_room->member_client_indices[i];
        if (member_idx != -1 && member_idx != client_idx ) {
             pthread_mutex_lock(&clients_mutex);
             int recipient_sockfd = (clients[member_idx].active) ? clients[member_idx].sockfd : -1;
             pthread_mutex_unlock(&clients_mutex);
             if(recipient_sockfd != -1) {
                send(recipient_sockfd, notification, strlen(notification), 0);
             }
        }
    }
    pthread_mutex_unlock(&target_room->room_mutex);

    snprintf(response_msg, MAX_MSG_LEN, "%sYou joined room '%s'.%s", SERVER_RESPONSE_OK, target_room->name, new_room_created ? " (New room created)" : "");
    send(client->sockfd, response_msg, strlen(response_msg), 0);
    
    if (!new_room_created) {
        log_message("[INFO] %s joined room '%s'", client->username, target_room->name);
    } else {
        log_message("[INFO] %s joined room '%s'", client->username, target_room->name);
    }
}

void handle_leave_room(int client_idx, int notify_client_about_leave) {
    client_t *client = &clients[client_idx];
    char response_msg[MAX_MSG_LEN];

    if (client->current_room_name[0] == '\0') {
        if (notify_client_about_leave) {
            snprintf(response_msg, MAX_MSG_LEN, "%sYou are not currently in any room.", SERVER_RESPONSE_ERROR);
            send(client->sockfd, response_msg, strlen(response_msg), 0);
        }
        return;
    }

    char room_to_leave_name[MAX_ROOM_NAME_LEN + 1];
    strncpy(room_to_leave_name, client->current_room_name, MAX_ROOM_NAME_LEN);
    room_to_leave_name[MAX_ROOM_NAME_LEN] = '\0';

    int room_idx = -1;
    pthread_mutex_lock(&rooms_list_mutex);
    room_idx = find_room_idx_by_name(room_to_leave_name);
    pthread_mutex_unlock(&rooms_list_mutex);

    if (room_idx == -1) {
        memset(client->current_room_name, 0, sizeof(client->current_room_name));
        if (notify_client_about_leave) {
             snprintf(response_msg, MAX_MSG_LEN, "%sError leaving room '%s': room no longer exists.", SERVER_RESPONSE_ERROR, room_to_leave_name);
             send(client->sockfd, response_msg, strlen(response_msg), 0);
        }
        return;
    }

    room_t *target_room = &rooms[room_idx];
    int removed_from_list = 0;
    pthread_mutex_lock(&target_room->room_mutex);
    for (int i = 0; i < MAX_ROOM_USERS; i++) {
        if (target_room->member_client_indices[i] == client_idx) {
            target_room->member_client_indices[i] = -1;
            target_room->num_users--;
            removed_from_list = 1;
            break;
        }
    }
    
    if (removed_from_list) {
        char notification[MAX_MSG_LEN];
        snprintf(notification, MAX_MSG_LEN, "[%s][SERVER] User '%s' has left the room.", target_room->name, client->username);
        for (int i = 0; i < MAX_ROOM_USERS; i++) {
            int member_idx = target_room->member_client_indices[i];
            if (member_idx != -1) {
                pthread_mutex_lock(&clients_mutex);
                int recipient_sockfd = (clients[member_idx].active) ? clients[member_idx].sockfd : -1;
                pthread_mutex_unlock(&clients_mutex);
                if(recipient_sockfd != -1) {
                    send(recipient_sockfd, notification, strlen(notification), 0);
                }
            }
        }
    }
    int current_users_in_room = target_room->num_users;
    pthread_mutex_unlock(&target_room->room_mutex);

    memset(client->current_room_name, 0, sizeof(client->current_room_name));

    if (notify_client_about_leave) {
        snprintf(response_msg, MAX_MSG_LEN, "%sYou have left room '%s'.", SERVER_RESPONSE_OK, room_to_leave_name);
        send(client->sockfd, response_msg, strlen(response_msg), 0);
        
        log_message("[INFO] %s left room '%s'", client->username, room_to_leave_name);
    }

    if (current_users_in_room == 0 && removed_from_list) {
        pthread_mutex_lock(&rooms_list_mutex);
        if (rooms[room_idx].active && strcmp(rooms[room_idx].name, room_to_leave_name) == 0 && rooms[room_idx].num_users == 0) {
             rooms[room_idx].active = 0;
             memset(rooms[room_idx].name, 0, sizeof(rooms[room_idx].name));
        }
        pthread_mutex_unlock(&rooms_list_mutex);
    }
}

void handle_broadcast_message(int client_idx, const char* message) {
    client_t *sender = &clients[client_idx];
    char response_msg[MAX_MSG_LEN];

    if (sender->current_room_name[0] == '\0') {
        snprintf(response_msg, MAX_MSG_LEN, "%sYou are not in a room. Join a room first to broadcast.", SERVER_RESPONSE_ERROR);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }

    if (message == NULL || strlen(message) == 0) {
        snprintf(response_msg, MAX_MSG_LEN, "%sCannot broadcast an empty message.", SERVER_RESPONSE_ERROR);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        return;
    }

    int room_idx = -1;
    pthread_mutex_lock(&rooms_list_mutex);
    room_idx = find_room_idx_by_name(sender->current_room_name);
    pthread_mutex_unlock(&rooms_list_mutex);

    if (room_idx == -1) {
        snprintf(response_msg, MAX_MSG_LEN, "%sError broadcasting: your current room '%s' seems to no longer exist.", SERVER_RESPONSE_ERROR, sender->current_room_name);
        send(sender->sockfd, response_msg, strlen(response_msg), 0);
        memset(sender->current_room_name, 0, sizeof(sender->current_room_name));
        return;
    }

    room_t *target_room = &rooms[room_idx];
    char broadcast_content[MAX_MSG_LEN + MAX_USERNAME_LEN + MAX_ROOM_NAME_LEN + 50];
    snprintf(broadcast_content, sizeof(broadcast_content), "[%s] %s: %s", target_room->name, sender->username, message);

    int sent_count = 0;
    pthread_mutex_lock(&target_room->room_mutex);
    for (int i = 0; i < MAX_ROOM_USERS; i++) {
        int member_idx = target_room->member_client_indices[i];
        if (member_idx != -1 && member_idx != client_idx) {
            pthread_mutex_lock(&clients_mutex);
            int recipient_sockfd = -1;
            if (clients[member_idx].active) {
                recipient_sockfd = clients[member_idx].sockfd;
            }
            pthread_mutex_unlock(&clients_mutex);

            if(recipient_sockfd != -1) {
                if (send(recipient_sockfd, broadcast_content, strlen(broadcast_content), 0) > 0) {
                    sent_count++;
                }
            }
        }
    }
    int current_users_in_room = target_room->num_users;
    pthread_mutex_unlock(&target_room->room_mutex);

    if (current_users_in_room <= 1 && sent_count == 0) { 
         snprintf(response_msg, MAX_MSG_LEN, "%sMessage sent in '%s' (you are the only one here).", SERVER_RESPONSE_OK, target_room->name);
    } else {
         snprintf(response_msg, MAX_MSG_LEN, "%sMessage broadcast to %d other user(s) in '%s'.", SERVER_RESPONSE_OK, sent_count, target_room->name);
    }
    send(sender->sockfd, response_msg, strlen(response_msg), 0);
    
    log_message("[BROADCAST] %s: %s", sender->username, message);
}

// thread Function for handling client
void *client_handler_thread(void *arg) {
    int client_idx = (int)(intptr_t)arg;
    char buffer[MAX_MSG_LEN];
    ssize_t bytes_received;
    client_t *current_client = &clients[client_idx];

    int login_successful = 0;
    do {
        char username_buffer[MAX_USERNAME_LEN + 2];
        bytes_received = recv(current_client->sockfd, username_buffer, MAX_USERNAME_LEN + 1, 0);

        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                log_message("[DISCONNECT] Client from %s disconnected before login", current_client->ip_addr);
            } else {
                log_message("[ERROR] Recv error during login from %s: %s", current_client->ip_addr, strerror(errno));
            }
            cleanup_client(client_idx);
            pthread_exit(NULL);
        }
        username_buffer[bytes_received] = '\0';
        username_buffer[strcspn(username_buffer, "\r\n")] = 0;

        char temp_username_storage[MAX_USERNAME_LEN + 1]; 
        strncpy(temp_username_storage, username_buffer, MAX_USERNAME_LEN);
        temp_username_storage[MAX_USERNAME_LEN] = '\0';

        pthread_mutex_lock(&clients_mutex);
        if (is_username_valid_and_unique(username_buffer)) {
            strcpy(current_client->username, username_buffer);
            login_successful = 1;
        }
        pthread_mutex_unlock(&clients_mutex);

        if (login_successful) {
            char ok_msg[MAX_MSG_LEN];
            snprintf(ok_msg, MAX_MSG_LEN, "%sWelcome, %s!", SERVER_RESPONSE_OK, current_client->username);
            send(current_client->sockfd, ok_msg, strlen(ok_msg), 0);
            log_message("[CONNECT] user '%s' connected", current_client->username);
        } else {
            char err_msg[MAX_MSG_LEN];
            if (strlen(temp_username_storage) > 0) {
                snprintf(err_msg, MAX_MSG_LEN, "%sUsername invalid (max %d chars, alphanumeric) or already taken.", SERVER_RESPONSE_ERROR, MAX_USERNAME_LEN);
                log_message("[REJECTED] Duplicate username attempted: %s", temp_username_storage);
            } else {
                snprintf(err_msg, MAX_MSG_LEN, "%sUsername invalid (max %d chars, alphanumeric) or already taken.", SERVER_RESPONSE_ERROR, MAX_USERNAME_LEN);
            }
            send(current_client->sockfd, err_msg, strlen(err_msg), 0);
        }
    } while (!login_successful && server_running);

    // main command receiving loop
    while (server_running && (bytes_received = recv(current_client->sockfd, buffer, MAX_MSG_LEN - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        buffer[strcspn(buffer, "\r\n")] = 0;

        if(strlen(buffer) == 0) continue;

        if (strncmp(buffer, "/join ", 6) == 0) {
            char *room_name = buffer + 6;
            while(*room_name == ' ') room_name++;
            handle_join_room(client_idx, room_name);
        } else if (strcmp(buffer, "/leave") == 0) {
            handle_leave_room(client_idx, 1);
        } else if (strncmp(buffer, "/broadcast ", 11) == 0) {
            char *message = buffer + 11;
            while(*message == ' ') message++;
            handle_broadcast_message(client_idx, message);
        } else if (strncmp(buffer, "/whisper ", 9) == 0) {
            char *args = buffer + 9;
            while(*args == ' ') args++;
            
            char *space_pos = strchr(args, ' ');
            if (space_pos == NULL) {
                char whisper_error[MAX_MSG_LEN];
                snprintf(whisper_error, MAX_MSG_LEN, "%sUsage: /whisper <username> <message>", SERVER_RESPONSE_ERROR);
                send(current_client->sockfd, whisper_error, strlen(whisper_error), 0);
            } else {
                *space_pos = '\0';
                char *target_username = args;
                char *message = space_pos + 1;
                while(*message == ' ') message++;
                
                handle_whisper_message(client_idx, target_username, message);
            }
        } else if (strncmp(buffer, "/sendfile ", 10) == 0) {
            char *args = buffer + 10;
            while(*args == ' ') args++;
            handle_sendfile_command(client_idx, args);
        } else if (strcmp(buffer, "/status") == 0) {
            char status_msg[MAX_MSG_LEN];
            int active_clients = 0;
            int active_transfers, queued_transfers, total_processed, total_failed;
            
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS_GLOBAL; i++) {
                if (clients[i].active) active_clients++;
            }
            pthread_mutex_unlock(&clients_mutex);
            
            get_file_transfer_stats(&active_transfers, &queued_transfers, &total_processed, &total_failed);
            
            snprintf(status_msg, sizeof(status_msg), 
                     "%sServer Status: %d clients online, File transfers: %d active, %d queued, %d completed, %d failed", 
                     SERVER_RESPONSE_OK, active_clients, active_transfers, queued_transfers, total_processed, total_failed);
            send(current_client->sockfd, status_msg, strlen(status_msg), 0);
        } else if (strcmp(buffer, "/exit") == 0) {
            char exit_ack[MAX_MSG_LEN];
            snprintf(exit_ack, sizeof(exit_ack), "%sGoodbye, %s!", SERVER_RESPONSE_OK, current_client->username);
            send(current_client->sockfd, exit_ack, strlen(exit_ack), 0);
            break; 
        } else if (buffer[0] == '/') {
            char unknown_cmd_msg[MAX_MSG_LEN];
            snprintf(unknown_cmd_msg, MAX_MSG_LEN, "%sUnknown command. Available: /join <room>, /leave, /broadcast <msg>, /whisper <user> <msg>, /sendfile <file> <user>, /status, /exit.", SERVER_RESPONSE_ERROR);
            send(current_client->sockfd, unknown_cmd_msg, strlen(unknown_cmd_msg), 0);
        } else {
            char plain_text_error[MAX_MSG_LEN];
            snprintf(plain_text_error, MAX_MSG_LEN, "%sInvalid command format. Commands start with /. Try /sendfile <filename> <username>.", SERVER_RESPONSE_ERROR);
            send(current_client->sockfd, plain_text_error, strlen(plain_text_error), 0);
        }
    }

    if (!server_running) {
        send(current_client->sockfd, "SERVER_DOWN:Server is shutting down gracefully.\n", strlen("SERVER_DOWN:Server is shutting down gracefully.\n"), MSG_NOSIGNAL);
    }

    cleanup_client(client_idx);
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    init_logging();

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        log_message("[ERROR] Usage: %s <port>", argv[0]);
        close_logging();
        exit(EXIT_FAILURE);
    }

    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        log_message("[ERROR] Failed to set SIGINT handler: %s", strerror(errno));
        perror("Failed to set SIGINT handler");
        close_logging();
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        log_message("[ERROR] Invalid port number: %d", port);
        fprintf(stderr, "Invalid port number: %d\n", port);
        close_logging();
        exit(EXIT_FAILURE);
    }

    initialize_clients();
    initialize_rooms();
    init_file_transfer_system(); // Initialize file transfer system

    int server_sockfd_main;
    struct sockaddr_in server_addr, client_addr_main_loop;
    socklen_t client_addr_len_main_loop = sizeof(client_addr_main_loop);

    server_sockfd_main = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sockfd_main < 0) error_exit("Error creating socket");

    int opt = 1;
    if (setsockopt(server_sockfd_main, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) error_exit("setsockopt(SO_REUSEADDR) failed");

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_sockfd_main, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) error_exit("Error binding socket");

    if (listen(server_sockfd_main, 20) < 0) error_exit("Error listening on socket");
    log_message("[INFO] Server listening on port %d...", port);

    while (server_running) {
        int new_client_sockfd = accept(server_sockfd_main, (struct sockaddr *)&client_addr_main_loop, &client_addr_len_main_loop);
        
        if (!server_running) {
            if (new_client_sockfd >= 0) close(new_client_sockfd);
            break;
        }

        if (new_client_sockfd < 0) {
            if (errno == EINTR && !server_running) {
                 log_message("[INFO] accept() interrupted by SIGINT during shutdown");
            } else if (errno == EINTR) {
                 continue;
            } else {
                 log_message("[ERROR] accept() failed: %s", strerror(errno));
                 if (errno == EMFILE || errno == ENFILE || errno == ENOMEM) {
                     server_running = 0;
                     break;
                 }
            }
            continue;
        }

        char client_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr_main_loop.sin_addr, client_ip_str, INET_ADDRSTRLEN);

        pthread_mutex_lock(&clients_mutex);
        int client_idx = find_free_client_slot();

        if (client_idx == -1) {
            pthread_mutex_unlock(&clients_mutex);
            log_message("[ERROR] Max clients (%d) reached. Connection from %s rejected", MAX_CLIENTS_GLOBAL, client_ip_str);
            char full_msg[100];
            snprintf(full_msg, sizeof(full_msg), "%sServer is full (max %d clients). Try again later.", SERVER_RESPONSE_ERROR, MAX_CLIENTS_GLOBAL);
            send(new_client_sockfd, full_msg, strlen(full_msg), 0);
            close(new_client_sockfd);
            continue;
        }

        clients[client_idx].sockfd = new_client_sockfd;
        strcpy(clients[client_idx].ip_addr, client_ip_str);
        clients[client_idx].active = 1;
        
        if (pthread_create(&clients[client_idx].thread_id, NULL, client_handler_thread, (void *)(intptr_t)client_idx) != 0) {
            log_message("[ERROR] Failed to create thread for client from %s: %s", client_ip_str, strerror(errno));
            clients[client_idx].active = 0; 
            close(new_client_sockfd);
            clients[client_idx].sockfd = -1;
        } else {
            pthread_detach(clients[client_idx].thread_id);
        }
        pthread_mutex_unlock(&clients_mutex);
    }

    // Count active clients for shutdown log
    int active_clients = 0;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS_GLOBAL; i++) {
        if (clients[i].active) active_clients++;
    }
    pthread_mutex_unlock(&clients_mutex);
    
    log_message("[SHUTDOWN] SIGINT received. Disconnecting %d clients, saving logs", active_clients);
    
    if (server_sockfd_main >=0) {
        close(server_sockfd_main);
        server_sockfd_main = -1;
    }

    for (int i = 0; i < MAX_CLIENTS_GLOBAL; i++) {
        pthread_mutex_lock(&clients_mutex);
        if (clients[i].active && clients[i].sockfd != -1) {
            send(clients[i].sockfd, "SERVER_DOWN:Server is shutting down NOW.\n", strlen("SERVER_DOWN:Server is shutting down NOW.\n"), MSG_NOSIGNAL);
            shutdown(clients[i].sockfd, SHUT_RDWR);
            close(clients[i].sockfd);
            clients[i].sockfd = -1;
        }
        pthread_mutex_unlock(&clients_mutex);
    }
    
    // Cleanup file transfer system
    cleanup_file_transfer_system();
    
    close_logging();

    for(int i=0; i < MAX_ROOMS; ++i) {
        pthread_mutex_destroy(&rooms[i].room_mutex);
    }
    pthread_mutex_destroy(&rooms_list_mutex);
    pthread_mutex_destroy(&clients_mutex);
    pthread_mutex_destroy(&log_mutex);

    printf("[INFO] Server has shut down gracefully.\n");
    return 0;
}