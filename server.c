/*
 * server.c - Practicum 2 Project
 * 
 * Author: Shorena K. Anzhilov
 * Course: CS5600 - Northeastern University
 * Semester: Spring 2025
 * 
 * This server program handles multiple client connections concurrently.
 * It supports the following functionalities:
 * - WRITE: Receive and store encrypted files with versioning.
 * - GET: Send decrypted files to clients, supporting version retrieval.
 * - RM: Remove specified files from the server storage.
 * - LS: List files in the server storage, with optional filtering.
 * - SIGINT Handling: Gracefully shuts down the server upon receiving Ctrl+C.
 * 
 * Features Implemented:
 * - Multi-client handling using pthreads.
 * - File versioning.
 * - XOR-based encryption/decryption.
 * - Signal handling for graceful termination.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>

#define PORT 2024
#define BUFFER_SIZE 4096
#define ROOT_DIR "server_storage"
#define ENCRYPTION_KEY "secretkey"

int server_sock; 
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER; 

// ===  Helper Functions === // 

/*
 * xor_cipher - Encrypts/Decrypts data using XOR cipher with a given key.
 */

void xor_cipher(char *data, long size, const char *key) {
    size_t key_len = strlen(key);
    for (long i = 0; i < size; ++i) {
        data[i] ^= key[i % key_len];
    }
}

/*
 * make_parent_dirs - Creates parent directories for a given file path.
 */

void make_parent_dirs(const char *path) {
    char temp[1024];
    strcpy(temp, path);
    for (char *p = temp + strlen(ROOT_DIR) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(temp, 0755);
            *p = '/';
        }
    }
}

/*
 * get_latest_version - Retrieves the latest version number of a file.
 */

int get_latest_version(const char *basepath) {
    int max_version = 0;
    DIR *dir = opendir(ROOT_DIR);
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, basepath) && strstr(entry->d_name, "_v")) {
            int ver = 0;
            sscanf(entry->d_name, "%*[^_]_v%d", &ver);
            if (ver > max_version) max_version = ver;
        }
    }
    closedir(dir);
    return max_version;
}

/*
 * list_files - Sends a list of files in the server storage to the client.
 * If a filter is provided, only matching files are listed.
 */

void list_files(int client_sock, const char *filter) {
    DIR *dir = opendir(ROOT_DIR);
    struct dirent *entry;
    char line[1024];
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            if (!filter || strstr(entry->d_name, filter)) {
                snprintf(line, sizeof(line), "%s\n", entry->d_name);
                send(client_sock, line, strlen(line), 0);
            }
        }
    }
    send(client_sock, "__END__\n", 8, 0);
    closedir(dir);
}

/*
 * handle_sigint - Signal handler for SIGINT (Ctrl+C).
 * Closes the server socket and exits gracefully.
 */

void handle_sigint(int sig) {
    printf("\nCaught SIGINT, closing server socket...\n");
    close(server_sock);
    exit(0);
}

// ===  Client Handler Thread === // 

/*
 * handle_client - Handles client requests in a separate thread.
 */

void *handle_client(void *arg) {
    int client_sock = *(int *)arg;
    free(arg);
    
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));
    ssize_t received = recv(client_sock, buffer, sizeof(buffer), 0);
    if (received <= 0) {
        close(client_sock);
        pthread_exit(NULL);
    }
    
    // === WRITE Operation  === //
    
    if (strncmp(buffer, "WRITE", 5) == 0) {
        char filepath[1024];
        long filesize;
        sscanf(buffer, "WRITE %s %ld", filepath, &filesize);
        
        char *dot = strrchr(filepath, '.');
        char filename[512];
        char ext[32] = "";
        if (dot) {
            strncpy(filename, filepath, dot - filepath);
            filename[dot - filepath] = '\0';
            strcpy(ext, dot);
        } else {
            strcpy(filename, filepath);
        }
        
        pthread_mutex_lock(&file_mutex);
        int version = get_latest_version(filename) + 1;
        
        char final[2048];
        snprintf(final, sizeof(final), "%s/%s_v%d%s", ROOT_DIR, filename, version, ext);
        make_parent_dirs(final);
        FILE *fp = fopen(final, "wb");
        pthread_mutex_unlock(&file_mutex);
        if (!fp) {
            close(client_sock);
            pthread_exit(NULL);
        }
        
        char *file_start = strchr(buffer, '\n') + 1;
        long written = received - (file_start - buffer);
        fwrite(file_start, 1, written, fp);
        
        while (written < filesize) {
            ssize_t chunk = recv(client_sock, buffer, sizeof(buffer), 0);
            if (chunk <= 0) break;
            fwrite(buffer, 1, chunk, fp);
            written += chunk;
        }
        
        fclose(fp);
        printf("Saved: %s (%ld bytes)\n", final, written);
    }
    
    // === GET: Retrieve a File === //
    
    else if (strncmp(buffer, "GET", 3) == 0) {
        char path[1024];
        int version = -1;
        
        // Parse the GET command to extract the file path and optional version number
        sscanf(buffer, "GET %[^:\n]:%d", path, &version);
        if (version == -1) sscanf(buffer, "GET %s", path);
        
        // Separate the filename and extension
        char *dot = strrchr(path, '.');
        char filename[512];
        char ext[32] = "";
        if (dot) {
            strncpy(filename, path, dot - path);
            filename[dot - path] = '\0';
            strcpy(ext, dot);
        } else {
            strcpy(filename, path);
        }
        
        // Determine the latest version if not specified
        if (version == -1) version = get_latest_version(filename);
        if (version == 0) {
            send(client_sock, "SIZE 0\n", 8, 0);
            close(client_sock);
            pthread_exit(NULL);
        }
        
        // Construct the full path to the requested file version
        char final[2048];
        snprintf(final, sizeof(final), "%s/%s_v%d%s", ROOT_DIR, filename, version, ext);
        
        // Open the file for reading
        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(final, "rb");
        pthread_mutex_unlock(&file_mutex);
        if (!fp) {
            send(client_sock, "SIZE 0\n", 8, 0);
            close(client_sock);
            pthread_exit(NULL);
        }
        
        // Determine the file size and send it to the client
        fseek(fp, 0, SEEK_END);
        long filesize = ftell(fp);
        rewind(fp);
        char msg[64];
        snprintf(msg, sizeof(msg), "SIZE %ld\n", filesize);
        send(client_sock, msg, strlen(msg), 0);
        
        // Wait for client acknowledgment
        recv(client_sock, buffer, sizeof(buffer), 0);
        
        // Read the file, encrypt it, and send it to the client
        while (!feof(fp)) {
            size_t read = fread(buffer, 1, sizeof(buffer), fp);
            xor_cipher(buffer, read, ENCRYPTION_KEY);
            send(client_sock, buffer, read, 0);
        }
        
        fclose(fp);
        printf("Sent: %s (%ld bytes)\n", final, filesize);
    }
    
    // === RM -->  Delete a File ====== //
    
    else if (strncmp(buffer, "RM", 2) == 0) {
        char path[1024];
        
        // Parse the RM command to extract the file path
        sscanf(buffer, "RM %s", path);
        
        // Construct the full path to the file
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", ROOT_DIR, path);
        
        // Attempt to delete the file
        pthread_mutex_lock(&file_mutex);
        int status = remove(full);
        pthread_mutex_unlock(&file_mutex);
        
        // Inform the client of the result
        if (status == 0) {
            send(client_sock, "File deleted.\n", 15, 0);
        } else {
            send(client_sock, "Delete failed.\n", 16, 0);
        }
    }
    
    // === LS: List Server Files === //
    
    else if (strncmp(buffer, "LS", 2) == 0) {
        char filter[1024] = "";
        
        // Parse the LS command to extract an optional filter
        sscanf(buffer, "LS %s", filter);
        
        // List files in the server storage, applying the filter if provided
        list_files(client_sock, strlen(filter) > 0 ? filter : NULL);
    }
    
    close(client_sock);
    pthread_exit(NULL);
 }
    // === Main Function ========== //

    int main() {
        // Set up the SIGINT handler for graceful shutdown
        signal(SIGINT, handle_sigint);

        struct sockaddr_in server_addr, client_addr;
        socklen_t client_size = sizeof(client_addr);

        // Create the server socket
        server_sock = socket(AF_INET, SOCK_STREAM, 0);

        // Configure the server address
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(PORT);
        server_addr.sin_addr.s_addr = INADDR_ANY;

        // Bind the socket to the specified address and port
        bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));

        // Start listening for incoming connections
        listen(server_sock, 5);
        printf("Server listening on port %d...\n", PORT);

        // Main loop to accept and handle client connections
        while (1) {
            int *client = malloc(sizeof(int));
            *client = accept(server_sock, (struct sockaddr*)&client_addr, &client_size);
            pthread_t tid;
            pthread_create(&tid, NULL, handle_client, client);
            pthread_detach(tid);
        }
 }
