#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SIZE 516
#define BLOCK_SIZE 512

#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERROR 5

void send_ack(int sockfd, struct sockaddr_in addr, int block_num) {
    char ack[4];
    ack[0] = 0;
    ack[1] = ACK;
    ack[2] = (block_num >> 8) & 0xFF;
    ack[3] = block_num & 0xFF;

    sendto(sockfd, ack, sizeof(ack), 0, (struct sockaddr*)&addr, sizeof(addr));
}

void receive_file_data(int sockfd, struct sockaddr_in addr, char* filename) {
    int n, block_num = 0;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);

    FILE* fp = fopen(filename, "wb");
    if (fp == NULL) {
        perror("tftp> Impossible de créer le fichier.");
        return;
    }

    while (1) {
        memset(buffer, 0, SIZE);
        n = recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        if (n < 4) {
            printf("tftp> Erreur lors de la réception.\n");
            fclose(fp);
            remove(filename);
            return;
        }

        int opcode = (buffer[0] << 8) | buffer[1];

        if (opcode == ERROR) {
            printf("tftp> Erreur du serveur : %s\n", buffer + 4);
            fclose(fp);
            remove(filename);
            return;
        } 
        else if (opcode == DATA) {
            block_num = (buffer[2] << 8) | buffer[3];
            fwrite(buffer + 4, 1, n - 4, fp);
            send_ack(sockfd, addr, block_num);

            if (n < SIZE) break; // Fin de transmission
        }
    }

    fclose(fp);
}

void send_file_data(FILE* fp, int sockfd, struct sockaddr_in addr, char* filename) {
    int n, block_num = 1;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);

    while (1) {
        memset(buffer, 0, SIZE);
        buffer[0] = 0;
        buffer[1] = DATA;
        buffer[2] = (block_num >> 8) & 0xFF;
        buffer[3] = block_num & 0xFF;

        n = fread(buffer + 4, 1, BLOCK_SIZE, fp);
        sendto(sockfd, buffer, n + 4, 0, (struct sockaddr*)&addr, sizeof(addr));

        recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        if ((buffer[0] << 8 | buffer[1]) != ACK) break;

        block_num++;
        if (n < BLOCK_SIZE) break;
    }

    fclose(fp);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *ip = argv[1];
    const int port = htons(6969);
    int sockfd;
    struct sockaddr_in server_addr;
    char command[SIZE], filename[SIZE];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("tftp> Échec de la création du socket.");
        exit(1);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = port;
    server_addr.sin_addr.s_addr = inet_addr(ip);

    while (1) {
        printf("tftp> ");
        fgets(command, sizeof(command), stdin);
        command[strcspn(command, "\n")] = 0;

        if (strncmp(command, "put ", 4) == 0) {
            strcpy(filename, command + 4);
            FILE *fp = fopen(filename, "rb");
            if (fp == NULL) {
                perror("tftp> Impossible d'ouvrir le fichier.");
                continue;
            }

            char request[SIZE + 10];
            int req_len = sprintf(request, "%c%c%s%c%s%c", 0, WRQ, filename, 0, "octet", 0);
            sendto(sockfd, request, req_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));

            send_file_data(fp, sockfd, server_addr, filename);
        } 
        else if (strncmp(command, "get ", 4) == 0) {
            strcpy(filename, command + 4);

            char request[SIZE + 10];
            int req_len = sprintf(request, "%c%c%s%c%s%c", 0, RRQ, filename, 0, "octet", 0);
            sendto(sockfd, request, req_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));

            receive_file_data(sockfd, server_addr, filename);
        } 
        else if (strcmp(command, "quit") == 0) {
            break;
        } 
        else {
            printf("tftp> Commande invalide.\n");
        }
    }

    close(sockfd);
    return 0;
}
