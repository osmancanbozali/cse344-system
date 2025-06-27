#ifndef COMMON_H
#define COMMON_H

// Include standard libraries needed for the definitions below
#include <stdbool.h> // For bool in BankAccount
#include <sys/types.h> // For pid_t

// Constants used by both client and server
#define MAX_ID_LENGTH 20
#define MAX_TRANSACTIONS 100         // Max transactions per account history / client file lines
#define MAX_TRANSACTION_LENGTH 100 // Max length of transaction string in history
#define MAX_ACCOUNTS 100           // Max accounts the server can handle
#define MAX_MESSAGE_LENGTH 256     // Max length for general messages
#define MAX_PATH_LENGTH 256        // Max length for file/FIFO paths

// Structures shared between client and server

// Represents a bank account (defined here as client needs MAX_ID_LENGTH etc.)
// Server manages the actual instances of this struct.
typedef struct {
    char id[MAX_ID_LENGTH];
    int balance;
    bool isActive;
    int transactionCount;
    char transactionHistory[MAX_TRANSACTIONS][MAX_TRANSACTION_LENGTH];
    char clientName[MAX_ID_LENGTH];
} BankAccount;

// Initial request sent from client to server FIFO
typedef struct {
    char accountId[MAX_ID_LENGTH]; // 'N' for new account, or existing BankID_xx
    char transactionType;       // 'D' for deposit, 'W' for withdrawal
    int clientPid;              // PID of the specific client process handling this transaction
    char clientRequestFifo[50];  // Path to the client's request FIFO
    char clientResponseFifo[50]; // Path to the client's response FIFO
    int parentPid;              // PID of the main client process (parent of handlers)
    int totalTransactions;      // Total transactions listed in the client's input file
} InitialClientRequest;

// Initial response sent from server (teller) to client's response FIFO
typedef struct {
    char clientName[MAX_ID_LENGTH]; // Assigned or retrieved client name
    char accountId[MAX_ID_LENGTH];  // Assigned or validated account ID (or "INVALID")
} InitialResponse;

// Transaction details sent from client to server (teller) via client's request FIFO
typedef struct {
    char accountId[MAX_ID_LENGTH]; // Account ID for the transaction (should match initial response)
    int amount;                 // Amount to deposit or withdraw
} TransactionRequest;

// Final response sent from server (teller) to client's response FIFO
typedef struct {
    char accountId[MAX_ID_LENGTH];    // Account ID (may be "INVALID" on failure)
    char message[MAX_MESSAGE_LENGTH]; // Result message (e.g., "account closed", error, or account ID)
} TransactionResponse;


#endif // COMMON_H 