#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <signal.h>

#include "common.h"

#define MAX_CLIENTS 100 // Server limit for clients
#define SHM_NAME "/bank_shared_memory" // Shared memory name
#define SEM_REQUEST "/bank_sem_request" // Request semaphore name
#define SEM_RESPONSE "/bank_sem_response" // Response semaphore name
#define SEM_ACCOUNT_PREFIX "/bank_sem_account_" // Account semaphore prefix
#define SEM_GLOBAL_ACCESS "/bank_global_access" // Global access semaphore
#define SEM_SHM_ACCESS "/bank_shm_access_lock" // Semaphore to protect SHM access

// Shared memory structure for communication between tellers and main process
typedef struct {
    int tellerPid; // Teller process ID
    char tellerType; // Teller type (D, W, N, E)
    char accountId[MAX_ID_LENGTH]; // Account ID
    int amount; // Amount
    bool success; // Success flag
    char message[MAX_MESSAGE_LENGTH]; // Message
    char clientName[MAX_ID_LENGTH]; // Client name
} SharedMemoryData;

// Global variables 
BankAccount accounts[MAX_ACCOUNTS]; // Array of accounts
int accountCount = 0; // Number of accounts
char bankName[MAX_ID_LENGTH]; // Bank name
char serverFifoName[MAX_PATH_LENGTH]; // Server FIFO name
int nextClientId = 1; // For generating new client IDs based on the number of clients
int nextClientNumber = 1; // For assigning client names sequentially

// Shared memory and semaphore globals
int shmFd; // Shared memory file descriptor
SharedMemoryData* sharedMemory; // Pointer to shared memory
sem_t* requestSemaphore; // Request semaphore
sem_t* responseSemaphore; // Response semaphore
sem_t* globalAccessSemaphore; // Protects global account array operations
sem_t* shmAccessSemaphore;    // Protects access to the shared memory structure

// Array to store account-specific semaphores
sem_t* accountSemaphores[MAX_ACCOUNTS];
char accountSemNames[MAX_ACCOUNTS][MAX_PATH_LENGTH];

volatile sig_atomic_t shutdownRequested = 0; // Global flag to indicate if we're shutting down

// Global variables for cleanup
char logFileName[MAX_PATH_LENGTH]; // Log file name
pid_t tellerPids[MAX_CLIENTS]; // Array to store teller process IDs
int tellerCount = 0; // Number of tellers
int serverFd = -1; // Server file descriptor

pid_t announcedParentPids[MAX_CLIENTS]; // Array to store parent PIDs that have been announced
int announcedCount = 0; // Number of announced parent PIDs

// Function prototypes 
void saveToLogFile(const char* filename, bool deleteZeroBalance); // Save to log file
bool isValidAccountId(const char* accountId); // Check if account ID is valid
void deposit(void* arg); // Deposit
void withdraw(void* arg); // Withdraw
void handleTransaction(SharedMemoryData* transaction); // Handle transaction
void deleteAccount(const char* accountId); // Delete account

// Function to create a teller process
pid_t Teller(void* func, void* arg_func) {
    pid_t pid = fork();
    if (pid == 0) {
        void (*tellerFunc)(void*) = (void (*)(void*))func;
        tellerFunc(arg_func);
        exit(0);
    }
    return pid;
}

// Function to wait for a teller process
void waitTeller(pid_t pid, int* status) {
    waitpid(pid, status, 0);
}

// Function to initialize shared resources
void initializeSharedResources() {
    shm_unlink(SHM_NAME); // Remove any existing shared memory object
    
    shmFd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shmFd == -1) {
        perror("shm_open failed");
        exit(1);
    }
    
    size_t shm_size = sizeof(SharedMemoryData); // Calculate the size needed
    if (ftruncate(shmFd, shm_size) == -1) { // Set size of shared memory
        perror("ftruncate failed");
        close(shmFd);
        shm_unlink(SHM_NAME);
        exit(1);
    }
    
    sharedMemory = (SharedMemoryData*)mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (sharedMemory == MAP_FAILED) {
        perror("mmap failed");
        close(shmFd);
        shm_unlink(SHM_NAME);
        exit(1);
    }
    memset(sharedMemory, 0, shm_size);
    
    // Remove any existing semaphores
    sem_unlink(SEM_REQUEST);
    sem_unlink(SEM_RESPONSE);
    sem_unlink(SEM_GLOBAL_ACCESS);
    sem_unlink(SEM_SHM_ACCESS);
    
    requestSemaphore = sem_open(SEM_REQUEST, O_CREAT | O_EXCL, 0666, 0); // Initialize request semaphore
    if (requestSemaphore == SEM_FAILED) {
        perror("sem_open request failed");
        munmap(sharedMemory, shm_size);
        close(shmFd);
        shm_unlink(SHM_NAME);
        exit(1);
    }
    
    responseSemaphore = sem_open(SEM_RESPONSE, O_CREAT | O_EXCL, 0666, 0); // Initialize response semaphore
    if (responseSemaphore == SEM_FAILED) {
        perror("sem_open response failed");
        sem_close(requestSemaphore);
        sem_unlink(SEM_REQUEST);
        munmap(sharedMemory, shm_size);
        close(shmFd);
        shm_unlink(SHM_NAME);
        exit(1);
    }
    
    globalAccessSemaphore = sem_open(SEM_GLOBAL_ACCESS, O_CREAT | O_EXCL, 0666, 1); // Initialize global access semaphore
    if (globalAccessSemaphore == SEM_FAILED) {
        perror("sem_open global access failed");
        sem_close(requestSemaphore);
        sem_close(responseSemaphore);
        sem_unlink(SEM_REQUEST);
        sem_unlink(SEM_RESPONSE);
        munmap(sharedMemory, shm_size);
        close(shmFd);
        shm_unlink(SHM_NAME);
        exit(1);
    }
    
    shmAccessSemaphore = sem_open(SEM_SHM_ACCESS, O_CREAT | O_EXCL, 0666, 1); // Initialize SHM access semaphore
    if (shmAccessSemaphore == SEM_FAILED) {
        perror("sem_open shm access failed");
        sem_close(requestSemaphore);
        sem_close(responseSemaphore);
        sem_close(globalAccessSemaphore);
        sem_unlink(SEM_REQUEST);
        sem_unlink(SEM_RESPONSE);
        sem_unlink(SEM_GLOBAL_ACCESS);
        munmap(sharedMemory, shm_size);
        close(shmFd);
        shm_unlink(SHM_NAME);
        exit(1);
    }
    
    // Initialize account semaphores (one per existing account)
    for (int i = 0; i < accountCount; i++) {
        snprintf(accountSemNames[i], MAX_PATH_LENGTH, "%s%s", SEM_ACCOUNT_PREFIX, accounts[i].id); 
        sem_unlink(accountSemNames[i]);
        
        accountSemaphores[i] = sem_open(accountSemNames[i], O_CREAT | O_EXCL, 0666, 1); // Create semaphore with initial value
        if (accountSemaphores[i] == SEM_FAILED) {
            perror("sem_open account failed");

            for (int j = 0; j < i; j++) {
                sem_close(accountSemaphores[j]);
                sem_unlink(accountSemNames[j]);
            }
            
            sem_close(requestSemaphore);
            sem_close(responseSemaphore);
            sem_close(globalAccessSemaphore);
            sem_close(shmAccessSemaphore);
            sem_unlink(SEM_REQUEST);
            sem_unlink(SEM_RESPONSE);
            sem_unlink(SEM_GLOBAL_ACCESS);
            sem_unlink(SEM_SHM_ACCESS);
            munmap(sharedMemory, shm_size);
            close(shmFd);
            shm_unlink(SHM_NAME);
            exit(1);
        }
    }
    
}

// Function to cleanup shared resources
void cleanupSharedResources() {
    if (munmap(sharedMemory, sizeof(SharedMemoryData)) == -1) perror("munmap failed"); // Unmap shared memory
    
    if (close(shmFd) == -1) perror("close failed"); // Close shared memory
    
    if (shm_unlink(SHM_NAME) == -1) perror("shm_unlink failed"); // Unlink shared memory
    
    sem_close(requestSemaphore);
    sem_close(responseSemaphore);
    sem_close(globalAccessSemaphore);
    sem_close(shmAccessSemaphore);
    sem_unlink(SEM_REQUEST);
    sem_unlink(SEM_RESPONSE);
    sem_unlink(SEM_GLOBAL_ACCESS);
    sem_unlink(SEM_SHM_ACCESS);
    
    // Clean up account semaphores
    for (int i = 0; i < accountCount; i++) {
        if (accountSemaphores[i] != NULL) {
            sem_close(accountSemaphores[i]);
            sem_unlink(accountSemNames[i]);
        }
    }   
}

// Function to find an account by ID (globalAccessSemaphore'u tutmayi unutma!)
BankAccount* findAccount(const char* accountId) {
    for (int i = 0; i < accountCount; i++) {
        if (strcmp(accounts[i].id, accountId) == 0 && accounts[i].isActive) return &accounts[i];
    }
    return NULL;
}

// Function to find an account by ID, including inactive accounts (globalAccessSemaphore'u tutmayi unutma!)
BankAccount* findAccountIncludingInactive(const char* accountId) {
    for (int i = 0; i < accountCount; i++) {
        if (strcmp(accounts[i].id, accountId) == 0) return &accounts[i];
    }
    return NULL;
}

// Function to get or create semaphore for an account
sem_t* getAccountSemaphore(const char* accountId) {
    for (int i = 0; i < accountCount; i++) {
        if (strcmp(accounts[i].id, accountId) == 0) return accountSemaphores[i];
    }
    
    char semName[MAX_PATH_LENGTH];
    snprintf(semName, MAX_PATH_LENGTH, "%s%s", SEM_ACCOUNT_PREFIX, accountId);
    
    sem_unlink(semName);
    
    sem_t* semaphore = sem_open(semName, O_CREAT | O_EXCL, 0666, 1); // Create semaphore with initial value
    if (semaphore == SEM_FAILED) {
        perror("sem_open new account failed");
        return NULL;
    }
    
    if (accountCount < MAX_ACCOUNTS) {
        strncpy(accountSemNames[accountCount], semName, MAX_PATH_LENGTH - 1);
        accountSemaphores[accountCount] = semaphore;
    }
    return semaphore;
}

// Function to delete an account by ID (globalAccessSemaphore'u tutmayi unutma!)
void deleteAccount(const char* accountId) {
    int indexToDelete = -1;
    for (int i = 0; i < accountCount; i++) {
        if (strcmp(accounts[i].id, accountId) == 0) {
            indexToDelete = i;
            break;
        }
    }
    if (indexToDelete == -1) return; // Account not found or already deleted


    if (indexToDelete < MAX_ACCOUNTS && accountSemaphores[indexToDelete] != NULL) {
        sem_close(accountSemaphores[indexToDelete]);
        sem_unlink(accountSemNames[indexToDelete]);
        accountSemaphores[indexToDelete] = NULL;
    }

    // Shift array elements
    for (int i = indexToDelete; i < accountCount - 1; i++) {
        accounts[i] = accounts[i + 1];
        if (i + 1 < MAX_ACCOUNTS) {
            accountSemaphores[i] = accountSemaphores[i + 1];
            strncpy(accountSemNames[i], accountSemNames[i + 1], MAX_PATH_LENGTH);
        }
    }

    if (accountCount > 0) {
        if (accountCount - 1 < MAX_ACCOUNTS) {
            accountSemaphores[accountCount - 1] = NULL;
            memset(accountSemNames[accountCount - 1], 0, MAX_PATH_LENGTH);
         }
         memset(&accounts[accountCount - 1], 0, sizeof(BankAccount));
     }

    accountCount--;
}

// Function to create a new bank account with a unique ID (globalAccessSemaphore'u tutmayi unutma!)
BankAccount* createNewAccount() {
    sem_wait(globalAccessSemaphore);
    
    if (accountCount >= MAX_ACCOUNTS) {
        printf("Error: Maximum number of accounts reached.\n");
        sem_post(globalAccessSemaphore);
        return NULL;
    }
    
    char newId[MAX_ID_LENGTH];
    int numericPart = 1;

    while (true) {
        snprintf(newId, MAX_ID_LENGTH, "BankID_%02d", numericPart);
        if (findAccountIncludingInactive(newId) == NULL) break;
        numericPart++;
    }
    
    BankAccount* newAccount = &accounts[accountCount];
    strncpy(newAccount->id, newId, MAX_ID_LENGTH - 1);
    newAccount->id[MAX_ID_LENGTH - 1] = '\0';
    newAccount->balance = 0;
    newAccount->isActive = true;
    newAccount->transactionCount = 0;
    snprintf(newAccount->clientName, MAX_ID_LENGTH, "Client%02d", nextClientNumber++);
    newAccount->clientName[MAX_ID_LENGTH - 1] = '\0';

    sem_t* accountSem = getAccountSemaphore(newId);
    if (accountSem == NULL) {
        sem_post(globalAccessSemaphore);
        return NULL;
    }
    accountCount++;
    sem_post(globalAccessSemaphore);
    return newAccount;
}

// Deposit teller function - runs in a teller process
void deposit(void* arg) {
    InitialClientRequest* initialRequest = (InitialClientRequest*)arg;
    pid_t tellerPid = getpid();
    int responseFd = -1;
    int requestFd = -1;
    char clientName[MAX_ID_LENGTH] = {0};
    char accountId[MAX_ID_LENGTH] = {0};
    InitialResponse initialResponse;
    TransactionRequest txRequest;
    TransactionResponse txResponse;
    ssize_t writeResult, readResult;
    bool transactionSuccess = false;
    char requestType;

    // Determine request type
    if (strcmp(initialRequest->accountId, "N") == 0) requestType = 'N';
    else requestType = 'E';

    // Acquire SHM access for initial check/request
    if (sem_wait(shmAccessSemaphore) == -1) {
        perror("Teller failed to wait on shmAccessSemaphore");
        free(initialRequest);
        return;
    }

    // Prepare request for main process
    sharedMemory->tellerPid = tellerPid;
    sharedMemory->tellerType = requestType;
    strncpy(sharedMemory->accountId, initialRequest->accountId, MAX_ID_LENGTH - 1);
    sharedMemory->accountId[MAX_ID_LENGTH - 1] = '\0';

    sem_post(requestSemaphore); // Signal main process

    sem_wait(responseSemaphore); // Wait for response

    strncpy(accountId, sharedMemory->accountId, MAX_ID_LENGTH - 1);
    accountId[MAX_ID_LENGTH - 1] = '\0';
    strncpy(clientName, sharedMemory->clientName, MAX_ID_LENGTH - 1);
    clientName[MAX_ID_LENGTH - 1] = '\0';
    bool initialSuccess = sharedMemory->success;

    sem_post(shmAccessSemaphore); // Release SHM access after initial check/request


    // Send Initial Response and Get Transaction Request
    printf("-- Teller PID%d is active serving %s..", tellerPid, clientName);
    if (initialSuccess && requestType == 'E') printf(" Welcome back %s\n", clientName);
    else printf("\n");
    sleep(1);

    strncpy(initialResponse.clientName, clientName, MAX_ID_LENGTH - 1);
    initialResponse.clientName[MAX_ID_LENGTH - 1] = '\0';
    strncpy(initialResponse.accountId, accountId, MAX_ID_LENGTH - 1);
    initialResponse.accountId[MAX_ID_LENGTH - 1] = '\0';

    responseFd = open(initialRequest->clientResponseFifo, O_WRONLY);
    if (responseFd == -1) {
        printf("Teller PID%d: Connection lost with the client..\n", tellerPid);
        free(initialRequest);
        return;
    }

    writeResult = write(responseFd, &initialResponse, sizeof(InitialResponse));
    if (writeResult != sizeof(InitialResponse)) {
        printf("Teller PID%d: Connection lost with the client..\n", tellerPid);
        close(responseFd);
        free(initialRequest);
        return;
    }

    requestFd = open(initialRequest->clientRequestFifo, O_RDONLY);
    if (requestFd == -1) {
        printf("Teller PID%d: Connection lost with the client..\n", tellerPid);
        close(responseFd);
        free(initialRequest);
        return;
    }

    readResult = read(requestFd, &txRequest, sizeof(TransactionRequest));
    if (readResult != sizeof(TransactionRequest)) {
        printf("Teller PID%d: Connection lost with the client..\n", tellerPid);
        close(responseFd);
        close(requestFd);
        free(initialRequest);
        return;
    }

    strncpy(txResponse.accountId, accountId, MAX_ID_LENGTH - 1);
    txResponse.accountId[MAX_ID_LENGTH - 1] = '\0';

    if (strcmp(accountId, "INVALID") == 0) {
        snprintf(txResponse.message, MAX_MESSAGE_LENGTH, "something went WRONG..");
        transactionSuccess = false;
    } else {
        if (sem_wait(shmAccessSemaphore) == -1) {
            perror("Teller failed to wait on shmAccessSemaphore for transaction");
            close(requestFd);
            close(responseFd);
            free(initialRequest);
            return;
        }

        sharedMemory->tellerPid = tellerPid;
        sharedMemory->tellerType = 'D';
        strncpy(sharedMemory->accountId, accountId, MAX_ID_LENGTH - 1);
        sharedMemory->amount = txRequest.amount;

        sem_post(requestSemaphore); // Signal main process

        sem_wait(responseSemaphore); // Wait for response

        strncpy(txResponse.accountId, sharedMemory->accountId, MAX_ID_LENGTH - 1);
        txResponse.accountId[MAX_ID_LENGTH - 1] = '\0';
        strncpy(txResponse.message, sharedMemory->message, MAX_MESSAGE_LENGTH - 1);
        txResponse.message[MAX_MESSAGE_LENGTH - 1] = '\0';
        transactionSuccess = sharedMemory->success;

        sem_post(shmAccessSemaphore); // Release SHM access after transaction processing
    }


    if (!transactionSuccess || strcmp(txResponse.accountId, "INVALID") == 0) printf("%s deposited %d credits.. operation not permitted\n", clientName, txRequest.amount);
    else printf("%s deposited %d credits... updating log\n", clientName, txRequest.amount);


    writeResult = write(responseFd, &txResponse, sizeof(TransactionResponse));
    if (writeResult != sizeof(TransactionResponse)) printf("Teller PID%d: Connection lost with the client..\n", tellerPid);

    // Clean up
    close(requestFd);
    close(responseFd);

    sem_close(requestSemaphore);
    sem_close(responseSemaphore);
    sem_close(shmAccessSemaphore);
    sem_close(globalAccessSemaphore);

    free(initialRequest);
}

// Withdraw teller function - runs in a teller process
void withdraw(void* arg) {
    InitialClientRequest* initialRequest = (InitialClientRequest*)arg;
    pid_t tellerPid = getpid();
    int responseFd = -1;
    int requestFd = -1;
    char clientName[MAX_ID_LENGTH] = {0};
    char accountId[MAX_ID_LENGTH] = {0};
    InitialResponse initialResponse;
    TransactionRequest txRequest = {0};
    TransactionResponse txResponse;
    ssize_t writeResult, readResult;
    bool transactionSuccess = false;
    bool initialSuccess = false;

    // Acquire SHM access for initial check/request
    if (sem_wait(shmAccessSemaphore) == -1) {
        perror("Teller failed to wait on shmAccessSemaphore");
        free(initialRequest);
        return;
    }

    // Prepare request for main process (always type 'E' for withdraw)
    sharedMemory->tellerPid = tellerPid;
    sharedMemory->tellerType = 'E'; // Check existing account
    strncpy(sharedMemory->accountId, initialRequest->accountId, MAX_ID_LENGTH - 1);
    sharedMemory->accountId[MAX_ID_LENGTH - 1] = '\0';

    sem_post(requestSemaphore); // Signal main process

    sem_wait(responseSemaphore); // Wait for response

    strncpy(accountId, sharedMemory->accountId, MAX_ID_LENGTH - 1);
    accountId[MAX_ID_LENGTH - 1] = '\0';
    strncpy(clientName, sharedMemory->clientName, MAX_ID_LENGTH - 1);
    clientName[MAX_ID_LENGTH - 1] = '\0';
    initialSuccess = sharedMemory->success;

    sem_post(shmAccessSemaphore); // Release SHM access after initial check


    // Send Initial Response and Get Transaction Request
    printf("-- Teller PID%d is active serving %s..", tellerPid, clientName);
    if (initialSuccess) printf(" Welcome back %s\n", clientName);
    else printf("\n");
    sleep(1);

    // Prepare initial response with determined account ID and client name
    strncpy(initialResponse.clientName, clientName, MAX_ID_LENGTH - 1);
    initialResponse.clientName[MAX_ID_LENGTH - 1] = '\0';
    strncpy(initialResponse.accountId, accountId, MAX_ID_LENGTH - 1);
    initialResponse.accountId[MAX_ID_LENGTH - 1] = '\0';

    responseFd = open(initialRequest->clientResponseFifo, O_WRONLY);
    if (responseFd == -1) {
        printf("Teller PID%d: Connection lost with the client..\n", tellerPid);
        free(initialRequest);
        return;
    }

    // Send initial response
    writeResult = write(responseFd, &initialResponse, sizeof(InitialResponse));
    if (writeResult != sizeof(InitialResponse)) {
        printf("Teller PID%d: Connection lost with the client..\n", tellerPid);
        close(responseFd);
        free(initialRequest);
        return;
    }

    requestFd = open(initialRequest->clientRequestFifo, O_RDONLY);
    if (requestFd == -1) {
        printf("Teller PID%d: Connection lost with the client..\n", tellerPid);
        close(responseFd);
        free(initialRequest);
        return;
    }

    readResult = read(requestFd, &txRequest, sizeof(TransactionRequest));
    if (readResult != sizeof(TransactionRequest)) {
        printf("Teller PID%d: Connection lost with the client..\n", tellerPid);
        close(responseFd);
        close(requestFd);
        free(initialRequest);
        return;
    }

    strncpy(txResponse.accountId, accountId, MAX_ID_LENGTH - 1);
    txResponse.accountId[MAX_ID_LENGTH - 1] = '\0';

    if (strcmp(accountId, "INVALID") == 0) { 
        snprintf(txResponse.message, MAX_MESSAGE_LENGTH, "something went WRONG..");
        transactionSuccess = false;
    } else {
        if (sem_wait(shmAccessSemaphore) == -1) { // Acquire SHM access for transaction processing
            perror("Teller failed to wait on shmAccessSemaphore for transaction");
            close(requestFd);
            close(responseFd);
            free(initialRequest);
            return;
        }

        sharedMemory->tellerPid = tellerPid;
        sharedMemory->tellerType = 'W';
        strncpy(sharedMemory->accountId, accountId, MAX_ID_LENGTH - 1);
        sharedMemory->amount = txRequest.amount;

        sem_post(requestSemaphore); // Signal main process

        sem_wait(responseSemaphore); // Wait for response

        strncpy(txResponse.accountId, sharedMemory->accountId, MAX_ID_LENGTH - 1);
        txResponse.accountId[MAX_ID_LENGTH - 1] = '\0';
        strncpy(txResponse.message, sharedMemory->message, MAX_MESSAGE_LENGTH - 1);
        txResponse.message[MAX_MESSAGE_LENGTH - 1] = '\0';
        transactionSuccess = sharedMemory->success;

        sem_post(shmAccessSemaphore); // Release SHM access after transaction processing
    }

    if (!transactionSuccess || strcmp(txResponse.accountId, "INVALID") == 0) printf("%s withdraws %d credits.. operation not permitted\n", clientName, txRequest.amount);
    else {
        printf("%s withdraws %d credits... updating log ", clientName, txRequest.amount);
        if (strcmp(txResponse.message, "account closed") == 0) printf("Bye %s\n", clientName);
        else printf("\n");
    }

    writeResult = write(responseFd, &txResponse, sizeof(TransactionResponse));
    if (writeResult != sizeof(TransactionResponse)) printf("Teller PID%d: Connection lost with the client..\n", tellerPid);

    close(requestFd);
    close(responseFd);

    sem_close(requestSemaphore);
    sem_close(responseSemaphore);
    sem_close(shmAccessSemaphore);
    sem_close(globalAccessSemaphore);

    free(initialRequest);
}

// Function to handle transaction requests from tellers
void handleTransaction(SharedMemoryData* transaction) {    
    bool success = false;
    char message[MAX_MESSAGE_LENGTH] = {0};
    sem_t* accountSem = NULL;
    char determinedClientName[MAX_ID_LENGTH] = {0};
    char determinedAccountId[MAX_ID_LENGTH] = {0};

    memset(transaction->clientName, 0, MAX_ID_LENGTH);
    
    switch (transaction->tellerType) {
        case 'N': { // Handle new account
            BankAccount* newAccount = createNewAccount();
            if (newAccount) {
                strncpy(determinedAccountId, newAccount->id, MAX_ID_LENGTH - 1);
                strncpy(determinedClientName, newAccount->clientName, MAX_ID_LENGTH - 1);
                snprintf(message, MAX_MESSAGE_LENGTH, "New account created: %s", newAccount->id);
                success = true;
            } else {
                strncpy(determinedAccountId, "INVALID", MAX_ID_LENGTH - 1);
                snprintf(determinedClientName, MAX_ID_LENGTH, "Client%02d", nextClientNumber++);
                snprintf(message, MAX_MESSAGE_LENGTH, "Failed to create new account");
                success = false;
            }
            break;
        }
        
        case 'E': { // Handle existing account
            sem_wait(globalAccessSemaphore);
            
            if (!isValidAccountId(transaction->accountId)) {
                strncpy(determinedAccountId, "INVALID", MAX_ID_LENGTH - 1);
                snprintf(determinedClientName, MAX_ID_LENGTH, "Client%02d", nextClientNumber++);
                snprintf(message, MAX_MESSAGE_LENGTH, "something went WRONG..");
                success = false;
                sem_post(globalAccessSemaphore);
                break;
            }
            BankAccount* account = findAccount(transaction->accountId);
            if (account) {
                strncpy(determinedAccountId, account->id, MAX_ID_LENGTH - 1);
                strncpy(determinedClientName, account->clientName, MAX_ID_LENGTH - 1);
                snprintf(message, MAX_MESSAGE_LENGTH, "Account exists: %s", account->id);
                success = true;
            } else {
                strncpy(determinedAccountId, "INVALID", MAX_ID_LENGTH - 1);
                snprintf(determinedClientName, MAX_ID_LENGTH, "Client%02d", nextClientNumber++);
                snprintf(message, MAX_MESSAGE_LENGTH, "Account not found: %s", transaction->accountId);
                success = false;
            }            
            sem_post(globalAccessSemaphore);
            break;
        }
        
        case 'D': {  // Handle deposit transaction
            char targetAccountId[MAX_ID_LENGTH];
            strncpy(targetAccountId, transaction->accountId, MAX_ID_LENGTH);

            if (!isValidAccountId(targetAccountId)) {
                snprintf(message, MAX_MESSAGE_LENGTH, "something went WRONG..");
                strncpy(transaction->accountId, "INVALID", MAX_ID_LENGTH);
                success = false;
                break;
            }

            if (transaction->amount <= 0) {
                snprintf(message, MAX_MESSAGE_LENGTH, "something went WRONG..");
                success = false;
                break;
            }
            
            sem_wait(globalAccessSemaphore);
            BankAccount* account = findAccount(targetAccountId);
            
            if (account) {
                accountSem = getAccountSemaphore(account->id);
                sem_post(globalAccessSemaphore);
                
                if (accountSem == NULL) {
                    snprintf(message, MAX_MESSAGE_LENGTH, "Failed to get account semaphore");
                    success = false;
                    break;
                }
                
                sem_wait(accountSem);
                
                if (!account->isActive) {
                    snprintf(message, MAX_MESSAGE_LENGTH, "Account %s is inactive and cannot be used", targetAccountId);
                    success = false;
                    sem_post(accountSem);
                    break;
                }
                
                account->balance += transaction->amount;
                
                if (account->transactionCount < MAX_TRANSACTIONS) {
                    char transactionStr[MAX_TRANSACTION_LENGTH];
                    snprintf(transactionStr, MAX_TRANSACTION_LENGTH, "Deposit: +%d", transaction->amount);
                    strncpy(account->transactionHistory[account->transactionCount++], 
                            transactionStr, MAX_TRANSACTION_LENGTH - 1);
                }
                
                snprintf(message, MAX_MESSAGE_LENGTH, "served.. %s", account->id);
                success = true;
                
                sem_post(accountSem);
            } else {
                sem_post(globalAccessSemaphore);
                snprintf(message, MAX_MESSAGE_LENGTH, "Account not found: %s", targetAccountId);
                strncpy(transaction->accountId, "INVALID", MAX_ID_LENGTH);
                success = false;
            }
            break;
        }
        
        case 'W': { // Handle withdrawal transaction
            char targetAccountId[MAX_ID_LENGTH];
            strncpy(targetAccountId, transaction->accountId, MAX_ID_LENGTH);

            if (!isValidAccountId(targetAccountId)) {
                snprintf(message, MAX_MESSAGE_LENGTH, "something went WRONG..");
                strncpy(transaction->accountId, "INVALID", MAX_ID_LENGTH);
                success = false;
                break;
            }

            if (transaction->amount <= 0) {
                snprintf(message, MAX_MESSAGE_LENGTH, "something went WRONG..");
                success = false;
                break;
            }
            
            sem_wait(globalAccessSemaphore);
            BankAccount* account = findAccount(targetAccountId);
            
            if (account) {
                accountSem = getAccountSemaphore(account->id);
                sem_post(globalAccessSemaphore);
                
                if (accountSem == NULL) {
                    snprintf(message, MAX_MESSAGE_LENGTH, "Failed to get account semaphore");
                    success = false;
                    break;
                }
                
                sem_wait(accountSem);
                
                if (!account->isActive) {
                    snprintf(message, MAX_MESSAGE_LENGTH, "Account %s is inactive and cannot be used", targetAccountId);
                    success = false;
                    sem_post(accountSem);
                    break;
                }
                
                if (account->balance >= transaction->amount) {
                    account->balance -= transaction->amount;
                    
                    if (account->transactionCount < MAX_TRANSACTIONS) {
                        char transactionStr[MAX_TRANSACTION_LENGTH];
                        snprintf(transactionStr, MAX_TRANSACTION_LENGTH, "Withdrawal: -%d", transaction->amount);
                        strncpy(account->transactionHistory[account->transactionCount++], transactionStr, MAX_TRANSACTION_LENGTH - 1);
                    }
                    
                    if (account->balance == 0) {
                        account->isActive = false;
                        snprintf(message, MAX_MESSAGE_LENGTH, "account closed");
                    } else {
                        snprintf(message, MAX_MESSAGE_LENGTH, "served.. %s", account->id);
                    }
                    success = true;
                } else {
                    snprintf(message, MAX_MESSAGE_LENGTH, "something went WRONG..");
                    success = false;
                }
                sem_post(accountSem);
            } else {
                sem_post(globalAccessSemaphore);
                snprintf(message, MAX_MESSAGE_LENGTH, "Account not found: %s", targetAccountId);
                strncpy(transaction->accountId, "INVALID", MAX_ID_LENGTH);
                success = false;
            }
            break;
        }
        
        default: // Invalid transaction type
            strncpy(determinedAccountId, transaction->accountId, MAX_ID_LENGTH);
            snprintf(determinedClientName, MAX_ID_LENGTH, "ClientUnknown");
            snprintf(message, MAX_MESSAGE_LENGTH, "something went WRONG..");
            success = false;
            break;
    }
    
    transaction->success = success; // Set response in shared memory
    strncpy(transaction->message, message, MAX_MESSAGE_LENGTH - 1);
    if (transaction->tellerType == 'N' || transaction->tellerType == 'E') {
        strncpy(transaction->accountId, determinedAccountId, MAX_ID_LENGTH - 1);
        strncpy(transaction->clientName, determinedClientName, MAX_ID_LENGTH - 1);
    }
    
    if (transaction->tellerType == 'W' || transaction->tellerType == 'D') {
        if (success) {
            sem_wait(globalAccessSemaphore);
            saveToLogFile(logFileName, false);
            sem_post(globalAccessSemaphore);
        }  
    }
}

// Function to parse log file to generate bank database
void parseLogFile(const char* filename) {
    FILE* file = fopen(filename, "r");
    int highestNumericId = 0;

    if (!file) {
        printf("No previous logs.. Creating the bank database\n");
        return;
    }
    printf("Previous logs found.. Loading the bank database\n");

    char line[256];
    while (fgets(line, sizeof(line), file) && accountCount < MAX_ACCOUNTS) {
        if (line[0] == '\n' || line[0] == '\r') continue; // Skip empty lines
        
        if (strncmp(line, "## ", 3) == 0) continue; // Skip end of log
        
        if (line[0] == '#' && line[1] == ' ' && 
            (strstr(line, "Adabank") || strstr(line, "Log file") || 
             strstr(line, "updated") || !isalnum(line[2]))) {
            continue; // Skip header comments that start with # and don't look like account IDs
        }
        
        char* rest = line;
        bool isActive = true;
        
        if (line[0] == '#') { // Check if account is inactive
            isActive = false;
            rest++;
            
            while (*rest == ' ') rest++;
        }
        
        char* token = strtok(rest, " \t\n\r");
        if (!token) continue;
        
        BankAccount* account = &accounts[accountCount];
        strncpy(account->id, token, MAX_ID_LENGTH - 1);
        account->id[MAX_ID_LENGTH - 1] = '\0';
        account->isActive = isActive;
        account->transactionCount = 0;
        account->balance = 0;
        memset(account->clientName, 0, MAX_ID_LENGTH);

        if (strncmp(account->id, "BankID_", 7) == 0) {
            const char* numPartStr = account->id + 7;
            bool isNumeric = true;
            if (*numPartStr == '\0') {
                isNumeric = false;
            } else {
                const char *p = numPartStr;
                while (*p) {
                    if (!isdigit(*p)) {
                        isNumeric = false;
                        break;
                    }
                    p++;
                }
            }
            if (isNumeric) {
                int numPart = atoi(numPartStr);
                snprintf(account->clientName, MAX_ID_LENGTH, "Client%02d", numPart);
                if (numPart > highestNumericId) {
                    highestNumericId = numPart;
                }
            } else {
                snprintf(account->clientName, MAX_ID_LENGTH, "Client_%s", account->id);
            }
        } else {
            snprintf(account->clientName, MAX_ID_LENGTH, "Client_%s", account->id);
        }
        
        int balance = 0;
        char transactionStr[MAX_TRANSACTION_LENGTH];
        
        while ((token = strtok(NULL, " \t\n\r")) != NULL) { // Process transactions in the log file
            if (strcmp(token, "D") == 0) {
                token = strtok(NULL, " \t\n\r");
                if (!token) break;
                
                int amount = atoi(token);
                balance += amount;
                
                if (account->transactionCount < MAX_TRANSACTIONS) {
                    snprintf(transactionStr, MAX_TRANSACTION_LENGTH, "Deposit: +%d", amount);
                    strncpy(account->transactionHistory[account->transactionCount], transactionStr, MAX_TRANSACTION_LENGTH - 1);
                    account->transactionHistory[account->transactionCount][MAX_TRANSACTION_LENGTH - 1] = '\0';
                    account->transactionCount++;
                }
            }
            else if (strcmp(token, "W") == 0) {
                token = strtok(NULL, " \t\n\r");
                if (!token) break;
                
                int amount = atoi(token);
                balance -= amount;
                
                if (account->transactionCount < MAX_TRANSACTIONS) {
                    snprintf(transactionStr, MAX_TRANSACTION_LENGTH, "Withdrawal: -%d", amount);
                    strncpy(account->transactionHistory[account->transactionCount], transactionStr, MAX_TRANSACTION_LENGTH - 1);
                    account->transactionHistory[account->transactionCount][MAX_TRANSACTION_LENGTH - 1] = '\0';
                    account->transactionCount++;
                }
            }
            else if (isdigit(token[0]) || (token[0] == '-' && isdigit(token[1]))) account->balance = atoi(token);
        }
        accountCount++;
    }
    fclose(file);
    nextClientNumber = highestNumericId + 1;
}

// Save accounts to log file
void saveToLogFile(const char* filename, bool deleteZeroBalance) {
    FILE* file = fopen(filename, "w");
    if (!file) {
        printf("Error opening file for writing: %s\n", filename);
        return;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timeStr[20];
    strftime(timeStr, 20, "%H:%M %B %d %Y", tm_info);

    fprintf(file, "# %s Log file updated @%s\n", bankName, timeStr);

    if (deleteZeroBalance) { // Delete active zero balance accounts before writing to log file
        for (int i = accountCount - 1; i >= 0; i--) {
            if (accounts[i].isActive && accounts[i].balance == 0) {
                char idToDelete[MAX_ID_LENGTH];
                strncpy(idToDelete, accounts[i].id, MAX_ID_LENGTH);
                idToDelete[MAX_ID_LENGTH - 1] = '\0';
                deleteAccount(idToDelete);
            }
        }
    }

    for (int i = 0; i < accountCount; i++) {
        BankAccount* account = &accounts[i];

        if (account->isActive && account->balance == 0) continue;

        if (!account->isActive) fprintf(file, "# ");

        fprintf(file, "%s", account->id);

        for (int j = 0; j < account->transactionCount; j++) {
            char transType = 'D';
            int amount = 0;

            if (strstr(account->transactionHistory[j], "Deposit:")) {
                transType = 'D';
                sscanf(account->transactionHistory[j], "Deposit: +%d", &amount);
            } else if (strstr(account->transactionHistory[j], "Withdrawal:")) {
                transType = 'W';
                sscanf(account->transactionHistory[j], "Withdrawal: -%d", &amount);
            }

            fprintf(file, " %c %d", transType, amount);
        }

        fprintf(file, " %d\n", account->balance);
    }

    fprintf(file, "## end of log.\n");
    fclose(file);
}

// Create server FIFO
void openServerFifo() {
    unlink(serverFifoName);
    if (mkfifo(serverFifoName, 0666) == -1) {
        perror("mkfifo failed");
        exit(1);
    }
}

// Function for cleaning up server resources
void cleanupServer() {
    printf("Removing ServerFIFO.. Updating log file..\n");

    // Close server FIFO
    if (serverFd != -1) {
        close(serverFd);
        unlink(serverFifoName);
    }
    
    saveToLogFile(logFileName, true); // Save account information to log file
    
    cleanupSharedResources(); // Clean up shared memory and semaphores
    
    for (int i = 0; i < tellerCount; i++) {
        if (kill(tellerPids[i], SIGTERM) == -1) {
            if (errno != ESRCH) perror("Failed to terminate teller process");
        }
    }
    
    for (int i = 0; i < tellerCount; i++) { // Wait for all teller processes to terminate
        int status;
        waitpid(tellerPids[i], &status, 0);
    }
}

// Function to handle termination signals
void serverSignalHandler(int signum) {
   printf("\nSignal received closing active Tellers\n");
    shutdownRequested = 1;    
}

// Function to set up signal handlers
void setupServerSignalHandlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = serverSignalHandler;
    
    sigaction(SIGINT, &sa, NULL);   // ctrl+c
    sigaction(SIGTERM, &sa, NULL);  // kill signal
    sigaction(SIGHUP, &sa, NULL);   // terminal closed
    
}

// Check if the account ID has the correct format
bool isValidAccountId(const char* accountId) {
    if (strcmp(accountId, "N") == 0) return true;
    if (strncmp(accountId, "BankID_", 7) != 0) return false;
    const char* numPart = accountId + 7;
    if (*numPart == '\0') return false;
    
    while (*numPart) {
        if (!isdigit(*numPart)) return false;
        numPart++;
    }
    return true;
}

// Function to find the index of an account in the accounts array
int findAccountIndex(const char* accountId) {
    for (int i = 0; i < accountCount; i++) {
        if (strcmp(accounts[i].id, accountId) == 0) return i;
    }
    return -1;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <bankName> <serverFifoName>\n", argv[0]);
        return 1;
    }
    
    setupServerSignalHandlers(); // Set up signal handlers
    
    strncpy(bankName, argv[1], sizeof(bankName) - 1);
    bankName[sizeof(bankName) - 1] = '\0';
    
    strncpy(serverFifoName, argv[2], sizeof(serverFifoName) - 1);
    serverFifoName[sizeof(serverFifoName) - 1] = '\0';
    
    snprintf(logFileName, MAX_PATH_LENGTH, "%s.bankLog", bankName); // Log file name determined based on bank name

    printf("%s is active..\n", bankName);
    
    parseLogFile(logFileName); // Parse existing log file if it exists
    
    initializeSharedResources(); // Initialize shared memory and semaphores
    
    openServerFifo(); // Create server FIFO
        
    int openAttempts = 0;
    const int maxOpenAttempts = 3;
    
    while (openAttempts < maxOpenAttempts && !shutdownRequested) {
        serverFd = open(serverFifoName, O_RDONLY | O_NONBLOCK);
        if (serverFd == -1) {
            if (errno == EINTR) { // Check EINTR
                if (shutdownRequested) break;
                openAttempts++;
                continue;
            } else {
                perror("Failed to open server FIFO for reading");
                cleanupSharedResources();
                return 1;
            }
        }
        break;
    }
    
    if (serverFd == -1 && !shutdownRequested) {
        cleanupSharedResources();
        return 1;
    }
    
    if (shutdownRequested) {
        cleanupServer();
        return 0;
    }
    
    int flags = fcntl(serverFd, F_GETFL);
    fcntl(serverFd, F_SETFL, flags | O_NONBLOCK);
    
    fd_set readFds;
    struct timeval tv;
    bool waitingForClient = true;
    
    while (!shutdownRequested) { // Main server loop
        FD_ZERO(&readFds);
        FD_SET(serverFd, &readFds);
        
        if (waitingForClient) {
            printf("Waiting for clients @%s...\n", serverFifoName);
            waitingForClient = false;
        }
        
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        int selectResult = select(serverFd + 1, &readFds, NULL, NULL, &tv); // Check for available data on server FIFO
        
        if (selectResult == -1) {
            if (errno == EINTR) continue;
            perror("Select failed");
            break;
        }
        
        if (selectResult > 0 && FD_ISSET(serverFd, &readFds)) {
            InitialClientRequest request;
            ssize_t bytesRead = read(serverFd, &request, sizeof(InitialClientRequest));
            
            if (bytesRead == -1) {
                if (errno == EINTR) continue;
                else if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                else perror("Error reading from server FIFO");
                continue;
            } else if (bytesRead == 0) {
                close(serverFd);
                int reopenAttempts = 0;
                const int maxReopenAttempts = 3;
                bool reopened = false;
                
                while (reopenAttempts < maxReopenAttempts && !shutdownRequested) {
                    serverFd = open(serverFifoName, O_RDONLY | O_NONBLOCK);
                    if (serverFd == -1) {
                        if (errno == EINTR) { // Check EINTR
                            if (shutdownRequested) break;
                            reopenAttempts++;
                            continue;
                        }
                        perror("Failed to reopen server FIFO");
                        shutdownRequested = 1;
                        break;
                    }
                    reopened = true;
                    break;
                }
                
                if (!reopened && !shutdownRequested) shutdownRequested = 1;
                
                continue;
            } else if (bytesRead != sizeof(InitialClientRequest)) continue;
                        
            bool alreadyAnnounced = false;
            for (int j = 0; j < announcedCount; j++) {
                if (announcedParentPids[j] == request.parentPid) {
                    alreadyAnnounced = true;
                    break;
                }
            }

            if (!alreadyAnnounced) { // Check if this parent PID has already been announced
                if (announcedCount < MAX_CLIENTS) {
                    announcedParentPids[announcedCount++] = request.parentPid;
                    printf("Received %d clients from PIDClient%d..\n", request.totalTransactions, request.parentPid);
                    fflush(stdout);
                }
            }

            InitialClientRequest* requestCopy = malloc(sizeof(InitialClientRequest));
            if (!requestCopy) {
                perror("Failed to allocate memory for request");
                continue;
            }
            memcpy(requestCopy, &request, sizeof(InitialClientRequest));
            
            pid_t tellerPid;
            if (request.transactionType == 'D') {
                tellerPid = Teller(deposit, requestCopy);
            } else if (request.transactionType == 'W') {
                tellerPid = Teller(withdraw, requestCopy);
            } else {
                printf("Error: Invalid transaction type: %c\n", request.transactionType);
                free(requestCopy);
                continue;
            }
            
            if (tellerPid > 0) {                
                if (tellerCount < MAX_CLIENTS) {
                    tellerPids[tellerCount++] = tellerPid;
                }
                free(requestCopy);
            } else {
                printf("Error creating teller process\n");
                free(requestCopy);
            }
        }
        
        if (sem_trywait(requestSemaphore) == 0) {
            handleTransaction(sharedMemory);
            sem_post(responseSemaphore);
        }
        
        for (int i = 0; i < tellerCount; i++) {
            int status;
            pid_t result = waitpid(tellerPids[i], &status, WNOHANG);
            
            if (result > 0) {
                for (int j = i; j < tellerCount - 1; j++) tellerPids[j] = tellerPids[j + 1];
                tellerCount--;
                i--;
                if (tellerCount == 0) waitingForClient = true;
            }
        }
    }
    
    cleanupServer(); // Clean up resources
    
    printf("%s says \"Bye\"..\n", bankName);
    return 0;
}