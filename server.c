/ === server.c with 4a (multi-client), 4b (versioning), 4c (encryption), 4d (LS, SIGINT) ===
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

int server_sock; // for SIGINT handling
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

void xor_cipher(char *data, long size, const char *key) {
    size_t key_len = strlen(key);
    for (long i = 0; i < size; ++i) {
        data[i] ^= key[i % key_len];
    }
}

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

void handle_sigint(int sig) {
    printf("\nCaught SIGINT, closing server socket...\n");
    close(server_sock);
    exit(0);
}

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

    // === WRITE ===
    if (strncmp(buffer, "WRITE", 5) == 0) {
        char filepath[1024]; long filesize;
        sscanf(buffer, "WRITE %s %ld", filepath, &filesize);

        char *dot = strrchr(filepath, '.');
        char filename[512]; char ext[32] = "";
        if (dot) {
            strncpy(filename, filepath, dot - filepath);
            filename[dot - filepath] = '\0';
            strcpy(ext, dot);
        } else strcpy(filename, filepath);

        pthread_mutex_lock(&file_mutex);
        int version = get_latest_version(filename) + 1;

        char final[2048];
        snprintf(final, sizeof(final), "%s/%s_v%d%s", ROOT_DIR, filename, version, ext);
        make_parent_dirs(final);
        FILE *fp = fopen(final, "wb");
        pthread_mutex_unlock(&file_mutex);
        if (!fp) { close(client_sock); pthread_exit(NULL); }

        char *file_start = strchr(buffer, '\n') + 1;
        long written = received - (file_start - buffer);
        xor_cipher(file_start, written, ENCRYPTION_KEY);
        fwrite(file_start, 1, written, fp);

        while (written < filesize) {
            ssize_t chunk = recv(client_sock, buffer, sizeof(buffer), 0);
            if (chunk <= 0) break;
            xor_cipher(buffer, chunk, ENCRYPTION_KEY);
            fwrite(buffer, 1, chunk, fp);
            written += chunk;
        }

        fclose(fp);
        printf("Saved: %s (%ld bytes)\n", final, written);
    }

    // === GET ===
    else if (strncmp(buffer, "GET", 3) == 0) {
        char path[1024]; int version = -1;
        sscanf(buffer, "GET %[^:\n]:%d", path, &version);
        if (version == -1) sscanf(buffer, "GET %s", path);

        char *dot = strrchr(path, '.');
        char filename[512]; char ext[32] = "";
        if (dot) {
            strncpy(filename, path, dot - path);
            filename[dot - path] = '\0';
            strcpy(ext, dot);
        } else strcpy(filename, path);

        if (version == -1) version = get_latest_version(filename);
        if (version == 0) { send(client_sock, "SIZE 0\n", 8, 0); close(client_sock); pthread_exit(NULL); }

        char final[2048];
        snprintf(final, sizeof(final), "%s/%s_v%d%s", ROOT_DIR, filename, version, ext);

        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(final, "rb");
        pthread_mutex_unlock(&file_mutex);
        if (!fp) { send(client_sock, "SIZE 0\n", 8, 0); close(client_sock); pthread_exit(NULL); }

        fseek(fp, 0, SEEK_END); long filesize = ftell(fp); rewind(fp);
        char msg[64]; snprintf(msg, sizeof(msg), "SIZE %ld\n", filesize);
        send(client_sock, msg, strlen(msg), 0);

        recv(client_sock, buffer, sizeof(buffer), 0);
        while (!feof(fp)) {
            size_t read = fread(buffer, 1, sizeof(buffer), fp);
            xor_cipher(buffer, read, ENCRYPTION_KEY);
            send(client_sock, buffer, read, 0);
        }
        fclose(fp);
        printf("Sent: %s (%ld bytes)\n", final, filesize);
    }

    // === RM ===
    else if (strncmp(buffer, "RM", 2) == 0) {
        char path[1024]; sscanf(buffer, "RM %s", path);
        char full[2048]; snprintf(full, sizeof(full), "%s/%s", ROOT_DIR, path);
        pthread_mutex_lock(&file_mutex);
        int status = remove(full);
        pthread_mutex_unlock(&file_mutex);
        if (status == 0) send(client_sock, "File deleted.\n", 15, 0);
        else send(client_sock, "Delete failed.\n", 16, 0);
    }

    // === LS ===
    else if (strncmp(buffer, "LS", 2) == 0) {
        char filter[1024] = "";
        sscanf(buffer, "LS %s", filter);
        list_files(client_sock, strlen(filter) > 0 ? filter : NULL);
    }

    close(client_sock);
    pthread_exit(NULL);
}

int main() {
    signal(SIGINT, handle_sigint); // === 4d: SIGINT Handling ===

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_size = sizeof(client_addr);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_sock, 5);
    printf("Server listening on port %d...\n", PORT);

    while (1) {
        int *client = malloc(sizeof(int));
        *client = accept(server_sock, (struct sockaddr*)&client_addr, &client_size);
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, client);
        pthread_detach(tid);
    }
}
