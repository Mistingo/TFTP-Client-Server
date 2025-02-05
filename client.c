#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SIZE 1024

void send_file_data(FILE* fp, int sockfd, struct sockaddr_in addr, char* filename)
{
    int n;
    char buffer[SIZE];

    // Envoi du nom du fichier
    n = sendto(sockfd, filename, strlen(filename), 0, (struct sockaddr*)&addr, sizeof(addr));
    if (n == -1) {
        perror("[ERROR] Échec de l'envoi du nom du fichier.");
        exit(1);
    }
    printf("[SENDING] Nom du fichier envoyé : %s\n", filename);

    // Envoi des données du fichier
    while (fgets(buffer, SIZE, fp) != NULL) {
        n = sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr*)&addr, sizeof(addr));
        if (n == -1) {
            perror("[ERROR] Échec de l'envoi des données.");
            exit(1);
        }
        printf("[SENDING] Data envoyée : %s", buffer);
        memset(buffer, 0, SIZE);
    }

    // Signal de fin
    strcpy(buffer, "END");
    sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr*)&addr, sizeof(addr));

    fclose(fp);
}

void receive_file_data(int sockfd, struct sockaddr_in addr, char* filename)
{
    int n;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);

    // Envoi du nom du fichier demandé
    sendto(sockfd, filename, strlen(filename), 0, (struct sockaddr*)&addr, sizeof(addr));
    printf("[REQUEST] Demande de fichier : %s\n", filename);

    // Ouverture du fichier en écriture
    FILE* fp = fopen(filename, "w");
    if (fp == NULL) {
        perror("[ERROR] Impossible de créer le fichier.");
        return;
    }

    // Réception des données
    while (1) {
        memset(buffer, 0, SIZE);
        n = recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        if (n <= 0) {
            perror("[ERROR] Problème de réception.");
            break;
        }

        if (strcmp(buffer, "END") == 0) {
            printf("[INFO] Fin de la transmission.\n");
            break;
        }

        fwrite(buffer, 1, n, fp);
        printf("[RECEIVING] Data écrit dans le fichier...\n");
    }

    fclose(fp);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <file_name>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *ip = argv[1];
    const int port = 8080;
    int sockfd;
    struct sockaddr_in server_addr;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("[ERROR] Échec de la création du socket.");
        exit(1);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);

    receive_file_data(sockfd, server_addr, argv[2]);

    printf("[SUCCESS] Transfert terminé.\n");
    close(sockfd);
    return 0;
}