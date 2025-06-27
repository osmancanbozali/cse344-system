#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include "buffer.h"

// WorkerData structure to hold data for each worker thread
typedef struct {
    Buffer *buf;
    const char *search;
    size_t match_count;
    size_t id;
} WorkerData;

// Global variables
static WorkerData *worker_data = NULL;
static size_t num_workers = 0;
static pthread_barrier_t finish_barrier;
static volatile sig_atomic_t terminate_flag = 0;

// Signal handler for SIGINT
static void sigint_handler(int sig) {
    (void)sig;
    terminate_flag = 1;
}

// Worker thread routine
static void *worker(void *arg) {
    WorkerData *wd = (WorkerData *)arg;
    Buffer *buf = wd->buf;
    const char *search = wd->search;
    size_t local = 0;


    while (1) {
        char *line = buffer_pop(buf);
        if (line == NULL) { // Sentinel value indicating termination
            break;
        }

        if (strstr(line, search) != NULL)
            local++;

        free(line);
    }

    wd->match_count = local;

    printf("Thread %zu finished search with %zu matches.\n", wd->id, local);

    // Wait for all threads to finish
    int rc = pthread_barrier_wait(&finish_barrier);
    if (rc == PTHREAD_BARRIER_SERIAL_THREAD) {
        size_t total = 0;
        for (size_t i = 0; i < num_workers; ++i)
            total += worker_data[i].match_count;
        printf("Total matches: %zu\n", total);
    }
    return NULL;
}

// Function to print usage information
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <buffer_size> <num_workers> <log_file> \"<search_term>\"\n" "Note: Enclose <search_term> in double quotes if it contains spaces.\n", prog);
}

// Function to allocate memory and handle errors
void *xmalloc(size_t size) {
    void *pointer = malloc(size);
    if (!pointer) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    return pointer;
}

// Main function
int main(int argc, char *argv[]) {
    if (argc != 5) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Parse command line arguments
    size_t buffer_size = strtoul(argv[1], NULL, 10);
    num_workers = strtoul(argv[2], NULL, 10);
    const char *file_path = argv[3];
    const char *search_term = argv[4];

    if (buffer_size == 0 || num_workers == 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Set up signal handler for SIGINT
    struct sigaction sa = { 0 };
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    // Allocate memory for worker data and threads
    Buffer buffer;
    buffer_init(&buffer, buffer_size);

    worker_data = xmalloc(sizeof(WorkerData) * num_workers);
    pthread_t *threads = xmalloc(sizeof(pthread_t) * num_workers);

    // Initialize the barrier for synchronization
    if (pthread_barrier_init(&finish_barrier, NULL, num_workers)) {
        perror("pthread_barrier_init");
        exit(EXIT_FAILURE);
    }

    // Create worker threads
    for (size_t i = 0; i < num_workers; ++i) {
        worker_data[i].buf = &buffer;
        worker_data[i].search = search_term;
        worker_data[i].match_count = 0;
        worker_data[i].id = i;
        if (pthread_create(&threads[i], NULL, worker, &worker_data[i])) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    // Open the log file and read lines into the buffer
    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t nread;

    while (!terminate_flag && (nread = getline(&line, &len, fp)) != -1) {
        char *copy = strdup(line);
        if (!copy) {
            perror("strdup");
            exit(EXIT_FAILURE);
        }
        buffer_push(&buffer, copy);
    }

    free(line);
    fclose(fp);

    // Signal termination to all worker threads
    if (terminate_flag) {
        printf("SIGINT received, initiating shutdown...\n");
        pthread_mutex_lock(&buffer.mutex);
        buffer.terminating = 1;
        // Notify all workers to wake up and check for termination
        pthread_cond_broadcast(&buffer.items_available); 
        pthread_cond_broadcast(&buffer.space_available);
        pthread_mutex_unlock(&buffer.mutex);
    } else {
        // Push sentinel values to signal termination to all workers
        for (size_t i = 0; i < num_workers; ++i) {
            buffer_push(&buffer, NULL);
        }
    }

    // Wait for all worker threads to finish
    for (size_t i = 0; i < num_workers; ++i)
        pthread_join(threads[i], NULL);

    // Clean up
    pthread_barrier_destroy(&finish_barrier);
    buffer_destroy(&buffer);

    free(threads);
    free(worker_data);

    return 0;
}