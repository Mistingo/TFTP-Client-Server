#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>

#define DATA_SIZE 512               // Taille maximale de la portion "données" d'un paquet TFTP
#define PACKET_SIZE (DATA_SIZE + 4) // 4 octets d'en-tête TFTP + DATA_SIZE
#define MAX_RETRIES 5

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

void send_error(int sockfd, struct sockaddr_in addr, int error_code, char* msg) {
    char buffer[PACKET_SIZE];
    buffer[0] = 0;
    buffer[1] = ERROR;
    buffer[2] = (error_code >> 8) & 0xFF;
    buffer[3] = error_code & 0xFF;
    strcpy(buffer + 4, msg);
    sendto(sockfd, buffer, 4 + strlen(msg) + 1, 0, (struct sockaddr*)&addr, sizeof(addr));
}

void send_file_data(FILE* fp, int sockfd, struct sockaddr_in addr, char* filename) {
    int n, block_num = 1;
    char buffer[PACKET_SIZE];
    socklen_t addr_size = sizeof(addr);
    int retries;

    // Configuration du timeout de réception (3 secondes)
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    while (1) {
        memset(buffer, 0, PACKET_SIZE);
        buffer[0] = 0;
        buffer[1] = DATA;
        buffer[2] = (block_num >> 8) & 0xFF;
        buffer[3] = block_num & 0xFF;

        // Lecture de 512 octets max
        n = fread(buffer + 4, 1, DATA_SIZE, fp);
        int packet_len = n + 4;
        retries = 0;

        // Envoi et retransmission si nécessaire
        while (retries < MAX_RETRIES) {
            sendto(sockfd, buffer, packet_len, 0, (struct sockaddr*)&addr, sizeof(addr));
            int recv_len = recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&addr, &addr_size);
            if (recv_len < 0) { // délai expiré ou erreur
                retries++;
                continue;
            }
            int opcode = (buffer[0] << 8) | buffer[1];
            int ack_block = (buffer[2] << 8) | buffer[3];
            if (opcode == ACK && ack_block == block_num) {
                break; // ACK correct reçu
            } else {
                retries++;
            }
        }
        if (retries == MAX_RETRIES) {
            fprintf(stderr, "tftp> Erreur: Nombre maximum de retransmissions atteint pour le bloc %d\n", block_num);
            fclose(fp);
            return;
        }
        block_num++;
        // Si on a lu moins de 512 octets, c’est la fin du fichier
        if (n < DATA_SIZE) break;
    }
    fclose(fp);
}

void receive_file_data(int sockfd, struct sockaddr_in addr, char* filename) {
    int n, expected_block = 1;
    char buffer[PACKET_SIZE];
    socklen_t addr_size = sizeof(addr);

    FILE* fp = fopen(filename, "wb");
    if (fp == NULL) {
        perror("tftp> Impossible de créer le fichier.");
        return;
    }

    // Configuration du timeout de réception (3 secondes)
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    while (1) {
        n = recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        if (n < 0) {
            fprintf(stderr, "tftp> Erreur: délai d'attente dépassé pour le bloc %d\n", expected_block);
            fclose(fp);
            remove(filename);
            return;
        }
        if (n < 4) {
            fprintf(stderr, "tftp> Erreur lors de la réception.\n");
            fclose(fp);
            remove(filename);
            return;
        }
        int opcode = (buffer[0] << 8) | buffer[1];
        int block_num = (buffer[2] << 8) | buffer[3];

        if (opcode == ERROR) {
            printf("tftp> Erreur du serveur : %s\n", buffer + 4);
            fclose(fp);
            remove(filename);
            return;
        }
        if (opcode == DATA) {
            if (block_num == expected_block) {
                int data_len = n - 4; // nombre d’octets de données
                fwrite(buffer + 4, 1, data_len, fp);
                send_ack(sockfd, addr, block_num);
                expected_block++;
                // Fin de fichier si on a reçu < 512 octets de données
                if (data_len < DATA_SIZE) break;
            } else if (block_num < expected_block) {
                // Paquet en double, renvoyer l'ACK déjà envoyé
                send_ack(sockfd, addr, block_num);
            } else {
                // Bloc inattendu, on l'ignore
                continue;
            }
        }
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
    char command[256], filename[256]; // 256 suffisent pour les noms

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
        if (!fgets(command, sizeof(command), stdin)) {
            // En cas d'erreur ou de fin de stdin
            break;
        }
        command[strcspn(command, "\n")] = 0;

        if (strncmp(command, "put ", 4) == 0) {
            strcpy(filename, command + 4);
            FILE *fp = fopen(filename, "rb");
            if (fp == NULL) {
                perror("tftp> Impossible d'ouvrir le fichier.");
                continue;
            }
            // Construction du message WRQ
            char request[PACKET_SIZE];
            int req_len = sprintf(request, "%c%c%s%c%s%c", 0, WRQ, filename, 0, "octet", 0);
            sendto(sockfd, request, req_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));

            // Attente de l'ACK pour le bloc 0
            char response[PACKET_SIZE];
            socklen_t addr_size = sizeof(server_addr);
            int n = recvfrom(sockfd, response, PACKET_SIZE, 0, (struct sockaddr*)&server_addr, &addr_size);
            if (n < 4 || (((response[0] << 8) | response[1]) != ACK) || (((response[2] << 8) | response[3]) != 0)) {
                printf("tftp> Le serveur n'a pas confirmé l'écriture du fichier (ACK pour bloc 0 attendu).\n");
                fclose(fp);
                continue;
            }
            send_file_data(fp, sockfd, server_addr, filename);
        } 
        else if (strncmp(command, "get ", 4) == 0) {
            strcpy(filename, command + 4);
            // Construction du message RRQ
            char request[PACKET_SIZE];
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