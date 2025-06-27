// Include necessary libraries
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <time.h> 

extern int errno;

// Function to write a formatted message with quotes around a name
void writeMessage(int fd, const char* prefix, const char* name, const char* suffix) {
    if (prefix) write(fd, prefix, strlen(prefix));
    if (name) {
        write(fd, "\"", 1);
        write(fd, name, strlen(name));
        write(fd, "\"", 1);
    }
    if (suffix) write(fd, suffix, strlen(suffix));
}

// Function that returns the current timestamp
char* getTimestamp() {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    static char timestamp[22];
    strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]", tm_info);
    return timestamp;
}

// Function that logs the operation to the log.txt file
void logOperationToFile(const char* operation, const char* name, const char* result) {
    int fd = open("log.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd != -1) {
        char* timestamp = getTimestamp();
        write(fd, timestamp, strlen(timestamp));
        write(fd, " ", 1);
        if (operation) write(fd, operation, strlen(operation));
        if (operation && name) write(fd, " \"", 2);
        if (name) write(fd, name, strlen(name));
        if (name) write(fd, "\"", 1);
        if (result) {
            write(fd, " ", 1);
            write(fd, result, strlen(result));
        }
        write(fd, "\n", 1);
        close(fd);
    }
}

// Function that creates a directory with the given name
void createDirectory(const char *name) {
    if(mkdir(name, 0777) == -1) {
        if(errno == EEXIST) { // Directory already exists
            writeMessage(1, "Error: Directory ", name, " already exists.\n");
            logOperationToFile("Directory", name, "creation failed (already exists).");
        }
        else { // Directory could not be created
            writeMessage(1, "Error: Directory ", name, " can not created.\n");
            logOperationToFile("Directory", name, "creation failed.");
        }
        return;
    }
    logOperationToFile("Directory", name, "created successfully.");
}

// Function that creates a file with the given name
void createFile(const char *name) {
    int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0666);
    if(fd == -1) {
        if(errno == EEXIST) { // File already exists
            writeMessage(1, "Error: File ", name, " already exists.\n");
            logOperationToFile("File", name, "creation failed (already exists).");
        }
        else { // File could not be created
            writeMessage(1, "Error: File ", name, " cannot be created.\n");
            logOperationToFile("File", name, "creation failed.");
        }
    }
    else { // File created successfully
        char* timestamp = getTimestamp();
        write(fd, timestamp, strlen(timestamp));
        write(fd, "\n", 1);
        close(fd);
        logOperationToFile("File", name, "created successfully.");
    }
}

// Function that lists all files in the given directory
void listDirectory(const char *name) {
    pid_t pid = fork();
    
    if (pid < 0) { // Fork failed
        const char error[] = "Error: Fork failed.\n";
        write(1, error, strlen(error));
        logOperationToFile("Directory", name, "listing failed. (fork failed)");
        return;
    }
    else if (pid == 0) { // Child process
        DIR *dir;
        struct dirent *entry;
        if ((dir = opendir(name)) == NULL) { // Directory not found
            writeMessage(1, "Error: Directory ", name, " not found.\n");
            exit(1);
        }
        else {
            while ((entry = readdir(dir)) != NULL) {
                write(1, entry->d_name, strlen(entry->d_name));
                write(1, "\n", 1);
            }
            closedir(dir);
            exit(0);
        }
    }
    else { // Parent process
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == 0) { // Child process exited successfully
                logOperationToFile("Directory", name, "listed successfully.");
            }
            else { // Child process exited with an error
                logOperationToFile("Directory", name, "listing failed. (directory not found)");
            }
        }
    }
}

// Function that lists files with the given extension in the given directory
void listFilesByExtension(const char *name, const char *extension) {
    pid_t pid = fork();
    
    if (pid < 0) { // Fork failed
        const char error[] = "Error: Fork failed.\n";
        write(1, error, strlen(error));
        logOperationToFile("Directory", name, "listing failed. (fork failed)");
        return;
    }
    else if (pid == 0) { // Child process
        DIR *dir;
        struct dirent *entry;
        if ((dir = opendir(name)) == NULL) { // Directory not found
            writeMessage(1, "Error: Directory ", name, " not found.\n");
            exit(1);
        }
        else { // Directory found
            int counter = 0;
            while ((entry = readdir(dir)) != NULL) {
                if (strstr(entry->d_name, extension) != NULL) {
                    write(1, entry->d_name, strlen(entry->d_name));
                    write(1, "\n", 1);
                    counter++;
                }
            }
            if (counter == 0) { // No files with the given extension found
                const char message1[] = "No file with extension \"";
                write(1, message1, strlen(message1));
                write(1, extension, strlen(extension));
                const char message3[] = "\" found in \"";
                write(1, message3, strlen(message3));
                write(1, name, strlen(name));
                const char message5[] = "\".\n";
                write(1, message5, strlen(message5));
            }
            closedir(dir);
            exit(0);
        }
    }
    else { // Parent process
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == 0) { // Child process exited successfully
                logOperationToFile("Directory", name, "listed by extension successfully.");
            }
            else { // Child process exited with an error
                logOperationToFile("Directory", name, "listing by extension failed. (directory not found)");
            }
        }
    }
}

// Function that reads the content of the given file
void readFile(const char *name) {
    int fd = open(name, O_RDONLY);
    if(fd == -1) { // File not found
        writeMessage(1, "Error: File ", name, " not found.\n");
        logOperationToFile("File", name, "read failed. (file not found)");
    }
    else { // File found
        char buffer[1024];
        int bytesRead;
        while((bytesRead = read(fd, buffer, 1024)) > 0) {
            write(1, buffer, bytesRead);
        }
        close(fd);
        write(1, "\n", 1);
        logOperationToFile("File", name, "read successfully.");
    }
}

// Function that appends the given content to the given file
void appendToFile(const char *name, const char *content) {
    int fd = open(name, O_WRONLY | O_APPEND);
    if (fd == -1) { // File not found
        if (errno == EACCES) { // Permission error
            writeMessage(1, "Error: Cannot write to ", name, ". File is locked or read-only.\n");
            logOperationToFile("File", name, "append failed. (file is locked or read-only)");
        }
        else { // File could not be opened
            writeMessage(1, "Error: File ", name, " not found.\n");
            logOperationToFile("File", name, "append failed. (file not found)");
        }
        return;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == -1) { // File is locked
        writeMessage(1, "Error: Cannot write to ", name, ". File is locked or read-only.\n");
        close(fd);
        logOperationToFile("File", name, "append failed. (file is locked or read-only)");
        return;
    }
    write(fd, content, strlen(content));
    flock(fd, LOCK_UN);
    close(fd);
    logOperationToFile("Content appended to ", name, " successfully.");
}

// Function that deletes the given file
void deleteFile(const char *name) {
    pid_t pid = fork();
    if (pid < 0) { // Fork failed
        const char error[] = "Error: Fork failed.\n";
        write(1, error, strlen(error));
        logOperationToFile("File", name, "deletion failed. (fork failed)");
        return;
    }
    else if (pid == 0) { // Child process
        if (unlink(name) == -1) { // File not found
            writeMessage(1, "Error: File ", name, " not found.\n");
            exit(1);
        }
        exit(0);
    }
    else { // Parent process
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == 0) { // Child process exited successfully
                logOperationToFile("File", name, "deleted.");
            }
            else { // Child process exited with an error
                logOperationToFile("File", name, "deletion failed. (file not found)");
            }
        }
    }
}

// Function that deletes the given directory
void deleteDirectory(const char *name) {
    pid_t pid = fork();
    if (pid < 0) { // Fork failed
        const char error[] = "Error: Fork failed.\n";
        write(1, error, strlen(error));
        logOperationToFile("Directory", name, "deletion failed. (fork failed)");
        return;
    }
    else if (pid == 0) { // Child process
        if (rmdir(name) == -1) {
            if (errno == ENOTEMPTY) { // Directory is not empty
                writeMessage(1, "Error: Directory ", name, " is not empty.\n");
            }
            else if (errno == ENOENT) { // Directory not found
                writeMessage(1, "Error: Directory ", name, " not found.\n");
            }
            else { // Directory could not be deleted
                writeMessage(1, "Error: Directory ", name, " cannot be deleted.\n");
            }
            exit(1);
        }
        exit(0);
    }
    else { // Parent process
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == 0) { // Child process exited successfully
                logOperationToFile("Directory", name, "deleted.");
            }
            else { // Child process exited with an error
                logOperationToFile("Directory", name, "deletion failed. (directory not found or not empty)");
            }
        }
    }
}
// Function that shows the logs
void showLogs() {
    int fd = open("log.txt", O_RDONLY);
    if(fd == -1) { // Log file not found
        const char message1[] = "Error: Log file not found.\n";
        write(1, message1, strlen(message1));
        logOperationToFile("Log", NULL, "read failed. (log file not found)");
        return;
    }
    char buffer[1024];
    int bytesRead;
    while((bytesRead = read(fd, buffer, 1024)) > 0) {
        write(1, buffer, bytesRead);
    }
    close(fd);
    logOperationToFile("Log", NULL, "read successfully.");
}

// Function that shows the help
void showHelp() {
    const char usage[] = "Usage: fileManager <command> [arguments]\n";
    write(1, usage, strlen(usage));
    const char commands[] = "Commands:\n";
    write(1, commands, strlen(commands));
    const char command1[] = "  createDir \"folderName\" - Create a new directory\n";
    write(1, command1, strlen(command1));
    const char command2[] = "  createFile \"fileName\"\t - Create a new file\n";
    write(1, command2, strlen(command2));
    const char command3[] = "  listDir \"folderName\"\t - List all files in a directory\n";
    write(1, command3, strlen(command3));
    const char command4[] = "  listFilesByExtension \"folderName\" \".txt\" - List files with specific extension\n";
    write(1, command4, strlen(command4));
    const char command5[] = "  readFile \"fileName\"\t - Read a file's content\n";
    write(1, command5, strlen(command5));
    const char command6[] = "  appendToFile \"fileName\" \"new content\"\t   - Append content to a file\n";
    write(1, command6, strlen(command6));
    const char command7[] = "  deleteFile \"fileName\"\t - Delete a file\n";
    write(1, command7, strlen(command7));
    const char command8[] = "  deleteDir \"folderName\" - Delete an empty directory\n";
    write(1, command8, strlen(command8));
    const char command9[] = "  showLogs\t\t - Display operation logs\n";
    write(1, command9, strlen(command9));
    logOperationToFile("Help", NULL, "displayed.");
}

// Main function
int main(int argc, char *argv[]) {
    // Check if the user entered a command
    if (argc < 2) {
        showHelp();
        return 0;
    }
    // Determine the command and execute the corresponding function
    if (strcmp(argv[1], "createDir") == 0) {
        if (argc < 3) {
            write(1, "Error: Missing directory name.\n", 31);
            return 1;
        }
        createDirectory(argv[2]);
    } 
    else if (strcmp(argv[1], "createFile") == 0) {
        if (argc < 3) {
            write(1, "Error: Missing file name.\n", 26);
            return 1;
        }
        createFile(argv[2]);
    } 
    else if (strcmp(argv[1], "listDir") == 0) {
        if (argc < 3) {
            write(1, "Error: Missing directory name.\n", 31);
            return 1;
        }
        listDirectory(argv[2]);
    } 
    else if (strcmp(argv[1], "listFilesByExtension") == 0) {
        if (argc < 4) {
            write(1, "Error: Missing directory name or extension.\n", 46);
            return 1;
        }
        listFilesByExtension(argv[2], argv[3]);
    } 
    else if (strcmp(argv[1], "readFile") == 0) {
        if (argc < 3) {
            write(1, "Error: Missing file name.\n", 26);
            return 1;
        }
        readFile(argv[2]);
    } 
    else if (strcmp(argv[1], "appendToFile") == 0) {
        if (argc < 4) {
            write(1, "Error: Missing file name or content.\n", 37);
            return 1;
        }
        appendToFile(argv[2], argv[3]);
    } 
    else if (strcmp(argv[1], "deleteFile") == 0) {
        if (argc < 3) {
            write(1, "Error: Missing file name.\n", 26);
            return 1;
        }
        deleteFile(argv[2]);
    } 
    else if (strcmp(argv[1], "deleteDir") == 0) {
        if (argc < 3) {
            write(1, "Error: Missing directory name.\n", 31);
            return 1;
        }
        deleteDirectory(argv[2]);
    } 
    else if (strcmp(argv[1], "showLogs") == 0) {
        showLogs();
    } 
    else {
        write(1, "Error: Unknown command.\n", 25);
        return 1;
    }   
    return 0;
}