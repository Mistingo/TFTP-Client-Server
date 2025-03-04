#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SIZE 512

#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERROR 5

#define TFTP_DIR "/var/lib/tftpboot/"

void send_ack(int sockfd, struct sockaddr_in addr, int block_num) {
    char ack[4];
    ack[0] = 0;
    ack[1] = ACK;
    ack[2] = (block_num >> 8) & 0xFF;
    ack[3] = block_num & 0xFF;

    sendto(sockfd, ack, sizeof(ack), 0, (struct sockaddr*)&addr, sizeof(addr));
    printf("[INFO] ACK envoyé - Bloc %d\n", block_num);
}

void send_file(int sockfd, struct sockaddr_in addr, char* filename) {
    int n, block_num = 1;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    FILE* fp = fopen(filepath, "rb");

    if (fp == NULL) {
        perror("[ERROR] Fichier introuvable.");
        return;
    }

    printf("[INFO] Début d'envoi du fichier : %s\n", filename);

    while (1) {
        memset(buffer, 0, SIZE);
        buffer[0] = 0;
        buffer[1] = DATA;
        buffer[2] = (block_num >> 8) & 0xFF;
        buffer[3] = block_num & 0xFF;

        n = fread(buffer + 4, 1, SIZE, fp);
        sendto(sockfd, buffer, n + 4, 0, (struct sockaddr*)&addr, addr_size);
        printf("[INFO] DATA envoyé - Bloc %d (%d octets)\n", block_num, n);

        recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        if ((buffer[0] << 8 | buffer[1]) != ACK) break;

        block_num++;
        if (n < SIZE) break;
    }

    fclose(fp);
    printf("[INFO] Fin d'envoi du fichier : %s\n", filename);
}

void receive_file(int sockfd, struct sockaddr_in addr, char* filename) {
    int n, block_num = 0;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    FILE* fp = fopen(filepath, "wb");
    if (fp == NULL) {
        perror("[ERROR] Impossible de créer le fichier.");
        return;
    }

    send_ack(sockfd, addr, block_num);

    while (1) {
        memset(buffer, 0, SIZE);
        n = recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        if (n < 4) break;

        int opcode = (buffer[0] << 8) | buffer[1];

        if (opcode == DATA) {
            block_num = (buffer[2] << 8) | buffer[3];
            fwrite(buffer + 4, 1, n - 4, fp);
            printf("[INFO] DATA reçu - Bloc %d (%d octets)\n", block_num, n - 4);
            send_ack(sockfd, addr, block_num);
            if (n < SIZE) break;
        }
    }

    fclose(fp);
    printf("[INFO] Fin de réception du fichier : %s\n", filename);
}

int main() {
    const int port = htons(6969);
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_size = sizeof(client_addr);
    char buffer[SIZE];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("[ERROR] Échec de la création du socket.");
        exit(1);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = port;
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[ERROR] Échec du bind.");
        exit(1);
    }

    printf("[STARTING] Serveur TFTP en attente...\n");

    while (1) {
        memset(buffer, 0, SIZE);
        recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&client_addr, &addr_size);

        int opcode = (buffer[0] << 8) | buffer[1];
        char *filename = buffer + 2;

        if (opcode == WRQ) {
            printf("[INFO] WRQ reçu - Demande d'écriture de fichier : %s\n", filename);
            receive_file(sockfd, client_addr, filename);
        } else if (opcode == RRQ) {
            printf("[INFO] RRQ reçu - Demande de lecture de fichier : %s\n", filename);
            send_file(sockfd, client_addr, filename);
        }
    }

    close(sockfd);
    return 0;
}