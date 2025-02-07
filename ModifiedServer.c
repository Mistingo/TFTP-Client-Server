#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SIZE 516
#define TFTP_DIR "/var/lib/tftpboot/"

void write_file(int sockfd, struct sockaddr_in addr, char* filename) {
    int n;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    printf("[INFO] Réception du fichier : %s\n", filepath);

    FILE* fp = fopen(filepath, "w");
    if (fp == NULL) {
        perror("[ERROR] Impossible de créer le fichier.");
        return;
    }

    while (1) {
        memset(buffer, 0, SIZE);
        n = recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        if (n <= 0 || strcmp(buffer, "END") == 0) {
            printf("[INFO] Fin de la transmission.\n");
            break;
        }
        fwrite(buffer, 1, n, fp);
    }

    fclose(fp);
}

void read_file(int sockfd, struct sockaddr_in addr, char* filename) {
    int n;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    printf("[INFO] Envoi du fichier : %s\n", filepath);

    FILE* fp = fopen(filepath, "r");
    if (fp == NULL) {
        perror("[ERROR] Fichier introuvable.");
        return;
    }

    while (fgets(buffer, SIZE, fp) != NULL) {
        sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr*)&addr, addr_size);
    }

    strcpy(buffer, "END");
    sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr*)&addr, addr_size);
    fclose(fp);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char* ip = argv[1];
    const int port = 8080;
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
    server_addr.sin_addr.s_addr = inet_addr(ip);

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[ERROR] Échec du bind.");
        exit(1);
    }

    printf("[STARTING] Serveur TFTP UDP en attente...\n");

    while (1) {
        memset(buffer, 0, SIZE);
        recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&client_addr, &addr_size);

        if (strncmp(buffer, "put ", 4) == 0) {
            char filename[SIZE];
            strcpy(filename, buffer + 4);
            write_file(sockfd, client_addr, filename);
        } 
        else if (strncmp(buffer, "get ", 4) == 0) {
            char filename[SIZE];
            strcpy(filename, buffer + 4);
            read_file(sockfd, client_addr, filename);
        }
        else if (strncmp(buffer, "quit ", 4) == 0) {
            break;
        }
        else {
            printf("[ERROR] Commande invalide reçue : %s\n", buffer);
        }
    }

    printf("[CLOSING] Fermeture du serveur.\n");
    close(sockfd);
    return 0;
}
