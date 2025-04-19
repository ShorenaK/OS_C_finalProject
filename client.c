#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define BUFFER_SIZE 4096
#define ENCRYPTION_KEY "secretkey"  // === Part 4c: Encryption Key ===

// === Part 4c: XOR Encryption Helper ===
void xor_cipher(char *data, long size, const char *key) {
    size_t key_len = strlen(key);
    for (long i = 0; i < size; ++i) {
        data[i] ^= key[i % key_len];
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s WRITE local_file_path remote_file_path\n", argv[0]);
        printf("  %s GET remote_file_path[:version] local_file_path\n", argv[0]);
        printf("  %s RM remote_file_path\n", argv[0]);
        printf("  %s LS [remote_path]\n", argv[0]);
        return 1;
    }

    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

    // === Connect to Server ===
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(2024);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return 1;
    }

    // === Part 1 + 4c: WRITE with Encryption ===
    if (strcmp(argv[1], "WRITE") == 0 && argc == 4) {
        char *local_path = argv[2];
        char *remote_path = argv[3];

        FILE *fp = fopen(local_path, "rb");
        if (!fp) {
            perror("Failed to open local file");
            close(sock);
            return 1;
        }

        fseek(fp, 0, SEEK_END);
        long filesize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (filesize <= 0) {
            printf("Empty or invalid file.\n");
            fclose(fp);
            close(sock);
            return 1;
        }

        char *filedata = malloc(filesize);
        if (!filedata) {
            perror("Memory allocation failed");
            fclose(fp);
            close(sock);
            return 1;
        }

        fread(filedata, 1, filesize, fp);
        fclose(fp);

        xor_cipher(filedata, filesize, ENCRYPTION_KEY); // Encrypt data

         // 🔍 Print encrypted bytes for debug
        printf("DEBUG: Encrypted content (first 32 bytes or less):\n");
        for (int i = 0; i < (filesize < 32 ? filesize : 32); i++) {
            printf("%02x ", (unsigned char)filedata[i]);
        }
        printf("\n");

        char header[1024];
        snprintf(header, sizeof(header), "WRITE %s %ld\n", remote_path, filesize);
        send(sock, header, strlen(header), 0);

        send(sock, filedata, filesize, 0);
        printf("Encrypted file '%s' sent to server as '%s'\n", local_path, remote_path);
        free(filedata);
    }

    // === Part 2 + 4b + 4c: GET with Versioning and Decryption ===
    else if (strcmp(argv[1], "GET") == 0 && argc == 4) {
        char *remote_arg = argv[2];
        char *local_path = argv[3];

        char remote_path[1024];
        int version = -1;

        char *colon = strchr(remote_arg, ':');
        if (colon) {
            *colon = '\0';
            strcpy(remote_path, remote_arg);
            version = atoi(colon + 1);
        } else {
            strcpy(remote_path, remote_arg);
        }

        char header[1024];
        if (version > 0) {
            snprintf(header, sizeof(header), "GET %s:%d\n", remote_path, version);
        } else {
            snprintf(header, sizeof(header), "GET %s\n", remote_path);
        }

        send(sock, header, strlen(header), 0);

        long filesize;
        memset(buffer, 0, sizeof(buffer));
        recv(sock, buffer, sizeof(buffer), 0);

        if (sscanf(buffer, "SIZE %ld", &filesize) != 1 || filesize <= 0) {
            printf("Invalid file or file not found on server.\n");
            close(sock);
            return 1;
        }

        send(sock, "READY\n", 6, 0);

        char *filedata = malloc(filesize);
        if (!filedata) {
            perror("Memory allocation failed");
            close(sock);
            return 1;
        }

        long bytes_received = 0;
        while (bytes_received < filesize) {
            ssize_t chunk = recv(sock, filedata + bytes_received, filesize - bytes_received, 0);
            if (chunk <= 0) break;
            bytes_received += chunk;
        }

        FILE *fp = fopen(local_path, "wb");
        if (!fp) {
            perror("Failed to create local file");
            free(filedata);
            close(sock);
            return 1;
        }

        fwrite(filedata, 1, filesize, fp);
        fclose(fp);
        free(filedata);

        printf("Decrypted file saved as '%s'\n", local_path);
    }

    // === Part 3: RM ===
    else if (strcmp(argv[1], "RM") == 0 && argc == 3) {
        char *remote_path = argv[2];

        char header[1024];
        snprintf(header, sizeof(header), "RM %s\n", remote_path);
        send(sock, header, strlen(header), 0);

        memset(buffer, 0, sizeof(buffer));
        recv(sock, buffer, sizeof(buffer), 0);
        printf("Server response: %s\n", buffer);
    }

    // === Part 4d: LS ===
    else if (strcmp(argv[1], "LS") == 0) {
        char header[1024];
        if (argc == 3)
            snprintf(header, sizeof(header), "LS %s\n", argv[2]);
        else
            snprintf(header, sizeof(header), "LS\n");

        send(sock, header, strlen(header), 0);

        while (1) {
            ssize_t len = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (len <= 0) break;
            buffer[len] = '\0';
            printf("%s", buffer);
            if (strstr(buffer, "__END__\n")) break;
        }
    }

    else {
        printf("Invalid command or argument count.\n");
    }

    close(sock);
    return 0;
}

