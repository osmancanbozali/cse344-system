#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>

#define NUM_SATELLITES 5 // Number of satellites
#define NUM_ENGINEERS 3 // Number of engineers
#define MAX_TIMEOUT 5 // Maximum timeout in seconds

typedef struct {
    int id; // Satellite ID
    int priority; // Priority of the request
    sem_t requestHandled; // Semaphore for request handling
    bool isHandled; // Flag to check if request is handled
    bool hasTimedOut; // Flag to check if request has timed out
    time_t timeout; // Timeout duration in seconds
} SatelliteRequest;

typedef struct {
    SatelliteRequest* requests[NUM_SATELLITES]; // Array of requests
    int size; // Current size of the queue
} RequestQueue;

// Global variables
int availableEngineers = NUM_ENGINEERS;
RequestQueue requestQueue = {.size = 0};
pthread_mutex_t engineerMutex = PTHREAD_MUTEX_INITIALIZER;
sem_t newRequest;

// Enqueue function for priority queue
void enqueue(SatelliteRequest* request) {
    // Add request to the end of the queue
    int i = requestQueue.size++;
    requestQueue.requests[i] = request;
    // Rearrange the heap by heapifying up
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (requestQueue.requests[parent]->priority >= requestQueue.requests[i]->priority) break;
        // Swap with parent if higher priority
        SatelliteRequest* temp = requestQueue.requests[parent];
        requestQueue.requests[parent] = requestQueue.requests[i];
        requestQueue.requests[i] = temp;
        i = parent;
    }
}

// Dequeue function for priority queue
SatelliteRequest* dequeue() {
    if (requestQueue.size == 0) return NULL; // No requests to dequeue
    // Get the highest priority request (root of the heap)
    SatelliteRequest* highest = requestQueue.requests[0];
    requestQueue.requests[0] = requestQueue.requests[--requestQueue.size]; // Replace root with last element
    // Rearrange the heap by heapifying down
    int i = 0;
    while (true) { // Check if we need to swap with children
        int leftChild = 2 * i + 1;
        int rightChild = 2 * i + 2;
        int highest_idx = i;
        if (leftChild < requestQueue.size && requestQueue.requests[leftChild]->priority > requestQueue.requests[highest_idx]->priority) highest_idx = leftChild;
        if (rightChild < requestQueue.size && requestQueue.requests[rightChild]->priority > requestQueue.requests[highest_idx]->priority) highest_idx = rightChild;
        if (highest_idx == i) break; // No swap needed
        // Swap with the highest priority child
        SatelliteRequest* temp = requestQueue.requests[i];
        requestQueue.requests[i] = requestQueue.requests[highest_idx];
        requestQueue.requests[highest_idx] = temp;
        i = highest_idx;
    }
    return highest;
}
// Function to remove a request from the queue (used for timeout)
void removeRequest(int satelliteId) {
    for (int i = 0; i < requestQueue.size; i++) { // Find the request with the given satellite ID
        if (requestQueue.requests[i]->id == satelliteId) {
            requestQueue.requests[i] = requestQueue.requests[--requestQueue.size]; // Replace with last element
            // Rearrange the heap by heapifying up or down
            int parent = (i - 1) / 2;
            if (i > 0 && requestQueue.requests[i]->priority > requestQueue.requests[parent]->priority) { // Heapify up
                while (i > 0) {
                    parent = (i - 1) / 2;
                    if (requestQueue.requests[parent]->priority >= requestQueue.requests[i]->priority) break;
                    
                    SatelliteRequest* temp = requestQueue.requests[parent];
                    requestQueue.requests[parent] = requestQueue.requests[i];
                    requestQueue.requests[i] = temp;
                    i = parent;
                }
            } else { // Heapify down
                while (true) {
                    int leftChild = 2 * i + 1;
                    int rightChild = 2 * i + 2;
                    int highest = i;
                    if (leftChild < requestQueue.size && requestQueue.requests[leftChild]->priority > requestQueue.requests[highest]->priority) highest = leftChild;
                    if (rightChild < requestQueue.size && requestQueue.requests[rightChild]->priority > requestQueue.requests[highest]->priority) highest = rightChild;
                    if (highest == i) break;
                    SatelliteRequest* temp = requestQueue.requests[i];
                    requestQueue.requests[i] = requestQueue.requests[highest];
                    requestQueue.requests[highest] = temp;
                    i = highest;
                }
            }
            break;
        }
    }
}

// Satellite thread function
void* satellite(void* arg) {
    int id = *((int*)arg);
    free(arg); // Free the allocated memory for satellite ID
    
    // Create request with random priority and timeout
    int priority = rand() % 5 + 1;
    SatelliteRequest* request = malloc(sizeof(SatelliteRequest));
    if (request == NULL) {
        fprintf(stderr, "Memory allocation failed for satellite request\n");
        return NULL;
    }
    // Initialize request
    request->id = id;
    request->priority = priority;
    request->isHandled = false;
    request->hasTimedOut = false;
    request->timeout = rand() % MAX_TIMEOUT + 1; // Random timeout between 1 and MAX_TIMEOUT
    sem_init(&request->requestHandled, 0, 0); // Initialize semaphore
    
    sleep(rand() % 2);  // Random delay
    
    // Add request to queue with mutex protection
    pthread_mutex_lock(&engineerMutex);
    printf("[SATELLITE] Satellite %d requesting (priority %d)\n", id, priority);
    enqueue(request);
    sem_post(&newRequest); // Signal engineer that a new request is available
    pthread_mutex_unlock(&engineerMutex); 
    
    // Wait with timeout
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += request->timeout;
    
    if (sem_timedwait(&request->requestHandled, &ts) != 0 && !request->isHandled) {
        // Timeout occurred
        pthread_mutex_lock(&engineerMutex);
        request->hasTimedOut = true;
        removeRequest(id);
        printf("[TIMEOUT] Satellite %d timeout %ld seconds.\n", id, request->timeout);
        pthread_mutex_unlock(&engineerMutex);
        
        sem_destroy(&request->requestHandled);
        free(request);
        return NULL;
    }
    return NULL; // Request handled successfully
}

// Engineer thread function
void* engineer(void* arg) {
    int id = *((int*)arg);
    free(arg); // Free the allocated memory for engineer ID
    
    while (1) {
        sem_wait(&newRequest); // Wait for new request
        pthread_mutex_lock(&engineerMutex);
        
        if (requestQueue.size > 0) { // Check if there are requests in the queue
            SatelliteRequest* request = dequeue(); // Dequeue the highest priority request
            
            // Check for exit signal (priority -1)
            if (request->priority == -1) {
                pthread_mutex_unlock(&engineerMutex);
                printf("[ENGINEER %d] Exiting...\n", id);
                sem_destroy(&request->requestHandled);
                free(request);
                return NULL;
            }
            
            // Check if timed out
            if (request->hasTimedOut) {
                pthread_mutex_unlock(&engineerMutex);
                continue;
            }
            
            // Handle request
            request->isHandled = true;
            sem_post(&request->requestHandled);
            availableEngineers--;
            printf("[ENGINEER %d] Handling Satellite %d (Priority %d)\n", id, request->id, request->priority);
            pthread_mutex_unlock(&engineerMutex);
            
            // Process time
            sleep(3 + (rand() % 2)); // Simulate processing time (3-5 seconds)
            printf("[ENGINEER %d] Finished Satellite %d\n", id, request->id);
            // Cleanup resources
            sem_destroy(&request->requestHandled);
            free(request);
            
            pthread_mutex_lock(&engineerMutex);
            availableEngineers++;
            pthread_mutex_unlock(&engineerMutex);
        } else {
            pthread_mutex_unlock(&engineerMutex);
        }
    }
}

// Main function
int main() {
    pthread_t satellite_threads[NUM_SATELLITES];
    pthread_t engineer_threads[NUM_ENGINEERS];
    
    srand(time(NULL));
    sem_init(&newRequest, 0, 0);
    
    // Start engineer threads
    for (int i = 0; i < NUM_ENGINEERS; i++) {
        int* id = malloc(sizeof(int));
        if (id == NULL) {
            fprintf(stderr, "Memory allocation failed for engineer ID\n");
            return 1;
        }
        *id = i;
        pthread_create(&engineer_threads[i], NULL, engineer, id);
    }
    
    // Start satellite threads
    for (int i = 0; i < NUM_SATELLITES; i++) {
        int* id = malloc(sizeof(int));
        if (id == NULL) {
            fprintf(stderr, "Memory allocation failed for satellite ID\n");
            return 1;
        }
        *id = i;
        pthread_create(&satellite_threads[i], NULL, satellite, id);
        usleep(500000);
    }
    
    // Wait for satellites to finish
    for (int i = 0; i < NUM_SATELLITES; i++) {
        pthread_join(satellite_threads[i], NULL);
    }

    // Signal engineers to exit
    for (int i = 0; i < NUM_ENGINEERS; i++) {
        SatelliteRequest* exitRequest = malloc(sizeof(SatelliteRequest));
        if (exitRequest == NULL) {
            fprintf(stderr, "Memory allocation failed for exit request\n");
            return 1;
        }
        exitRequest->priority = -1;
        exitRequest->isHandled = false;
        exitRequest->hasTimedOut = false;
        sem_init(&exitRequest->requestHandled, 0, 0);
        
        pthread_mutex_lock(&engineerMutex);
        enqueue(exitRequest);
        pthread_mutex_unlock(&engineerMutex);
        
        sem_post(&newRequest);
    }
    
    // Wait for engineers to exit
    for (int i = 0; i < NUM_ENGINEERS; i++) {
        pthread_join(engineer_threads[i], NULL);
    }
    
    // Cleanup resources
    sem_destroy(&newRequest);
    pthread_mutex_destroy(&engineerMutex);
    
    return 0;
}
