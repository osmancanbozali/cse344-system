#ifndef BUFFER_H
#define BUFFER_H

#include <pthread.h>
#include <stddef.h>

typedef struct {
    char **data; // Pointer to the buffer data
    size_t size; // Size of the buffer
    size_t head; // Index of the head of the buffer
    size_t tail; // Index of the tail of the buffer
    size_t count; // Number of items in the buffer
    pthread_mutex_t mutex; // Mutex for synchronizing access to the buffer
    pthread_cond_t space_available;  // Condition variable for signaling when buffer has space
    pthread_cond_t items_available;  // Condition variable for signaling when buffer has items
    int terminating; // Flag to signal termination
} Buffer;

// Function prototypes
void buffer_init(Buffer *buf, size_t size);
void buffer_destroy(Buffer *buf);
void buffer_push(Buffer *buf, char *item);
char *buffer_pop(Buffer *buf);

#endif /* BUFFER_H */
