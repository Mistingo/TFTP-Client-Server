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

#define TFTP_DIR "/var/lib/tftpboot/"

// Fonction pour envoyer un paquet d'erreur au client
void send_error(int sockfd, struct sockaddr_in addr, int error_code, const char *error_msg) {
    char buffer[SIZE];
    int len = sprintf(buffer, "%c%c%c%c%s%c", 0, ERROR, 0, error_code, error_msg, 0);
    sendto(sockfd, buffer, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    printf("[ERROR] Envoyé au client : Code %d - %s\n", error_code, error_msg);
}

// Fonction pour envoyer un fichier au client (GET)
void send_file(int sockfd, struct sockaddr_in addr, char* filename) {
    int n, block_num = 1;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    FILE* fp = fopen(filepath, "rb");

    if (fp == NULL) {
        perror("[ERROR] Fichier introuvable.");
        send_error(sockfd, addr, 1, "File not found");
        return;
    }

    printf("[INFO] Début d'envoi du fichier : %s\n", filename);

    while (1) {
        memset(buffer, 0, SIZE);
        buffer[0] = 0;
        buffer[1] = DATA;
        buffer[2] = (block_num >> 8) & 0xFF;
        buffer[3] = block_num & 0xFF;

        n = fread(buffer + 4, 1, BLOCK_SIZE, fp);
        sendto(sockfd, buffer, n + 4, 0, (struct sockaddr*)&addr, addr_size);
        printf("[INFO] DATA envoyé - Bloc %d (%d octets)\n", block_num, n);

        // Attente de l'ACK
        recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        int opcode = (buffer[0] << 8) | buffer[1];

        if (opcode != ACK) {
            printf("[ERROR] ACK attendu mais reçu opcode %d\n", opcode);
            break;
        }

        int received_block = (buffer[2] << 8) | buffer[3];
        printf("[INFO] ACK reçu - Bloc %d\n", received_block);

        block_num++;
        if (n < BLOCK_SIZE) break;  // Fin du fichier
    }

    fclose(fp);
    printf("[INFO] Fin d'envoi du fichier : %s\n", filename);
}

// Fonction pour recevoir un fichier du client (PUT)
void receive_file(int sockfd, struct sockaddr_in addr, char* filename) {
    int n, block_num = 0;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    FILE* fp = fopen(filepath, "wb");

    if (fp == NULL) {
        perror("[ERROR] Impossible de créer le fichier.");
        send_error(sockfd, addr, 2, "Access violation");
        return;
    }

    printf("[INFO] Début de réception du fichier : %s\n", filename);

    while (1) {
        // Envoi de l'ACK pour confirmer la réception du bloc précédent
        char ack[4] = {0, ACK, (block_num >> 8) & 0xFF, block_num & 0xFF};
        sendto(sockfd, ack, 4, 0, (struct sockaddr*)&addr, addr_size);
        printf("[INFO] ACK envoyé - Bloc %d\n", block_num);

        memset(buffer, 0, SIZE);
        n = recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        if (n < 4) break;

        int opcode = (buffer[0] << 8) | buffer[1];

        if (opcode == ERROR) {
            printf("[ERROR] Client a renvoyé une erreur : %s\n", buffer + 4);
            fclose(fp);
            remove(filepath);
            return;
        } 
        else if (opcode == DATA) {
            block_num = (buffer[2] << 8) | buffer[3];
            fwrite(buffer + 4, 1, n - 4, fp);
            printf("[INFO] DATA reçu - Bloc %d (%d octets)\n", block_num, n - 4);

            if (n < SIZE) break; // Fin de la transmission
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
        } 
        else if (opcode == RRQ) {
            printf("[INFO] RRQ reçu - Demande de lecture de fichier : %s\n", filename);
            send_file(sockfd, client_addr, filename);
        } 
        else {
            printf("[ERROR] Opcode inconnu reçu (%d)\n", opcode);
            send_error(sockfd, client_addr, 4, "Illegal TFTP operation");
        }
    }

    close(sockfd);
    return 0;
}