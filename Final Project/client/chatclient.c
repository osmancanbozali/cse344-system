#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>     

#include "common.h"

#define RECEIVED_FILES_DIR "receivedFiles"

volatile sig_atomic_t client_running = 1; // Controls main thread
volatile sig_atomic_t server_disconnected = 0; // Track server disconnection
int global_sockfd = -1;
pthread_t global_recv_tid = 0;

// Signal handler
void signal_handler(int signum) {
    if (signum == SIGINT) {
        printf("\n[INFO] Ctrl+C detected. Initiating graceful shutdown...\n");
        client_running = 0;
        
        if (global_sockfd != -1 && !server_disconnected) {
            send(global_sockfd, "/exit", strlen("/exit"), MSG_NOSIGNAL);
        }
    } else if (signum == SIGPIPE) { // Server disconnected unexpectedly
        printf("\n[INFO] Server connection lost (SIGPIPE).\n");
        server_disconnected = 1;
        client_running = 0;
    }
}

void error_exit(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

void clear_stdin_buffer() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

int is_local_username_valid(const char *username_input, size_t len) {
    if (len == 0 || len > MAX_USERNAME_LEN) {
        printf("%s[CLIENT_ERROR] Username must be 1-%d characters long.%s\n", KRED, MAX_USERNAME_LEN, KNRM);
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)username_input[i])) {
            printf("%s[CLIENT_ERROR] Username must be alphanumeric.%s\n", KRED, KNRM);
            return 0;
        }
    }
    return 1;
}

// Create receivedFiles directory
void create_received_files_directory() {
    struct stat st = {0};
    if (stat(RECEIVED_FILES_DIR, &st) == -1) {
        if (mkdir(RECEIVED_FILES_DIR, 0755) == 0) {
            printf("%s[INFO] Created directory: %s%s\n", KYEL, RECEIVED_FILES_DIR, KNRM);
        } else {
            printf("%s[WARNING] Failed to create directory: %s%s\n", KYEL, RECEIVED_FILES_DIR, KNRM);
        }
    }
}

// Create a received file
void create_received_file(const char* filename, const char* sender, size_t file_size) {
    char filepath[2048];
    snprintf(filepath, sizeof(filepath), "%s/%s", RECEIVED_FILES_DIR, filename);
    
    // Check if file already exists
    struct stat st;
    if (stat(filepath, &st) == 0) {
        // File exists, generate numbered version
        char base_path[2048];
        snprintf(base_path, sizeof(base_path), "%s/%s", RECEIVED_FILES_DIR, filename);
        
        char* dot = strrchr(base_path, '.');
        char extension[256] = "";
        char base_without_ext[1792];
        
        if (dot != NULL) {
            // Save extension
            strncpy(extension, dot, sizeof(extension) - 1);
            extension[sizeof(extension) - 1] = '\0';
            
            // Create base path
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
        
        // Collution resolution
        int counter = 1;
        do {
            if (strlen(extension) > 0) {
                int needed_size = snprintf(NULL, 0, "%s_%d%s", base_without_ext, counter, extension);
                if (needed_size < (int)sizeof(filepath)) {
                    snprintf(filepath, sizeof(filepath), "%s_%d%s", base_without_ext, counter, extension);
                } else {
                    snprintf(filepath, sizeof(filepath), "%s/%s_%d", RECEIVED_FILES_DIR, filename, counter);
                }
            } else {
                snprintf(filepath, sizeof(filepath), "%s_%d", base_without_ext, counter);
            }
            counter++;
        } while (stat(filepath, &st) == 0 && counter < 1000);
    }
    
    FILE* file = fopen(filepath, "w");
    if (file != NULL) {
        time_t current_time = time(NULL);
        char* time_str = ctime(&current_time);
        time_str[strlen(time_str) - 1] = '\0';
        
        fprintf(file, "=== RECEIVED FILE ===\n");
        fprintf(file, "Original Filename: %s\n", filename);
        fprintf(file, "Sender: %s\n", sender);
        fprintf(file, "File Size: %zu bytes (%.2f KB)\n", file_size, (float)file_size / 1024.0);
        fprintf(file, "Received Time: %s\n", time_str);
        fprintf(file, "===================\n\n");
        
        fclose(file);
        printf("%s File saved: %s%s\n", KCYN, filepath, KNRM);
    } else {
        printf("%s[ERROR] Failed to create received file: %s%s\n", KRED, filepath, KNRM);
    }
}

// Parse file notification message
int parse_file_notification(const char* message, char* filename, char* sender, size_t* file_size) {
    const char* file_start = strstr(message, "file '");
    const char* from_start = strstr(message, "' from '");
    const char* size_start = strstr(message, "(size: ");
    
    if (!file_start || !from_start || !size_start) { // invalid format check
        return 0;
    }
    
    // Extract filename
    file_start += 6;
    size_t filename_len = from_start - file_start;
    if (filename_len >= MAX_FILENAME_LEN) filename_len = MAX_FILENAME_LEN - 1;
    strncpy(filename, file_start, filename_len);
    filename[filename_len] = '\0';
    
    // Extract sender
    from_start += 8;
    const char* sender_end = strchr(from_start, '\'');
    if (!sender_end) return 0;
    size_t sender_len = sender_end - from_start;
    if (sender_len >= MAX_USERNAME_LEN) sender_len = MAX_USERNAME_LEN - 1;
    strncpy(sender, from_start, sender_len);
    sender[sender_len] = '\0';
    
    // Extract file size
    size_start += 7;
    *file_size = (size_t)atol(size_start);
    
    return 1;
}

// Thread function to handle receiving messages from the server
void *receive_handler_thread(void *arg) {
    int sockfd = *(int *)arg;
    char server_buffer[MAX_MSG_LEN];
    ssize_t bytes_received;

    while (client_running && !server_disconnected) {
        bytes_received = recv(sockfd, server_buffer, MAX_MSG_LEN - 1, 0);
        if (bytes_received > 0) {
            server_buffer[bytes_received] = '\0';
            
            if (strncmp(server_buffer, "SERVER_DOWN:", strlen("SERVER_DOWN:")) == 0) {
                printf("\r%s[Server]: %s%s\n", KYEL, server_buffer, KNRM);
                printf("%s[INFO] Server is shutting down. Disconnecting...%s\n", KYEL, KNRM);
                server_disconnected = 1;
                client_running = 0;
                break; 
            } else if (strncmp(server_buffer, SERVER_RESPONSE_OK, strlen(SERVER_RESPONSE_OK)) == 0 &&
                       strstr(server_buffer, "Goodbye") != NULL) {
                printf("\r%s[Server]: %s%s\n", KGRN, server_buffer + strlen(SERVER_RESPONSE_OK), KNRM);
                printf("%s[INFO] Gracefully disconnected from server.%s\n", KYEL, KNRM);
                client_running = 0;
                break;
            } else if (strncmp(server_buffer, SERVER_RESPONSE_OK, strlen(SERVER_RESPONSE_OK)) == 0) {
                // Check whether it's a file transfer message
                const char* msg_content = server_buffer + strlen(SERVER_RESPONSE_OK);
                if (strstr(msg_content, "queued") || strstr(msg_content, "sent successfully") || 
                    strstr(msg_content, "File") || strstr(msg_content, "Server Status")) {
                    printf("\r%s[Server]: %s%s\n> ", KGRN, msg_content, KNRM);
                } else {
                    printf("\r%s[Server]: %s%s\n> ", KGRN, msg_content, KNRM);
                }
            } else if (strncmp(server_buffer, SERVER_RESPONSE_ERROR, strlen(SERVER_RESPONSE_ERROR)) == 0) {
                const char* error_content = server_buffer + strlen(SERVER_RESPONSE_ERROR);
                printf("\r%s[Error]: %s%s\n> ", KRED, error_content, KNRM);
            } else if (strncmp(server_buffer, "[WHISPER from ", 14) == 0) {
                printf("\r%s %s%s\n> ", KMAG, server_buffer, KNRM); // whisper messages
            } else if (strncmp(server_buffer, FILE_NOTIFICATION, strlen(FILE_NOTIFICATION)) == 0) {
                const char* file_msg = server_buffer + strlen(FILE_NOTIFICATION); // file notification messages
                printf("\r%s %s%s\n", KCYN, file_msg, KNRM);
                
                // Parse and create the received file
                char filename[MAX_FILENAME_LEN];
                char sender[MAX_USERNAME_LEN + 1];
                size_t file_size;
                
                if (parse_file_notification(file_msg, filename, sender, &file_size)) {
                    create_received_file(filename, sender, file_size);
                } else {
                    printf("%s[WARNING] Could not parse file notification details%s\n", KYEL, KNRM);
                }
                printf("> ");
            } else if (strstr(server_buffer, "joined the room") || strstr(server_buffer, "left the room")) {
                printf("\r%s%s%s\n> ", KYEL, server_buffer, KNRM); // room join/leave messages
            } else if (server_buffer[0] == '[' && strchr(server_buffer, ']') != NULL) {
                printf("\r%s%s%s\n> ", KBLU, server_buffer, KNRM); // room broadcast messages
            } else {
                printf("\r%s%s%s\n> ", KBLU, server_buffer, KNRM); // server messages
            }
            fflush(stdout);
        } else if (bytes_received == 0) {
            printf("\r%s[INFO] Server closed the connection.%s\n", KYEL, KNRM);
            server_disconnected = 1;
            client_running = 0;
            break;
        } else {
            if (client_running && !server_disconnected && errno != EINTR) {
                printf("\r%s[ERROR] Connection error: %s%s\n", KRED, strerror(errno), KNRM);
            }
            server_disconnected = 1;
            client_running = 0;
            break;
        }
    }
    
    printf("[INFO] Receiver thread terminating.\n");
    return NULL;
}

int validate_sendfile_args(const char* args, char* filename, char* username) {
    if (args == NULL || strlen(args) == 0) return 0;
    
    while (*args == ' ') args++;
    const char* space = strchr(args, ' ');
    if (space == NULL) return 0;
    
    // Extract filename
    size_t filename_len = space - args;
    if (filename_len >= MAX_FILENAME_LEN) filename_len = MAX_FILENAME_LEN - 1;
    strncpy(filename, args, filename_len);
    filename[filename_len] = '\0';
    
    // Extract username
    space++;
    while (*space == ' ') space++;
    strncpy(username, space, MAX_USERNAME_LEN);
    username[MAX_USERNAME_LEN] = '\0';
    int len = strlen(username);
    while (len > 0 && username[len-1] == ' ') {
        username[--len] = '\0';
    }
    
    return (strlen(filename) > 0 && strlen(username) > 0);
}

int is_valid_file_extension(const char* filename) {
    if (filename == NULL) return 0;
    
    const char* dot = strrchr(filename, '.');
    if (dot == NULL) return 0;
    
    return (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".pdf") == 0 || strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".png") == 0);
}

// Check if input is available on stdin
int input_available() {
    fd_set readfds;
    struct timeval timeout;
    
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    
    int result = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
    
    if (result < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }
    
    return result > 0 && FD_ISSET(STDIN_FILENO, &readfds);
}

// Graceful cleanup
void cleanup_and_exit(int sockfd, pthread_t recv_tid) {
    printf("\n[INFO] Cleaning up and shutting down...\n");
    
    client_running = 0;
    
    // Close socket
    if (sockfd != -1) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
        global_sockfd = -1;
    }
    
    // Wait for receiver thread to finish
    if (recv_tid != 0) {
        void *thread_result;
        int join_result = pthread_join(recv_tid, &thread_result);
        if (join_result != 0) {
            printf("[WARNING] Failed to join receiver thread: %s\n", strerror(join_result));
        }
    } else if (global_recv_tid != 0) {
        void *thread_result;
        int join_result = pthread_join(global_recv_tid, &thread_result);
        if (join_result != 0) {
            printf("[WARNING] Failed to join global receiver thread: %s\n", strerror(join_result));
        }
    }
    printf("%s[INFO] Client disconnected and resources cleaned up.%s\n", KGRN, KNRM);
    
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        fprintf(stderr, "Example: %s 127.0.0.1 5000\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Set up signal handler
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Failed to set up SIGINT handler");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("Failed to set up SIGPIPE handler");
        exit(EXIT_FAILURE);
    }

    char *server_ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %d\n", port);
        exit(EXIT_FAILURE);
    }

    int sockfd;
    struct sockaddr_in server_addr;
    char user_input_buffer[MAX_MSG_LEN];
    char username_input[MAX_USERNAME_LEN + 2];

    create_received_files_directory();

    printf("Connecting to %s:%d...\n", server_ip, port);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error_exit("Error creating socket");
    
    // Store global socket for signal handler
    global_sockfd = sockfd;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        error_exit("Invalid server IP address");
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        error_exit("Error connecting to server");
    }
    printf("%s[INFO] Connected to server %s:%d.%s\n", KGRN, server_ip, port, KNRM);

    // Username registration loop
    int login_successful = 0;
    do {
        if (!client_running || server_disconnected) {
            printf("[INFO] Terminating due to shutdown signal or server disconnection.\n");
            cleanup_and_exit(sockfd, 0);
        }
        
        // Get username from user
        int valid_username_entered = 0;
        do {
            if (!client_running || server_disconnected) {
                cleanup_and_exit(sockfd, 0);
            }
            
            printf("Enter your username (max %d chars, alphanumeric): ", MAX_USERNAME_LEN);
            fflush(stdout);

            int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
            fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);
            
            char *input_result = NULL;
            while (!input_result && client_running && !server_disconnected) {
                input_result = fgets(username_input, sizeof(username_input), stdin);
                
                if (!input_result) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        fd_set check_fds;
                        struct timeval check_timeout;
                        FD_ZERO(&check_fds);
                        FD_SET(sockfd, &check_fds);
                        check_timeout.tv_sec = 0;
                        check_timeout.tv_usec = 100000;
                        
                        int server_ready = select(sockfd + 1, &check_fds, NULL, NULL, &check_timeout);
                        if (server_ready > 0) {
                            char check_buffer[256];
                            ssize_t check_bytes = recv(sockfd, check_buffer, sizeof(check_buffer) - 1, MSG_DONTWAIT);
                            if (check_bytes <= 0) {
                                printf("\n[INFO] Server disconnected during username entry.\n");
                                server_disconnected = 1;
                                fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
                                cleanup_and_exit(sockfd, 0);
                            } else {
                                check_buffer[check_bytes] = '\0';
                                if (strstr(check_buffer, "SERVER_DOWN:") != NULL) {
                                    printf("\n[INFO] Server is shutting down.\n");
                                    server_disconnected = 1;
                                    fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
                                    cleanup_and_exit(sockfd, 0);
                                }
                                printf("\n[Server]: %s\n", check_buffer);
                                printf("Enter your username (max %d chars, alphanumeric): ", MAX_USERNAME_LEN);
                                fflush(stdout);
                            }
                        }
                        
                        usleep(50000);
                        continue;
                    } else {
                        if (!client_running || server_disconnected) {
                            fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
                            cleanup_and_exit(sockfd, 0);
                        }
                        fprintf(stderr, "Error reading username.\n"); 
                        fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
                        close(sockfd);
                        exit(EXIT_FAILURE);
                    }
                }
            }
            
            fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
            
            if (!client_running || server_disconnected) {
                cleanup_and_exit(sockfd, 0);
            }
            
            if (input_result) {
                char *newline_ptr = strchr(username_input, '\n');
                if (newline_ptr == NULL) {
                    printf("%s[CLIENT_ERROR] Username too long. Must be max %d characters.%s\n", KRED, MAX_USERNAME_LEN, KNRM);
                    clear_stdin_buffer(); 
                    valid_username_entered = 0;
                } else {
                    *newline_ptr = '\0';
                    char *cr_ptr = strchr(username_input, '\r'); 
                    if (cr_ptr != NULL) *cr_ptr = '\0';
                    if (is_local_username_valid(username_input, strlen(username_input))) {
                        valid_username_entered = 1;
                    } else {
                        valid_username_entered = 0;
                    }
                }
            }
        } while (!valid_username_entered && client_running && !server_disconnected);
        
        if (!client_running || server_disconnected) {
            cleanup_and_exit(sockfd, 0);
        }

        // Send username to server
        if (send(sockfd, username_input, strlen(username_input), MSG_NOSIGNAL) < 0) {
            if (errno == EPIPE) {
                printf("[INFO] Server disconnected while sending username.\n");
                server_disconnected = 1;
            } else {
                perror("Error sending username");
            }
            cleanup_and_exit(sockfd, 0);
        }
        
        // Receive server response
        fd_set recv_fds;
        struct timeval recv_timeout;
        FD_ZERO(&recv_fds);
        FD_SET(sockfd, &recv_fds);
        recv_timeout.tv_sec = 5;
        recv_timeout.tv_usec = 0;
        
        int recv_ready = select(sockfd + 1, &recv_fds, NULL, NULL, &recv_timeout);
        if (recv_ready <= 0) {
            printf("[ERROR] Server response timeout or error during username approval.\n");
            cleanup_and_exit(sockfd, 0);
        }
        
        char initial_response[MAX_MSG_LEN];
        ssize_t bytes = recv(sockfd, initial_response, MAX_MSG_LEN - 1, 0);
        if (bytes <= 0) {
            if (bytes == 0) {
                printf("%s[Server] Server closed connection during username approval.%s\n", KRED, KNRM);
            } else {
                printf("[ERROR] Error receiving username approval: %s\n", strerror(errno));
            }
            server_disconnected = 1;
            cleanup_and_exit(sockfd, 0);
        }
        initial_response[bytes] = '\0';
        
        if (strncmp(initial_response, SERVER_RESPONSE_OK, strlen(SERVER_RESPONSE_OK)) == 0) {
            printf("%s[Server]: %s%s\n", KGRN, initial_response + strlen(SERVER_RESPONSE_OK), KNRM);
            login_successful = 1;
        } else {
            printf("%s[Server]: %s%s\n", KRED, 
                   (strncmp(initial_response, SERVER_RESPONSE_ERROR, strlen(SERVER_RESPONSE_ERROR)) == 0 ? 
                    initial_response + strlen(SERVER_RESPONSE_ERROR) : initial_response), KNRM);
            printf("%s[INFO] Please try a different username.%s\n", KYEL, KNRM);
            login_successful = 0;
        }
    } while (!login_successful && client_running && !server_disconnected);
    
    if (!client_running || server_disconnected) {
        cleanup_and_exit(sockfd, 0);
    }

    // Start the receiver thread
    pthread_t recv_tid;
    if (pthread_create(&recv_tid, NULL, receive_handler_thread, &sockfd) != 0) {
        error_exit("Error creating receiver thread");
    }
    
    // Store global receiver thread ID for cleanup
    global_recv_tid = recv_tid;

    printf("\n%s[READY] You can now start chatting!%s\n", KGRN, KNRM);
    printf("> ");
    fflush(stdout);

    // Main input loop
    while (client_running && !server_disconnected) {
        int input_ready = input_available();
        
        if (input_ready < 0) {
            if (errno == EINTR && !client_running) {
                break;
            }
            continue;
        }
        
        if (input_ready > 0) {
            if (fgets(user_input_buffer, MAX_MSG_LEN, stdin) == NULL) {
                if (client_running && !server_disconnected) {
                    printf("\n%s[INFO] EOF detected. Sending /exit command to server.%s\n", KYEL, KNRM);
                    if (send(sockfd, "/exit", strlen("/exit"), MSG_NOSIGNAL) < 0) {
                        if (client_running && !server_disconnected) {
                            printf("[WARNING] Failed to send /exit: %s\n", strerror(errno));
                        }
                    }
                }
                break;
            }

            char *newline_ptr_msg = strchr(user_input_buffer, '\n');
            if (newline_ptr_msg == NULL) {
                printf("%s[CLIENT_ERROR] Message too long. Discarding.%s\n> ", KRED, KNRM);
                clear_stdin_buffer();
                fflush(stdout);
                continue;
            } else {
                *newline_ptr_msg = '\0';
                char *cr_ptr_msg = strchr(user_input_buffer, '\r'); 
                if (cr_ptr_msg != NULL) *cr_ptr_msg = '\0';
            }
            
            if (strlen(user_input_buffer) == 0) { // enter check
                if (client_running && !server_disconnected) { 
                    printf("> "); 
                    fflush(stdout); 
                }
                continue;
            }
            
            // Client side validation for sendfile command
            if (strncmp(user_input_buffer, "/sendfile ", 10) == 0) {
                char filename[MAX_FILENAME_LEN];
                char username[MAX_USERNAME_LEN + 1];
                
                if (!validate_sendfile_args(user_input_buffer + 10, filename, username)) {
                    printf("%s[CLIENT_ERROR] Usage: /sendfile <filename> <username>%s\n", KRED, KNRM);
                    printf("%sExample: /sendfile document.pdf alice%s\n", KYEL, KNRM);
                    if (client_running && !server_disconnected) {
                        printf("> ");
                        fflush(stdout);
                    }
                    continue;
                }
                
                struct stat file_stat;
                if (stat(filename, &file_stat) != 0) {
                    printf("%s[CLIENT_ERROR] File '%s' not found or cannot be accessed.%s\n", KRED, filename, KNRM);
                    if (client_running && !server_disconnected) {
                        printf("> ");
                        fflush(stdout);
                    }
                    continue;
                }
                
                if (!S_ISREG(file_stat.st_mode)) {
                    printf("%s[CLIENT_ERROR] '%s' is not a regular file.%s\n", KRED, filename, KNRM);
                    if (client_running && !server_disconnected) {
                        printf("> ");
                        fflush(stdout);
                    }
                    continue;
                }
                
                if (!is_valid_file_extension(filename)) {
                    printf("%s[CLIENT_ERROR] File type not supported. Allowed: .txt, .pdf, .jpg, .png%s\n", KRED, KNRM);
                    if (client_running && !server_disconnected) {
                        printf("> ");
                        fflush(stdout);
                    }
                    continue;
                }
                
                char sendfile_with_size[MAX_MSG_LEN];
                snprintf(sendfile_with_size, sizeof(sendfile_with_size), "/sendfile %s %s %zu", 
                         filename, username, (size_t)file_stat.st_size);
                
                printf("%s[INFO] Sending file '%s' (%.2f KB) to '%s'...%s\n", 
                       KYEL, filename, (float)file_stat.st_size / 1024.0, username, KNRM);
                
                if (send(sockfd, sendfile_with_size, strlen(sendfile_with_size), MSG_NOSIGNAL) < 0) {
                    if (errno == EPIPE) {
                        printf("[INFO] Server disconnected while sending file command.\n");
                        server_disconnected = 1;
                    } else if (client_running && !server_disconnected) {
                        printf("[ERROR] Error sending message to server: %s\n", strerror(errno));
                    }
                    break;
                }
                
                if (client_running && !server_disconnected) {
                    printf("> ");
                    fflush(stdout);
                }
                continue;
            }

            if (send(sockfd, user_input_buffer, strlen(user_input_buffer), MSG_NOSIGNAL) < 0) {
                if (errno == EPIPE) {
                    printf("[INFO] Server disconnected while sending command.\n");
                    server_disconnected = 1;
                } else if (client_running && !server_disconnected) {
                    printf("[ERROR] Error sending message to server: %s\n", strerror(errno));
                }
                client_running = 0;
                break;
            }

            if (strncmp(user_input_buffer, "/exit", 5) == 0) {
                printf("[INFO] /exit command sent. Waiting for server confirmation...\n");
            } else {
                if (client_running && !server_disconnected) { 
                    printf("> "); 
                    fflush(stdout);
                }
            }
            
            if (!client_running || server_disconnected) break;
        }
        
        if (client_running && !server_disconnected) {
            usleep(50000);
        }
    }

    cleanup_and_exit(sockfd, recv_tid);
}