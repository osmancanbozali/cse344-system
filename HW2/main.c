#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define FIFO1 "firstfifo"
#define FIFO2 "secondfifo"
#define LOG_FILE "results.log"
#define TIMEOUT_SECONDS 15
#define CMD_FIND_LARGER 1 // Command to find the larger number
#define MAX_POLL_ATTEMPTS 100
#define POLL_INTERVAL 100000
#define MAX_CHILDREN 10

// Global variables
pid_t child_pids[MAX_CHILDREN];
int child_count = 0;
int completed_children = 0;
FILE* log_file = NULL;
int result = 0;
int num1, num2;

void log_message(const char* msg) {
    time_t now;
    time(&now);
    char* timestamp = ctime(&now);
    timestamp[strlen(timestamp)-1] = '\0';
    
    if (log_file) {
        fprintf(log_file, "[%s] %s\n", timestamp, msg);
        fflush(log_file);
    }
}

void cleanup() {
    if (log_file) fclose(log_file);
    unlink(FIFO1);
    unlink(FIFO2);
    log_message("Cleanup completed");
}

// Register a new child PID
void register_child(pid_t pid) {
    if (child_count < MAX_CHILDREN) {
        child_pids[child_count++] = pid;
    } else {
        log_message("ERROR: Maximum number of children reached");
    }
}

// SIGCHLD handler with proper exit status reporting and child tracking
void sigchld_handler(int sig) {
    pid_t pid;
    int status;
    char msg[100];
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // Find which child terminated
        if(status == sig){}
        for (int i = 0; i < child_count; i++) {
            if (child_pids[i] == pid) {
                if (WIFEXITED(status)) {
                    sprintf(msg, "Child %d terminated normally with exit status %d", 
                            pid, WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    sprintf(msg, "Child %d terminated by signal %d", 
                            pid, WTERMSIG(status));
                }
                log_message(msg);
                
                // Remove terminated child
                for (int j = i; j < child_count - 1; j++) {
                    child_pids[j] = child_pids[j + 1];
                }
                child_count--;
                break;
            }
        }
        completed_children++;
    }
}

// Signal handler for daemon that propagates signals to children
void daemon_signal_handler(int sig) {
    char msg[100];
    sprintf(msg, "Daemon received signal %d", sig);
    log_message(msg);
    
    if (sig == SIGTERM) {
        log_message("Daemon terminating due to SIGTERM, forwarding to children");
        
        // Forward SIGTERM to all children
        for (int i = 0; i < child_count; i++) {
            if (kill(child_pids[i], 0) == 0) { // Check if process exists
                kill(child_pids[i], SIGTERM);
                sprintf(msg, "Sent SIGTERM to child PID: %d", child_pids[i]);
                log_message(msg);
            }
        }
        
        cleanup();
        exit(0);
    } else if (sig == SIGHUP) {
        log_message("Daemon received SIGHUP, reconfiguring and forwarding to children");
        // Forward SIGHUP to children
        for (int i = 0; i < child_count; i++) {
            if (kill(child_pids[i], 0) == 0) {
                kill(child_pids[i], SIGHUP);
                sprintf(msg, "Sent SIGHUP to child PID: %d", child_pids[i]);
                log_message(msg);
            }
        }
        // Reconfiguration code would go here
    } else if (sig == SIGUSR1) {
        log_message("Daemon received SIGUSR1, reporting active children");
        // Report status of all children
        sprintf(msg, "Total active children: %d", child_count);
        log_message(msg);
        for (int i = 0; i < child_count; i++) {
            sprintf(msg, "Active child PID: %d", child_pids[i]);
            log_message(msg);
        }
    }
}

void setup_daemon() {
    pid_t pid = fork();
    if (pid < 0) {
        perror("Daemon fork failed");
        exit(1);
    }
    if (pid > 0) exit(0); // Parent exits
    
    if (setsid() < 0) {
        perror("setsid failed");
        exit(1);
    }
    
    umask(0);
    
    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    log_file = fopen(LOG_FILE, "a");
    if (!log_file) {
        fprintf(stderr, "Failed to open log file\n");
        exit(1);
    }
    
    // Redirect standard file descriptors to log file
    int log_fd = fileno(log_file);
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    
    // Set up signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = daemon_signal_handler;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    
    // Set up SIGCHLD handler
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa_chld, NULL);
    
    log_message("Daemon setup completed with signal handlers");
}

// Helper function to read with polling
int poll_read(int fd, void* buffer, size_t size, const char* error_message) {
    int attempts = 0;
    int bytes_read = 0;
    char msg[150];
    
    while (attempts < MAX_POLL_ATTEMPTS) {
        bytes_read = read(fd, buffer, size);
        
        if (bytes_read > 0) {
            // Successful read
            return bytes_read;
        } else if (bytes_read == 0 || (bytes_read == -1 && errno == EAGAIN)) {
            // No data available yet, continue polling
            usleep(POLL_INTERVAL);
            attempts++;
        } else {
            // Actual error
            sprintf(msg, "%s: %s", error_message, strerror(errno));
            log_message(msg);
            return -1;
        }
    }
    
    // Timeout
    sprintf(msg, "%s: Timed out after %d attempts", 
            error_message, MAX_POLL_ATTEMPTS);
    log_message(msg);
    return -1;
}

// Function to handle child process termination via signals
void setup_child_signal_handler() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    
    sa.sa_handler = SIG_DFL; // Default handler for most signals
    sigaction(SIGCHLD, &sa, NULL);
    
    sa.sa_handler = SIG_DFL;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <num1> <num2>\n", argv[0]);
        return 1;
    }
    
    num1 = atoi(argv[1]);
    num2 = atoi(argv[2]);
    char msg[100];
    
    // Create FIFOs with proper error handling
    if (mkfifo(FIFO1, 0666) < 0 && errno != EEXIST) {
        perror("FIFO1 creation failed");
        return 1;
    }
    if (mkfifo(FIFO2, 0666) < 0 && errno != EEXIST) {
        perror("FIFO2 creation failed");
        unlink(FIFO1);
        return 1;
    }
    
    // Set up daemon process
    setup_daemon();
    log_message("Daemon process started");
    pid_t daemon_pid = getpid();
    sprintf(msg, "Daemon PID: %d", daemon_pid);
    log_message(msg);
    
    pid_t pid1, pid2;
    int expected_children = 2;  // We'll create 2 child processes
    
    // Child 1
    if ((pid1 = fork()) < 0) {
        log_message("Fork failed for Child 1");
        cleanup();
        exit(1);
    } else if (pid1 == 0) {
        // Child 1 process
        setup_child_signal_handler();
        log_message("Child 1 started, sleeping for 10 seconds");
        sleep(10);
        log_message("Child 1 woke up, processing data");
        
        int fd = open(FIFO1, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            log_message("Child 1: Failed to open FIFO1");
            exit(1);
        }
        
        int n1, n2, cmd;
        
        // Use polling for reading num1
        if (poll_read(fd, &n1, sizeof(int), "Child 1: Failed to read first number") <= 0) {
            close(fd);
            exit(1);
        }
        // Use polling for reading num2
        if (poll_read(fd, &n2, sizeof(int), "Child 1: Failed to read second number") <= 0) {
            close(fd);
            exit(1);
        }
        // Use polling for reading cmd
        if (poll_read(fd, &cmd, sizeof(int), "Child 1: Failed to read command") <= 0) {
            close(fd);
            exit(1);
        }
        close(fd);
        
        sprintf(msg, "Child 1: Received numbers %d and %d with command %d", n1, n2, cmd);
        log_message(msg);
        
        int larger;
        if (cmd == CMD_FIND_LARGER) {
            larger = (n1 > n2) ? n1 : n2;
            sprintf(msg, "Child 1: Larger number is %d", larger);
            log_message(msg);
        } else {
            log_message("Child 1: Unknown command received");
            exit(1);
        }
        
        fd = open(FIFO2, O_WRONLY);
        if (fd < 0) {
            log_message("Child 1: Failed to open FIFO2 for writing");
            exit(1);
        }
        
        if (write(fd, &larger, sizeof(int)) < 0) {
            log_message("Child 1: Failed to write larger number");
            close(fd);
            exit(1);
        }
        close(fd);
        
        log_message("Child 1: Successfully wrote result to FIFO2");
        exit(0);
    } else {
        register_child(pid1);
    }
    
    // Child 2
    if ((pid2 = fork()) < 0) {
        log_message("Fork failed for Child 2");
        // Kill Child 1 if it was created successfully
        if (pid1 > 0) kill(pid1, SIGTERM);
        cleanup();
        exit(1);
    } else if (pid2 == 0) {
        // Child 2 process
        setup_child_signal_handler();
        log_message("Child 2 started, sleeping for 10 seconds");
        sleep(10);
        log_message("Child 2 woke up, processing data");
        
        int fd = open(FIFO2, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            log_message("Child 2: Failed to open FIFO2");
            exit(1);
        }
        
        int larger;
        // Use polling for reading
        if (poll_read(fd, &larger, sizeof(int), "Child 2: Failed to read larger number") <= 0) {
            close(fd);
            exit(1);
        }
        close(fd);
        
        result = larger; // Store in global variable
        sprintf(msg, "Child 2: Result - larger number is %d", larger);
        log_message(msg);
        
        exit(0);
    } else {
        // Parent registers the child
        register_child(pid2);
    }
    
    sprintf(msg, "Parent: Created child processes with PIDs %d and %d", pid1, pid2);
    log_message(msg);
    
    sleep(2);
    
    // Monitor child processes, write "proceeding" every 2 seconds
    time_t start_time = time(NULL);
    int wrote_to_fifo = 0;
    
    while (completed_children < expected_children) {
        log_message("Parent: proceeding");
        
        // After the children have been running for at least 10 seconds, send the data
        if (!wrote_to_fifo && time(NULL) - start_time >= 10) {
            // Open FIFO1 for writing
            int fd1 = open(FIFO1, O_WRONLY);
            if (fd1 < 0) {
                log_message("Parent: Failed to open FIFO1");
                cleanup();
                exit(1);
            }
            
            // Write the two integers and command to FIFO1
            if (write(fd1, &num1, sizeof(int)) < 0) {
                log_message("Parent: Failed to write num1 to FIFO1");
                close(fd1);
                cleanup();
                exit(1);
            }
            
            if (write(fd1, &num2, sizeof(int)) < 0) {
                log_message("Parent: Failed to write num2 to FIFO1");
                close(fd1);
                cleanup();
                exit(1);
            }
            
            int cmd = CMD_FIND_LARGER;
            if (write(fd1, &cmd, sizeof(int)) < 0) {
                log_message("Parent: Failed to write command to FIFO1");
                close(fd1);
                cleanup();
                exit(1);
            }
            close(fd1);
            
            sprintf(msg, "Parent: Sent numbers %d and %d with command %d", num1, num2, cmd);
            log_message(msg);
            wrote_to_fifo = 1;
        }
        
        sleep(2);  // 2 second intervals for "proceeding" messages
        
        // Check for timeout
        if (time(NULL) - start_time > TIMEOUT_SECONDS) {
            log_message("Timeout occurred, terminating children");
            // Send SIGTERM to all remaining children
            for (int i = 0; i < child_count; i++) {
                if (kill(child_pids[i], 0) == 0) { // Check if process exists
                    kill(child_pids[i], SIGTERM);
                    sprintf(msg, "Sent SIGTERM to Child PID: %d", child_pids[i]);
                    log_message(msg);
                }
            }
            break;
        }
    }
    
    sprintf(msg, "Program completed.");
    log_message(msg);
    cleanup();
    return 0;
}