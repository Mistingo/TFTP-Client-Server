#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>

#define DATA_SIZE 512
#define PACKET_SIZE (DATA_SIZE + 4)
#define MAX_RETRIES 5

#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERROR 5

// Fonctions de gestion du verrou par fichier
int check_lock(const char *filename) {
    char lock_filename[300];
    snprintf(lock_filename, sizeof(lock_filename), "%s.lock", filename);
    if (access(lock_filename, F_OK) == 0) {
         return 1; // verrou existant
    }
    return 0;
}

void add_lock(const char *filename) {
    char lock_filename[300];
    snprintf(lock_filename, sizeof(lock_filename), "%s.lock", filename);
    FILE *fp = fopen(lock_filename, "w");
    if (fp) {
        fclose(fp);
    }
}

void remove_lock(const char *filename) {
    char lock_filename[300];
    snprintf(lock_filename, sizeof(lock_filename), "%s.lock", filename);
    unlink(lock_filename);
}

// Envoi d'un ACK
void send_ack(int sockfd, struct sockaddr_in addr, int block_num) {
    char ack[4];
    ack[0] = 0;
    ack[1] = ACK;
    ack[2] = (block_num >> 8) & 0xFF;
    ack[3] = block_num & 0xFF;
    sendto(sockfd, ack, sizeof(ack), 0, (struct sockaddr*)&addr, sizeof(addr));
}

// Envoi d'un paquet d'erreur
void send_error(int sockfd, struct sockaddr_in addr, int error_code, char* msg) {
    char buffer[PACKET_SIZE];
    memset(buffer, 0, PACKET_SIZE);
    buffer[0] = 0;
    buffer[1] = ERROR;
    buffer[2] = (error_code >> 8) & 0xFF;
    buffer[3] = error_code & 0xFF;
    strcpy(buffer + 4, msg);
    sendto(sockfd, buffer, 4 + strlen(msg) + 1, 0, (struct sockaddr*)&addr, sizeof(addr));
}

// Envoi d'un fichier (PUT)
void do_tftp_put(const char* server_ip, const char* filename) {
    // Vérifier si un transfert sur ce fichier est déjà en cours
    if (check_lock(filename)) {
        printf("tftp> Erreur: Un transfert pour '%s' est déjà en cours.\n", filename);
        return;
    }
    add_lock(filename);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("tftp> Échec de création du socket (put).");
        remove_lock(filename);
        return;
    }
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(6969);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);

    FILE* fp = fopen(filename, "rb");
    if (!fp) {
        perror("tftp> Impossible d'ouvrir le fichier en lecture.");
        close(sockfd);
        remove_lock(filename);
        return;
    }
    char request[PACKET_SIZE];
    int req_len = sprintf(request, "%c%c%s%c%s%c", 0, WRQ, filename, 0, "octet", 0);
    sendto(sockfd, request, req_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));

    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    char response[PACKET_SIZE];
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int n = recvfrom(sockfd, response, PACKET_SIZE, 0, (struct sockaddr*)&from_addr, &from_len);
    if (n < 4) {
        printf("tftp> Le serveur n'a pas confirmé l'écriture (ACK(0) non reçu).\n");
        fclose(fp);
        close(sockfd);
        remove_lock(filename);
        return;
    }
    if (((response[0] << 8) | response[1]) != ACK || ((response[2] << 8) | response[3]) != 0) {
        printf("tftp> Le serveur n'a pas confirmé l'écriture du fichier (ACK(0) attendu).\n");
        fclose(fp);
        close(sockfd);
        remove_lock(filename);
        return;
    }
    server_addr.sin_port = from_addr.sin_port;

    int block_num = 1;
    char buffer_data[PACKET_SIZE];
    int retries;
    while (1) {
        memset(buffer_data, 0, PACKET_SIZE);
        buffer_data[1] = DATA;
        buffer_data[2] = (block_num >> 8) & 0xFF;
        buffer_data[3] = block_num & 0xFF;
        int bytes_read = fread(buffer_data + 4, 1, DATA_SIZE, fp);
        int packet_len = bytes_read + 4;
        retries = 0;
        while (retries < MAX_RETRIES) {
            sendto(sockfd, buffer_data, packet_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
            sleep(1);
            int recv_len = recvfrom(sockfd, buffer_data, PACKET_SIZE, 0, (struct sockaddr*)&from_addr, &from_len);
            if (recv_len < 0) {
                retries++;
                continue;
            }
            int opcode = (buffer_data[0] << 8) | buffer_data[1];
            int ack_block = (buffer_data[2] << 8) | buffer_data[3];
            if (opcode == ACK && ack_block == block_num)
                break;
            else
                retries++;
        }
        if (retries == MAX_RETRIES) {
            fprintf(stderr, "tftp> Erreur: retransmissions max atteintes pour le bloc %d\n", block_num);
            fclose(fp);
            close(sockfd);
            remove_lock(filename);
            return;
        }
        block_num++;
        if (bytes_read < DATA_SIZE)
            break;
    }
    fclose(fp);
    close(sockfd);
    remove_lock(filename);
}

// Réception d'un fichier (GET)
void do_tftp_get(const char* server_ip, const char* filename) {
    if (check_lock(filename)) {
        printf("tftp> Erreur: Un transfert pour '%s' est déjà en cours.\n", filename);
        return;
    }
    add_lock(filename);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("tftp> Échec de création du socket (get).");
        remove_lock(filename);
        return;
    }
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(6969);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);

    FILE* fp = fopen(filename, "wb");
    if (!fp) {
        perror("tftp> Impossible de créer le fichier local.");
        close(sockfd);
        remove_lock(filename);
        return;
    }
    char request[PACKET_SIZE];
    int req_len = sprintf(request, "%c%c%s%c%s%c", 0, RRQ, filename, 0, "octet", 0);
    sendto(sockfd, request, req_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int expected_block = 1;
    while (1) {
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        char buffer_data[PACKET_SIZE];
        int n = recvfrom(sockfd, buffer_data, PACKET_SIZE, 0, (struct sockaddr*)&from_addr, &from_len);
        if (n < 0) {
            fprintf(stderr, "tftp> Timeout ou erreur de réception (bloc %d)\n", expected_block);
            fclose(fp);
            remove(filename);
            close(sockfd);
            remove_lock(filename);
            return;
        }
        if (n < 4) {
            fprintf(stderr, "tftp> Paquet DATA trop court.\n");
            fclose(fp);
            remove(filename);
            close(sockfd);
            remove_lock(filename);
            return;
        }
        server_addr.sin_port = from_addr.sin_port;
        int opcode = (buffer_data[0] << 8) | buffer_data[1];
        int block_num = (buffer_data[2] << 8) | buffer_data[3];
        if (opcode == ERROR) {
            printf("tftp> Erreur du serveur : %s\n", buffer_data + 4);
            fclose(fp);
            remove(filename);
            close(sockfd);
            remove_lock(filename);
            return;
        }
        if (opcode == DATA) {
            if (block_num == expected_block) {
                int data_len = n - 4;
                fwrite(buffer_data + 4, 1, data_len, fp);
                send_ack(sockfd, server_addr, block_num);
                expected_block++;
                if (data_len < DATA_SIZE)
                    break;
            } else if (block_num < expected_block) {
                send_ack(sockfd, server_addr, block_num);
            } else {
                continue;
            }
        }
    }
    fclose(fp);
    close(sockfd);
    remove_lock(filename);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    char* server_ip = argv[1];
    char command[256], filename[256];
    while (1) {
        printf("tftp> ");
        if (!fgets(command, sizeof(command), stdin))
            break;
        command[strcspn(command, "\n")] = 0;
        if (strncmp(command, "put ", 4) == 0) {
            strcpy(filename, command + 4);
            do_tftp_put(server_ip, filename);
        }
        else if (strncmp(command, "get ", 4) == 0) {
            strcpy(filename, command + 4);
            do_tftp_get(server_ip, filename);
        }
        else if (strcmp(command, "quit") == 0) {
            break;
        }
        else {
            printf("tftp> Commande invalide.\n");
        }
    }
    return 0;
}