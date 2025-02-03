#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SIZE 516
#define TFTP_DIR "/var/lib/tftpboot/"  // Dossier où seront stockés les fichiers reçus

void write_file(int sockfd, struct sockaddr_in addr)
{
    int n;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);
    

    // Recevoir le nom du fichier
    n = recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
    if (n <= 0)
    {
        perror("[ERROR] Échec de la réception du nom du fichier.");
        return;
    }

    // Construire le chemin complet du fichier dans /tftpboot
    buffer[n] = '\0';  // Assurer la fin de chaîne
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, buffer);
    printf("[INFO] Fichier reçu : %s\n", filepath);

    // Ouvrir le fichier pour écriture
    FILE* fp = fopen(filepath, "w");
    if (fp == NULL)
    {
        perror("[ERROR] Impossible de créer le fichier.");
        return;
    }

    // Réception des données et écriture dans le fichier
    while (1)
    {
        memset(buffer, 0, SIZE);
        n = recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        if (n <= 0)
        {
            perror("[ERROR] Problème de réception.");
            break;
        }

        // Vérifier si l'envoi est terminé
        if (strcmp(buffer, "END") == 0)
        {
            printf("[INFO] Fin de la transmission.\n");
            break;
        }

        // Écriture dans le fichier
        fwrite(buffer, 1, n, fp);
        printf("[RECEIVING] Data écrit dans le fichier...\n");
    }

    fclose(fp);
}

int main()
{
    // Définition du serveur
    char* ip = "127.0.0.1";
    const int port = 8080;
    int server_sockfd;
    struct sockaddr_in server_addr, client_addr;

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

    // Liaison du socket
    if (bind(server_sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("[ERROR] Échec du bind.");
        exit(1);
    }

    printf("[STARTING] Serveur TFTP UDP démarré.\n");

    // Attente de la réception du fichier
    write_file(server_sockfd, client_addr);

    printf("[SUCCESS] Transfert terminé.\n");
    printf("[CLOSING] Fermeture du serveur.\n");

    close(server_sockfd);
    return 0;
}
