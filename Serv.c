#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>

#define DATA_SIZE 512
#define PACKET_SIZE (DATA_SIZE + 4)
#define MAX_RETRIES 5

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

void send_error(int sockfd, struct sockaddr_in addr, int error_code, char* msg) {
    char buffer[PACKET_SIZE];
    buffer[0] = 0;
    buffer[1] = ERROR;
    buffer[2] = (error_code >> 8) & 0xFF;
    buffer[3] = error_code & 0xFF;
    strcpy(buffer + 4, msg);
    sendto(sockfd, buffer, 4 + strlen(msg) + 1, 0, (struct sockaddr*)&addr, sizeof(addr));
    printf("[INFO] ERROR envoyé : %s\n", msg);
}

void send_file(int sockfd, struct sockaddr_in addr, char* filename) {
    int n, block_num = 1;
    char buffer[PACKET_SIZE];
    socklen_t addr_size = sizeof(addr);

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    FILE* fp = fopen(filepath, "rb");
    if (fp == NULL) {
        perror("[ERROR] Fichier introuvable.");
        send_error(sockfd, addr, 1, "Fichier introuvable");
        return;
    }

    printf("[INFO] Début d'envoi du fichier : %s\n", filename);

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

        int retries = 0;
        while (retries < MAX_RETRIES) {
            sendto(sockfd, buffer, packet_len, 0, (struct sockaddr*)&addr, addr_size);
            int recv_len = recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&addr, &addr_size);
            if (recv_len < 0) {
                retries++;
                continue;
            }
            int opcode = (buffer[0] << 8) | buffer[1];
            int ack_block = (buffer[2] << 8) | buffer[3];
            if (opcode == ACK && ack_block == block_num) {
                break;
            } else {
                retries++;
            }
        }
        if (retries == MAX_RETRIES) {
            fprintf(stderr, "[ERROR] Nombre maximum de retransmissions atteint pour le bloc %d\n", block_num);
            fclose(fp);
            return;
        }
        printf("[INFO] DATA envoyé - Bloc %d (%d octets)\n", block_num, n);
        block_num++;
        // Fin de fichier si on a lu moins de 512 octets
        if (n < DATA_SIZE) break;
    }
    fclose(fp);
    printf("[INFO] Fin d'envoi du fichier : %s\n", filename);
}

void receive_file(int sockfd, struct sockaddr_in addr, char* filename) {
    int n, expected_block = 1;
    char buffer[PACKET_SIZE];
    socklen_t addr_size = sizeof(addr);

    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    FILE* fp = fopen(filepath, "wb");
    if (fp == NULL) {
        perror("[ERROR] Impossible de créer le fichier.");
        send_error(sockfd, addr, 2, "Impossible de créer le fichier");
        return;
    }

    // Envoi immédiat de l'ACK pour le bloc 0 (acceptation de la WRQ)
    send_ack(sockfd, addr, 0);

    // Configuration du timeout de réception (3 secondes)
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    while (1) {
        n = recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        if (n < 0) {
            fprintf(stderr, "[ERROR] Délai d'attente dépassé pour la réception du bloc %d\n", expected_block);
            fclose(fp);
            remove(filepath);
            return;
        }
        if (n < 4) {
            fprintf(stderr, "[ERROR] Erreur lors de la réception.\n");
            fclose(fp);
            remove(filepath);
            return;
        }
        int opcode = (buffer[0] << 8) | buffer[1];
        int block_num = (buffer[2] << 8) | buffer[3];

        if (opcode == ERROR) {
            printf("[ERROR] Erreur du client : %s\n", buffer + 4);
            fclose(fp);
            remove(filepath);
            return;
        }
        if (opcode == DATA) {
            if (block_num == expected_block) {
                int data_len = n - 4; // nombre d’octets de données
                fwrite(buffer + 4, 1, data_len, fp);
                printf("[INFO] DATA reçu - Bloc %d (%d octets)\n", block_num, data_len);
                send_ack(sockfd, addr, block_num);
                expected_block++;
                if (data_len < DATA_SIZE) break;
            } else if (block_num < expected_block) {
                // Paquet en double, renvoyer l'ACK
                send_ack(sockfd, addr, block_num);
            } else {
                // Bloc inattendu, on l'ignore
                continue;
            }
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
    char buffer[PACKET_SIZE];

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
        memset(buffer, 0, PACKET_SIZE);
        recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&client_addr, &addr_size);

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