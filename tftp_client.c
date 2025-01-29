#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

#define TFTP_PORT 69
#define BUFFER_SIZE 516 // 512 bytes data + 4 bytes header
#define TIMEOUT 5 // Timeout en secondes

// OpCodes TFTP
#define OP_RRQ 1
#define OP_DATA 3
#define OP_ACK 4
#define OP_ERROR 5

void die(const char *message) {
    perror(message);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <remote_file> <local_file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *server_ip = argv[1];
    const char *remote_file = argv[2];
    const char *local_file = argv[3];

    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(server_addr);
    char buffer[BUFFER_SIZE];
    FILE *fp;

    // Création du socket UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        die("Socket creation failed");
    }

    // Configuration de l'adresse du serveur
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TFTP_PORT);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        die("Invalid server IP address");
    }

    // Création de la requête RRQ
    int rrq_len = snprintf(buffer, sizeof(buffer), "%c%c%s%c%s%c", 
                           0, OP_RRQ, remote_file, 0, "octet", 0);
    if (sendto(sockfd, buffer, rrq_len, 0, (struct sockaddr *)&server_addr, addr_len) < 0) {
        die("Failed to send RRQ");
    }

    // Ouverture du fichier local pour écriture
    if ((fp = fopen(local_file, "wb")) == NULL) {
        die("Failed to open local file");
    }

    // Boucle de réception des données
    int block_num = 1;
    while (1) {
        // Réception d'un paquet
        memset(buffer, 0, sizeof(buffer));
        int recv_len = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&server_addr, &addr_len);
        if (recv_len < 0) {
            die("Failed to receive data");
        }

        // Vérification du type de paquet
        short opcode = ntohs(*(short *)buffer);
        if (opcode == OP_DATA) {
            short received_block_num = ntohs(*(short *)(buffer + 2));

            if (received_block_num == block_num) {
                // Écrire les données reçues dans le fichier
                fwrite(buffer + 4, 1, recv_len - 4, fp);

                // Envoyer un ACK pour le bloc reçu
                memset(buffer, 0, sizeof(buffer));
                *(short *)buffer = htons(OP_ACK);
                *(short *)(buffer + 2) = htons(block_num);
                if (sendto(sockfd, buffer, 4, 0, (struct sockaddr *)&server_addr, addr_len) < 0) {
                    die("Failed to send ACK");
                }

                // Si le paquet reçu est inférieur à 512 octets, fin de la transmission
                if (recv_len < BUFFER_SIZE) {
                    printf("File transfer complete.\n");
                    break;
                }

                block_num++;
            }
        } else if (opcode == OP_ERROR) {
            // Gestion des erreurs
            fprintf(stderr, "Error received from server: %s\n", buffer + 4);
            break;
        } else {
            fprintf(stderr, "Unexpected opcode: %d\n", opcode);
            break;
        }
    }

    // Fermeture du fichier et du socket
    fclose(fp);
    close(sockfd);
    return 0;
}
