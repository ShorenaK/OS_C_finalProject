#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#define PORT 2024
#define BUFFER_SIZE 4096
#define ROOT_DIR "server_storage"

// === Part 4a: Global Mutex for File Operations ===
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// === Helper Function: Create Parent Directories ===
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

// === Part 4a: Handle Client in a Thread ===
void *handle_client(void *arg) {
    int client_sock = *(int *)arg;
    free(arg);

    char buffer[BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));

    ssize_t received = recv(client_sock, buffer, sizeof(buffer), 0);
    if (received <= 0) {
        printf("Failed to receive command.\n");
        close(client_sock);
        pthread_exit(NULL);
    }

    // === Handle WRITE Command ===
    if (strncmp(buffer, "WRITE", 5) == 0) {
        char filepath[1024];
        long filesize;

        if (sscanf(buffer, "WRITE %1023s %ld", filepath, &filesize) != 2 || filesize <= 0) {
            printf("Invalid WRITE command.\n");
            close(client_sock);
            pthread_exit(NULL);
        }

        char fullpath[2048];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", ROOT_DIR, filepath);
        make_parent_dirs(fullpath);

        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(fullpath, "wb");
        if (!fp) {
            perror("Failed to open file for writing");
            pthread_mutex_unlock(&file_mutex);
            close(client_sock);
            pthread_exit(NULL);
        }

        char *file_start = strchr(buffer, '\n');
        if (!file_start) {
            printf("Malformed WRITE command, no newline.\n");
            fclose(fp);
            pthread_mutex_unlock(&file_mutex);
            close(client_sock);
            pthread_exit(NULL);
        }

        file_start++;
        ssize_t header_len = file_start - buffer;
        ssize_t initial_data_len = received - header_len;

        long bytes_received = 0;
        if (initial_data_len > 0) {
            fwrite(file_start, 1, initial_data_len, fp);
            bytes_received += initial_data_len;
        }

        while (bytes_received < filesize) {
            ssize_t chunk = recv(client_sock, buffer, sizeof(buffer), 0);
            if (chunk <= 0) break;
            fwrite(buffer, 1, chunk, fp);
            bytes_received += chunk;
        }

        fclose(fp);
        pthread_mutex_unlock(&file_mutex);
        printf("File saved: %s (%ld bytes)\n", fullpath, bytes_received);
    }

    // === Handle GET Command ===
    else if (strncmp(buffer, "GET", 3) == 0) {
        char filepath[1024];
        if (sscanf(buffer, "GET %1023s", filepath) != 1) {
            printf("Invalid GET command.\n");
            close(client_sock);
            pthread_exit(NULL);
        }

        char fullpath[2048];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", ROOT_DIR, filepath);

        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(fullpath, "rb");
        if (!fp) {
            char *msg = "SIZE 0\n";
            send(client_sock, msg, strlen(msg), 0);
            pthread_mutex_unlock(&file_mutex);
            close(client_sock);
            pthread_exit(NULL);
        }

        fseek(fp, 0, SEEK_END);
        long filesize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        char header[64];
        snprintf(header, sizeof(header), "SIZE %ld\n", filesize);
        send(client_sock, header, strlen(header), 0);

        memset(buffer, 0, sizeof(buffer));
        recv(client_sock, buffer, sizeof(buffer), 0);

        if (strncmp(buffer, "READY", 5) != 0) {
            printf("Client did not confirm READY.\n");
            fclose(fp);
            pthread_mutex_unlock(&file_mutex);
            close(client_sock);
            pthread_exit(NULL);
        }

        while (!feof(fp)) {
            size_t bytes_read = fread(buffer, 1, sizeof(buffer), fp);
            if (bytes_read > 0) {
                send(client_sock, buffer, bytes_read, 0);
            }
        }

        fclose(fp);
        pthread_mutex_unlock(&file_mutex);
        printf("File sent: %s (%ld bytes)\n", fullpath, filesize);
    }

    // === Handle RM Command ===
    else if (strncmp(buffer, "RM", 2) == 0) {
        char filepath[1024];
        if (sscanf(buffer, "RM %1023s", filepath) != 1) {
            printf("Invalid RM command.\n");
            close(client_sock);
            pthread_exit(NULL);
        }

        char fullpath[2048];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", ROOT_DIR, filepath);

        pthread_mutex_lock(&file_mutex);
        if (remove(fullpath) == 0) {
            send(client_sock, "File deleted successfully.\n", 28, 0);
            printf("Deleted: %s\n", fullpath);
        } else {
            send(client_sock, "Failed to delete file.\n", 24, 0);
            perror("Delete failed");
        }
        pthread_mutex_unlock(&file_mutex);
    }

    else {
        printf("Unknown command: %s\n", buffer);
    }

    close(client_sock);
    pthread_exit(NULL);
}

// === Main Function ===
int main() {
    int server_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_size = sizeof(client_addr);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_sock);
        return 1;
    }

    if (listen(server_sock, 5) < 0) {
        perror("Listen failed");
        close(server_sock);
        return 1;
    }

    printf("Server listening on port %d...\n", PORT);

    while (1) {
        int *pclient = malloc(sizeof(int));
        *pclient = accept(server_sock, (struct sockaddr*)&client_addr, &client_size);
        if (*pclient < 0) {
            perror("Accept failed");
            free(pclient);
            continue;
        }

        printf("Client connected: %s\n", inet_ntoa(client_addr.sin_addr));
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, pclient);
        pthread_detach(tid);
    }

    close(server_sock);
    return 0;
}
