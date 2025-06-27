#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>

#include "common.h"

// Global arrays to store requests
InitialClientRequest initialClientRequests[MAX_TRANSACTIONS];
int initialClientRequestCount = 0; 
TransactionRequest transactionRequests[MAX_TRANSACTIONS];
int transactionRequestCount = 0;

volatile sig_atomic_t shutdownRequested = 0; // Global flag for shutdown

// Global arrays to store FIFO paths
char clientRequestFifos[MAX_TRANSACTIONS][MAX_PATH_LENGTH];
char clientResponseFifos[MAX_TRANSACTIONS][MAX_PATH_LENGTH];
int fifoCount = 0;
int serverFifo = -1; // Server FIFO file descriptor
pid_t childPids[MAX_TRANSACTIONS]; // Array to store child process IDs
int childCount = 0; // Number of child processes

// Function to clean up client resources
void cleanupClient() {
    if (serverFifo != -1) {
        close(serverFifo);
        serverFifo = -1;
    }
    // Terminate child processes
    for (int i = 0; i < childCount; i++) {
        if (childPids[i] > 0) {
            kill(childPids[i], SIGTERM);
            int status;
            waitpid(childPids[i], &status, 0); // Wait for child to terminate
        }
    }
    // Close and unlink FIFOs
    for (int i = 0; i < fifoCount; i++) {
        unlink(clientRequestFifos[i]);
        unlink(clientResponseFifos[i]);
    }
    printf("Client cleanup completed\n");
}

// Signal handler for client
void clientSignalHandler(int signum) {
    shutdownRequested = 1;
}

// Function to set up signal handlers for client
void setupClientSignalHandlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = clientSignalHandler;    
    sigaction(SIGINT, &sa, NULL);   // ctrl+c
    sigaction(SIGTERM, &sa, NULL);  // kill
    sigaction(SIGHUP, &sa, NULL);   // terminal closed
    
}

// Function to read and parse the client file
void readClientFile(const char* filename) {
    FILE* clientFile = fopen(filename, "r");
    if (clientFile == NULL) {
        printf("Error: Failed to open client file %s\n", filename);
        return;
    }
    printf("Reading %s..\n", filename);
    
    char line[256];
    char originalLine[256];
    char accountId[MAX_ID_LENGTH] = {0};
    char transactionType[20] = {0};
    int amount = 0;
    
    initialClientRequestCount = 0;
    transactionRequestCount = 0;
    
    // Read the client file line by line
    while (fgets(line, sizeof(line), clientFile) && initialClientRequestCount < MAX_TRANSACTIONS && transactionRequestCount < MAX_TRANSACTIONS) {   
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue; // skip empty lines or lines with just whitespace
    
        strncpy(originalLine, line, sizeof(originalLine) - 1);
        originalLine[sizeof(originalLine) - 1] = '\0';
        
        // Remove trailing newline character if present
        size_t len = strlen(line);
        if (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[len-1] = '\0';
            originalLine[len-1] = '\0';
        }
        
        memset(accountId, 0, sizeof(accountId));
        memset(transactionType, 0, sizeof(transactionType));
        amount = 0;
                
        char* token = strtok(line, " \t");
        if (!token) continue;
        
        strncpy(accountId, token, MAX_ID_LENGTH - 1); // Get account ID
        
        // Get transaction type
        token = strtok(NULL, " \t");
        if (!token) continue;
        strncpy(transactionType, token, sizeof(transactionType) - 1);
        
        // Get amount
        token = strtok(NULL, " \t");
        if (!token) continue;
        amount = atoi(token);
                
        // Check if this is a new account request ('N')
        if (strcmp(accountId, "N") == 0) {
            InitialClientRequest request;
            memset(&request, 0, sizeof(InitialClientRequest));
            strncpy(request.accountId, "N", MAX_ID_LENGTH - 1);

            // Determine transaction type
            if (strcmp(transactionType, "deposit") == 0) request.transactionType = 'D';
            else if (strcmp(transactionType, "withdraw") == 0) request.transactionType = 'W';
            else continue;

            // Set client process ID and FIFOs
            request.clientPid = getpid();
            snprintf(request.clientRequestFifo, sizeof(request.clientRequestFifo) - 1, "client_%d_request", request.clientPid);
            snprintf(request.clientResponseFifo, sizeof(request.clientResponseFifo) - 1, "client_%d_response", request.clientPid);
            
            // Store the initial request in the initialClientRequests array
            if (initialClientRequestCount < MAX_TRANSACTIONS) {
                initialClientRequests[initialClientRequestCount++] = request;
                
                // Store the corresponding transaction request in the transactionRequests array
                TransactionRequest txRequest;
                memset(&txRequest, 0, sizeof(TransactionRequest));
                strncpy(txRequest.accountId, "N", MAX_ID_LENGTH - 1);
                txRequest.amount = amount;
                if (transactionRequestCount < MAX_TRANSACTIONS) transactionRequests[transactionRequestCount++] = txRequest;
                else printf("Warning: Maximum transaction requests reached, skipping additional transactions\n");
            } else printf("Warning: Maximum initial client requests reached, skipping additional requests\n");
        } 
        else {
            char fullAccountId[MAX_ID_LENGTH] = {0};
            if (sscanf(originalLine, "%19s", fullAccountId) != 1) {
                printf("Warning: Failed to parse account ID from original line\n");
                continue;
            }
                        
            // Create initial client request
            InitialClientRequest request;
            memset(&request, 0, sizeof(InitialClientRequest));
            strncpy(request.accountId, fullAccountId, MAX_ID_LENGTH - 1);
            
            // Determine transaction type
            if (strcmp(transactionType, "deposit") == 0) request.transactionType = 'D';
            else if (strcmp(transactionType, "withdraw") == 0) request.transactionType = 'W';
            else continue;
            
            // Set client process ID and FIFOs
            request.clientPid = getpid();
            snprintf(request.clientRequestFifo, sizeof(request.clientRequestFifo) - 1, "client_%d_request", request.clientPid);
            snprintf(request.clientResponseFifo, sizeof(request.clientResponseFifo) - 1, "client_%d_response", request.clientPid);
            
            // Store the initial request in the initialClientRequests array 
            if (initialClientRequestCount < MAX_TRANSACTIONS) {
                initialClientRequests[initialClientRequestCount++] = request;
                
                // Store the corresponding transaction request in the transactionRequests array
                TransactionRequest txRequest;
                memset(&txRequest, 0, sizeof(TransactionRequest));
                strncpy(txRequest.accountId, fullAccountId, MAX_ID_LENGTH - 1);
                txRequest.amount = amount;
                if (transactionRequestCount < MAX_TRANSACTIONS) transactionRequests[transactionRequestCount++] = txRequest;
                else printf("Warning: Maximum transaction requests reached, skipping additional transactions\n");
            } else printf("Warning: Maximum initial client requests reached, skipping additional requests\n");
        }
    }
    fclose(clientFile);
    printf("%d clients to connect.. creating clients..\n", initialClientRequestCount);
}

// Function to handle client process
void handleClientProcess(const char* serverFifo, InitialClientRequest* request, TransactionRequest* transaction) {
    pid_t pid = getpid();
    
    char requestFifoPath[MAX_PATH_LENGTH];
    char responseFifoPath[MAX_PATH_LENGTH];
    
    request->clientPid = pid; // Update request with actual PID
    
    // Write the FIFO paths to the request and response FIFOs
    snprintf(requestFifoPath, MAX_PATH_LENGTH, "client_%d_request", pid);
    snprintf(responseFifoPath, MAX_PATH_LENGTH, "client_%d_response", pid);
    
    strncpy(request->clientRequestFifo, requestFifoPath, sizeof(request->clientRequestFifo) - 1);
    strncpy(request->clientResponseFifo, responseFifoPath, sizeof(request->clientResponseFifo) - 1);
        
    // Create the FIFOs
    if (mkfifo(requestFifoPath, 0666) == -1 && errno != EEXIST) {
        perror("Failed to create request FIFO");
        exit(1);
    }
    if (mkfifo(responseFifoPath, 0666) == -1 && errno != EEXIST) {
        perror("Failed to create response FIFO");
        unlink(requestFifoPath);  // Clean up
        exit(1);
    }
    
    // Open server FIFO for writing
    int serverFd = -1;
    int openAttempts = 0;
    const int maxOpenAttempts = 5;
    
    while (openAttempts < maxOpenAttempts) {
        if (shutdownRequested) {
            unlink(requestFifoPath);
            unlink(responseFifoPath);
            exit(1);
        }

        serverFd = open(serverFifo, O_WRONLY);
        if (serverFd == -1) {
            if (errno == EINTR) { // Check for EINTR
                if (shutdownRequested) {
                    unlink(requestFifoPath);
                    unlink(responseFifoPath);
                    exit(1);
                }
                openAttempts++;
                usleep(100000);
                continue;
            }
            perror("Failed to open server FIFO");
            unlink(requestFifoPath);
            unlink(responseFifoPath);
            exit(1);
        }
        break;
    }
    
    if (serverFd == -1) {
        unlink(requestFifoPath);
        unlink(responseFifoPath);
        exit(1);
    }
    
    ssize_t writeResult;
    
    do {
        if (shutdownRequested) {
             close(serverFd);
             unlink(requestFifoPath);
             unlink(responseFifoPath);
             exit(1);
        }
        writeResult = write(serverFd, request, sizeof(InitialClientRequest));
        if (writeResult == -1) {
            if (errno == EINTR) { // Check for EINTR
                if (shutdownRequested) {
                    close(serverFd);
                    unlink(requestFifoPath);
                    unlink(responseFifoPath);
                    exit(1);
                }
                continue;
            }
            perror("Failed to write initial request to server");
            close(serverFd);
            unlink(requestFifoPath);
            unlink(responseFifoPath);
            exit(1);
        }
    } while (writeResult == -1 && errno == EINTR);
    
    if (writeResult != sizeof(InitialClientRequest)) {
        close(serverFd); 
        unlink(requestFifoPath);
        unlink(responseFifoPath);
        exit(1);
    }
    
    close(serverFd);
    // Wait for response FIFO to be opened by the server 
    int waitAttempts = 0;
    const int maxWaitAttempts = 10;
    struct stat statbuf;
    
    while (waitAttempts < maxWaitAttempts) {
        if (shutdownRequested) {
            unlink(requestFifoPath);
            unlink(responseFifoPath);
            exit(1);
        }

        if (stat(responseFifoPath, &statbuf) == 0) { 
            int responseFd = open(responseFifoPath, O_RDONLY | O_NONBLOCK);
            if (responseFd != -1) {       
                int flags = fcntl(responseFd, F_GETFL);
                fcntl(responseFd, F_SETFL, flags & ~O_NONBLOCK);
                
                int requestFd = open(requestFifoPath, O_WRONLY); // Open request FIFO for writing
                if (requestFd == -1) {
                    close(responseFd);
                    unlink(requestFifoPath);
                    unlink(responseFifoPath);
                    exit(1);
                }
                
                InitialResponse initialResponse;
                ssize_t readResult;
                
                do {
                    if (shutdownRequested) {
                        close(requestFd);
                        close(responseFd);
                        unlink(requestFifoPath);
                        unlink(responseFifoPath);
                        exit(1);
                    }
                    readResult = read(responseFd, &initialResponse, sizeof(InitialResponse));
                    if (readResult == -1) {
                        if (errno == EINTR) { // Check for EINTR
                            if (shutdownRequested) {
                                close(requestFd);
                                close(responseFd);
                                unlink(requestFifoPath);
                                unlink(responseFifoPath);
                                exit(1);
                            }
                            continue;
                        }
                        perror("Failed to read initial response from server");
                        close(requestFd);
                        close(responseFd);
                        unlink(requestFifoPath);
                        unlink(responseFifoPath);
                        exit(1);
                    }
                } while (readResult == -1 && errno == EINTR);
                
                if (readResult != sizeof(InitialResponse)) {
                    close(requestFd);
                    close(responseFd);
                    unlink(requestFifoPath);
                    unlink(responseFifoPath);
                    exit(1);
                }
                
                if (strcmp(initialResponse.accountId, "INVALID") == 0) strncpy(transaction->accountId, initialResponse.accountId, MAX_ID_LENGTH - 1); // Server returned INVALID, no need to proceed
                else strncpy(transaction->accountId, initialResponse.accountId, MAX_ID_LENGTH - 1); // Update transaction's account ID as received from server
        
                ssize_t writeResult;
                do {
                    if (shutdownRequested) {
                        close(requestFd);
                        close(responseFd);
                        unlink(requestFifoPath);
                        unlink(responseFifoPath);
                        exit(1);
                    }
                    writeResult = write(requestFd, transaction, sizeof(TransactionRequest));
                    if (writeResult == -1) {
                        if (errno == EINTR) { // Check for EINTR
                            if (shutdownRequested) {
                                close(requestFd);
                                close(responseFd);
                                unlink(requestFifoPath);
                                unlink(responseFifoPath);
                                exit(1);
                            }
                            continue;
                        }
                        perror("Failed to write transaction request");
                        close(requestFd);
                        close(responseFd);
                        unlink(requestFifoPath);
                        unlink(responseFifoPath);
                        exit(1);
                    }
                } while (writeResult == -1 && errno == EINTR);
                
                if (writeResult != sizeof(TransactionRequest)) {
                    close(requestFd);
                    close(responseFd);
                    unlink(requestFifoPath);
                    unlink(responseFifoPath);
                    exit(1);
                }
                
                if(request->transactionType == 'D') printf("%s connected.. depositing %d credits\n", initialResponse.clientName, transaction->amount); // Print the transaction details (deposit)
                else if(request->transactionType == 'W') printf("%s connected.. withdrawing %d credits\n", initialResponse.clientName, transaction->amount); // Print the transaction details (withdraw)    
                sleep(1);
                
                TransactionResponse response;
                do {
                    if (shutdownRequested) {
                        close(requestFd);
                        close(responseFd);
                        unlink(requestFifoPath);
                        unlink(responseFifoPath);
                        exit(1);
                    }
                    readResult = read(responseFd, &response, sizeof(TransactionResponse));
                    if (readResult == -1) {
                        if (errno == EINTR) { // Check for EINTR
                            if (shutdownRequested) {
                                close(requestFd);
                                close(responseFd);
                                unlink(requestFifoPath);
                                unlink(responseFifoPath);
                                exit(1);
                            }
                            continue;
                        }
                        perror("Failed to read response from server");
                        close(requestFd);
                        close(responseFd);
                        unlink(requestFifoPath);
                        unlink(responseFifoPath);
                        exit(1);
                    }
                } while (readResult == -1 && errno == EINTR);
                
                if (readResult != sizeof(TransactionResponse)) {
                    close(requestFd);
                    close(responseFd);
                    unlink(requestFifoPath);
                    unlink(responseFifoPath);
                    exit(1);
                }
                
                if(strcmp(response.accountId, "INVALID") == 0) printf("%s something went WRONG..\n", initialResponse.clientName); // Print the response
                else printf("%s %s\n",initialResponse.clientName, response.message); // Print the response
                
                close(requestFd);
                close(responseFd);
                unlink(requestFifoPath);
                unlink(responseFifoPath);
                exit(0);
            } else close(responseFd);
        }
        
        if (shutdownRequested) {
            unlink(requestFifoPath);
            unlink(responseFifoPath);
            exit(1);
        }

        waitAttempts++;
        usleep(1000000);
    }
    unlink(requestFifoPath);
    unlink(responseFifoPath);
    exit(1);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <client_file> <server_fifo>\n", argv[0]);
        return 1;
    }

    setupClientSignalHandlers(); // Set up signal handlers

    readClientFile(argv[1]); // Read the client file
    
    if (initialClientRequestCount == 0 && transactionRequestCount == 0) { // Check if requests are read successfully
        printf("Warning: No valid requests were read from the file.\n");
        return 1;
    }
    
    int openAttempts = 0;
    const int maxOpenAttempts = 5;
    
    while (openAttempts < maxOpenAttempts && !shutdownRequested) {
        serverFifo = open(argv[2], O_WRONLY);
        if (serverFifo == -1) {
            if (errno == EINTR) { // Check for EINTR
                if (shutdownRequested) break;
                openAttempts++;
                usleep(100000);
                continue;
            }
            printf("Cannot connect %s..\n", argv[2]);
            return 1;
        }
        break;
    }
    
    if (serverFifo == -1 && !shutdownRequested) return 1;
    if (shutdownRequested) return 0;
    
    close(serverFifo);
    serverFifo = -1; 
    
    printf("Connected to the Bank...\n");
    // Create a process for each client request
    childCount = 0;
    pid_t parentPid = getpid(); // Get parent PID
    int totalCount = initialClientRequestCount; // Get total transaction count

    for (int i = 0; i < initialClientRequestCount && i < transactionRequestCount && !shutdownRequested; i++) {
        initialClientRequests[i].parentPid = parentPid;
        initialClientRequests[i].totalTransactions = totalCount;

        pid_t pid = fork(); // Fork a new process for each client
        
        if (pid == -1) {
            perror("Failed to fork");
            continue;
        } else if (pid == 0) {
            handleClientProcess(argv[2], &initialClientRequests[i], &transactionRequests[i]); // Handle client process
        } else {
            childPids[childCount++] = pid; // Store child PID
            if (fifoCount < MAX_TRANSACTIONS) { 
                snprintf(clientRequestFifos[fifoCount], MAX_PATH_LENGTH, "client_%d_request", pid);
                snprintf(clientResponseFifos[fifoCount], MAX_PATH_LENGTH, "client_%d_response", pid);
                fifoCount++;
            }
        }
    }
    
    for (int i = 0; i < childCount && !shutdownRequested; i++) { // Wait for all child processes to complete
        int status;
        waitpid(childPids[i], &status, 0);
    }
    
    if (shutdownRequested) { // Clean up if shutdown was requested
        printf("\nSignal received closing active clients\n");
        cleanupClient();
    }
    printf("exiting..\n");
    return 0;
}