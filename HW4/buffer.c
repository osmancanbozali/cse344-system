#include <stdlib.h>
#include <stdio.h>
#include "buffer.h"

// Function to handle fatal errors
static void fatal(const char *msg) {
    perror(msg); // Print the error message
    exit(EXIT_FAILURE); // Exit the program with failure status
}

// Function to initialize the buffer
void buffer_init(Buffer *buf, size_t size) {
    buf->data = calloc(size, sizeof(char *));
    if (!buf->data) fatal("calloc");
    buf->size = size;
    buf->head = buf->tail = buf->count = 0;
    buf->terminating = 0;
    // Initialize mutex and condition variables
    if (pthread_mutex_init(&buf->mutex, NULL)) fatal("pthread_mutex_init");
    if (pthread_cond_init(&buf->space_available, NULL)) fatal("pthread_cond_init");
    if (pthread_cond_init(&buf->items_available, NULL)) fatal("pthread_cond_init");
}

// Function to destroy the buffer
void buffer_destroy(Buffer *buf) {
    free(buf->data);
    pthread_mutex_destroy(&buf->mutex);
    pthread_cond_destroy(&buf->space_available);
    pthread_cond_destroy(&buf->items_available);
}

// Function to push an item onto the buffer
void buffer_push(Buffer *buf, char *item) {
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == buf->size && !buf->terminating) { // Wait until the buffer is not full
        pthread_cond_wait(&buf->space_available, &buf->mutex);
    }

    if (buf->terminating) {
        pthread_mutex_unlock(&buf->mutex);
        return;
    }

    buf->data[buf->tail] = item;
    buf->tail = (buf->tail + 1) % buf->size;
    buf->count++;

    pthread_cond_signal(&buf->items_available);
    pthread_mutex_unlock(&buf->mutex);
}

// Function to pop an item from the buffer
char *buffer_pop(Buffer *buf) {
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0 && !buf->terminating) {
        pthread_cond_wait(&buf->items_available, &buf->mutex);
    }

    if (buf->count == 0 && buf->terminating) {
        pthread_mutex_unlock(&buf->mutex);
        return NULL;
    }

    char *item = buf->data[buf->head];
    buf->head = (buf->head + 1) % buf->size;
    buf->count--;

    pthread_cond_signal(&buf->space_available);
    pthread_mutex_unlock(&buf->mutex);
    return item;
}