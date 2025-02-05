#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SIZE 516
#define TFTP_DIR "/var/lib/tftpboot/"  // Dossier où sont stockés les fichiers

void read_file(int sockfd, struct sockaddr_in addr)
{
    int n;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);

    // Réception du nom du fichier demandé
    n = recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
    if (n <= 0) {
        perror("[ERROR] Échec de la réception du nom du fichier.");
        return;
    }

    buffer[n] = '\0';
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, buffer);
    printf("[INFO] Demande de fichier reçue : %s\n", filepath);

    // Ouverture du fichier pour lecture
    FILE* fp = fopen(filepath, "r");
    if (fp == NULL) {
        perror("[ERROR] Fichier introuvable.");
        return;
    }

    // Envoi du contenu du fichier
    while (fgets(buffer, SIZE, fp) != NULL) {
        sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr*)&addr, addr_size);
        printf("[SENDING] Data envoyée : %s", buffer);
        memset(buffer, 0, SIZE);
    }

    // Signal de fin
    strcpy(buffer, "END");
    sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr*)&addr, addr_size);
    printf("[INFO] Fin de la transmission du fichier.\n");
    fclose(fp);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *ip = argv[1];
    const int port = 8080;
    int server_sockfd;
    struct sockaddr_in server_addr, client_addr;

    server_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_sockfd < 0) {
        perror("[ERROR] Échec de la création du socket.");
        exit(1);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);

    if (bind(server_sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[ERROR] Échec du bind.");
        exit(1);
    }

    printf("[STARTING] Serveur TFTP UDP démarré.\n");

    read_file(server_sockfd, client_addr);

    printf("[SUCCESS] Transfert terminé.\n");
    printf("[CLOSING] Fermeture du serveur.\n");

    close(server_sockfd);
    return 0;
}
