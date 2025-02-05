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

    // 1. Envoyer le nom du fichier
    n = sendto(sockfd, filename, strlen(filename), 0, (struct sockaddr*)&addr, sizeof(addr));
    if (n == -1)
    {
        perror("[ERROR] Échec de l'envoi du nom du fichier.");
        exit(1);
    }
    printf("[SENDING] Nom du fichier envoyé : %s\n", filename);

    // 2. Envoi des données du fichier
    while (fgets(buffer, SIZE, fp) != NULL)
    {
        n = sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr*)&addr, sizeof(addr));
        if (n == -1)
        {
            perror("[ERROR] Échec de l'envoi des données.");
            exit(1);
        }
        printf("[SENDING] Data envoyée : %s", buffer);
        memset(buffer, 0, SIZE);
    }

    // 3. Envoyer le signal de fin
    strcpy(buffer, "END");
    sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr*)&addr, sizeof(addr));

    fclose(fp);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <file_name> \n", argv[0]);
        exit(EXIT_FAILURE);
    }
    // Définition du serveur
    char *ip = argv[1];
    const int port = 8080;
    int server_sockfd;
    struct sockaddr_in server_addr;

    char input[256];
/*
    while(1) {
        printf("tftp> ");

        if (fgets(input, sizeof(input), stdin) != NULL) {
            input[strcspn(input, "\n")] = '\0';
            //printf("%s\n", input);
            if (strcmp(input, "quit") == 0) {
                exit(0);
            }
        }
    }
*/
    char *filename = argv[2];
    FILE *fp = fopen(filename, "r");

    // Création du socket UDP
    server_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_sockfd < 0)
    {
        perror("[ERROR] Échec de la création du socket.");
        exit(1);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = port;
    server_addr.sin_addr.s_addr = inet_addr(ip);

    // Vérification de l'ouverture du fichier
    if (fp == NULL)
    {
        perror("[ERROR] Impossible d'ouvrir le fichier.");
        exit(1);
    }

    // Envoyer les données au serveur
    send_file_data(fp, server_sockfd, server_addr, filename);

    printf("[SUCCESS] Transfert terminé.\n");
    printf("[CLOSING] Déconnexion du serveur.\n");

    close(server_sockfd);
    return 0;
}
