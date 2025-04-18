#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define BUFFER_SIZE 4096

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s WRITE local_file_path remote_file_path\n", argv[0]);
        printf("  %s GET remote_file_path local_file_path\n", argv[0]);
        printf("  %s RM remote_file_path\n", argv[0]);
        return 1;
    }

    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(2024);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Connect to server
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return 1;
    }
        // ====== PART 1: WRITE ======
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

    // Allocate memory for file content
    char *filedata = malloc(filesize);
    if (!filedata) {
        perror("Memory allocation failed");
        fclose(fp);
        close(sock);
        return 1;
    }

    // Read file into memory
    size_t bytes_read = fread(filedata, 1, filesize, fp);
    fclose(fp);

    if (bytes_read != filesize) {
        printf("Client: fread only read %zu of %ld bytes\n", bytes_read, filesize);
        free(filedata);
        close(sock);
        return 1;
    }

    printf("Client: read %zu bytes from file\n", bytes_read);

    // Send command header
    char header[1024];
    snprintf(header, sizeof(header), "WRITE %s %ld\n", remote_path, filesize);
    send(sock, header, strlen(header), 0);

    // Send actual file content
    ssize_t sent = send(sock, filedata, filesize, 0);
    printf("Client: sent %zd bytes to server\n", sent);

    printf("File '%s' sent to server as '%s'\n", local_path, remote_path);
    free(filedata);
  }
    // ====== PART 2: GET ======
    else if (strcmp(argv[1], "GET") == 0 && argc == 4) {
        char *remote_path = argv[2];
        char *local_path = argv[3];

        char header[1024];
        snprintf(header, sizeof(header), "GET %s\n", remote_path);
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

        FILE *fp = fopen(local_path, "wb");
        if (!fp) {
            perror("Failed to create local file");
            close(sock);
            return 1;
        }

        long bytes_received = 0;
        while (bytes_received < filesize) {
            ssize_t chunk = recv(sock, buffer, sizeof(buffer), 0);
            if (chunk <= 0) break;
            fwrite(buffer, 1, chunk, fp);
            bytes_received += chunk;
        }

        fclose(fp);
        printf("File '%s' received from server as '%s' (%ld bytes)\n", remote_path, local_path, bytes_received);
    }

    // ====== PART 3: RM (Delete) ======
    else if (strcmp(argv[1], "RM") == 0 && argc == 3) {
        char *remote_path = argv[2];

        char header[1024];
        snprintf(header, sizeof(header), "RM %s\n", remote_path);
        send(sock, header, strlen(header), 0);

        memset(buffer, 0, sizeof(buffer));
        recv(sock, buffer, sizeof(buffer), 0);
        printf("Server response: %s\n", buffer);
    }

    // ====== Multi-client ======
    /*
   
    */

    // ====== Invalid command ======
    else {
        printf("Invalid command or argument count.\n");
    }

    close(sock);
    return 0;
}

