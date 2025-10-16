#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>

#define DATA_SIZE 512                  // Taille maximale des données
#define PACKET_SIZE 518                // Suffisant pour 6 octets d'en-tête + 512 octets de données
#define MAX_RETRIES 5                  // Nombre maximal de retransmissions

// Codes d'opération TFTP
#define RRQ 1    // Read Request
#define WRQ 2    // Write Request
#define DATA 3   // Paquet DATA
#define ACK 4    // Accusé de réception
#define ERROR 5  // Message d'erreur
#define OACK 6   // Option Acknowledgment

// Variables globales pour la négociation BIGFILE
int client_bigfile = 0;  // 0 = mode standard, 1 = mode BIGFILE activé
int header_size = 4;     // Par défaut 4 octets ; en mode BIGFILE, 6

// Fonctions de gestion des verrous sur fichier
int check_lock(const char *filename) {
    char lock_filename[300];
    snprintf(lock_filename, sizeof(lock_filename), "%s.lock", filename);
    return (access(lock_filename, F_OK) == 0);
}
void add_lock(const char *filename) {
    char lock_filename[300];
    snprintf(lock_filename, sizeof(lock_filename), "%s.lock", filename);
    FILE *fp = fopen(lock_filename, "w");
    if (fp) { fclose(fp); }
}
void remove_lock(const char *filename) {
    char lock_filename[300];
    snprintf(lock_filename, sizeof(lock_filename), "%s.lock", filename);
    unlink(lock_filename);
}

// Construction de la requête TFTP : on inclut l'option "bigfile" seulement si client_bigfile est activé.
int build_request(char *buffer, int opcode, const char *filename) {
    int pos = 0;
    buffer[pos++] = 0;
    buffer[pos++] = (char)opcode;
    strcpy(&buffer[pos], filename);
    pos += strlen(filename);
    buffer[pos++] = '\0';
    strcpy(&buffer[pos], "octet");
    pos += strlen("octet");
    buffer[pos++] = '\0';
    if (client_bigfile) {
        strcpy(&buffer[pos], "bigfile");
        pos += strlen("bigfile");
        buffer[pos++] = '\0';
        strcpy(&buffer[pos], "yes");
        pos += strlen("yes");
        buffer[pos++] = '\0';
    }
    return pos;
}

// Extraction du numéro de bloc selon le mode
unsigned int parse_block(char *buffer) {
    if (client_bigfile) {
        unsigned int block = 0;
        block |= ((unsigned char)buffer[2]) << 24;
        block |= ((unsigned char)buffer[3]) << 16;
        block |= ((unsigned char)buffer[4]) << 8;
        block |= ((unsigned char)buffer[5]);
        return block;
    } else {
        unsigned int block = 0;
        block |= ((unsigned char)buffer[2]) << 8;
        block |= ((unsigned char)buffer[3]);
        return block;
    }
}

// Envoi d'un ACK adapté au mode
void send_ack_custom(int sockfd, struct sockaddr_in server_addr, unsigned int block_num) {
    char ack[8];
    if (client_bigfile) {
        ack[0] = 0;
        ack[1] = ACK;
        ack[2] = (block_num >> 24) & 0xFF;
        ack[3] = (block_num >> 16) & 0xFF;
        ack[4] = (block_num >> 8) & 0xFF;
        ack[5] = block_num & 0xFF;
        sendto(sockfd, ack, 6, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
    } else {
        ack[0] = 0;
        ack[1] = ACK;
        ack[2] = (block_num >> 8) & 0xFF;
        ack[3] = block_num & 0xFF;
        sendto(sockfd, ack, 4, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
    }
}

// Transfert en PUT (envoi vers le serveur)
void do_tftp_put(int sockfd, struct sockaddr_in server_addr, char* filename) {
    if (check_lock(filename)) {
        printf("tftp> Erreur : Un transfert pour '%s' est déjà en cours.\n", filename);
        return;
    }
    add_lock(filename);
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("tftp> Impossible d'ouvrir le fichier en lecture.");
        remove_lock(filename);
        return;
    }
    // Vérification de la taille du fichier (pour PUT, côté client)
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);
    if (fsize > 65000 && client_bigfile == 0) {
        printf("tftp> Erreur : fichier trop grand (%ld octets) – activez le mode BIGFILE.\n", fsize);
        fclose(fp);
        remove_lock(filename);
        return;
    }
    
    char request[PACKET_SIZE];
    int req_len = build_request(request, WRQ, filename);
    sendto(sockfd, request, req_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
    
    struct timeval tv;
    tv.tv_sec = 3; tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    char response[PACKET_SIZE];
    socklen_t addr_size = sizeof(server_addr);
    int n = recvfrom(sockfd, response, PACKET_SIZE, 0, (struct sockaddr*)&server_addr, &addr_size);
    if (n < 4) {
        printf("tftp> Le serveur n'a pas confirmé l'écriture (ACK(0)/OACK non reçu).\n");
        fclose(fp);
        remove_lock(filename);
        return;
    }
    int resp_opcode = (response[0] << 8) | (unsigned char)response[1];
    if (resp_opcode == OACK) {
        client_bigfile = 1;
        header_size = 6;
        printf("tftp> OACK reçu.\n");
        char ack[8];
        ack[0] = 0; ack[1] = ACK;
        ack[2] = 0; ack[3] = 0; ack[4] = 0; ack[5] = 0;
        sendto(sockfd, ack, 6, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
    } else if (resp_opcode == ACK) {
        int resp_block = ((unsigned char)response[2] << 8) | ((unsigned char)response[3]);
        if (resp_block != 0) {
            printf("tftp> Le serveur n'a pas confirmé l'écriture (ACK(0) attendu).\n");
            fclose(fp);
            remove_lock(filename);
            return;
        }
        client_bigfile = 0;
        header_size = 4;
    } else {
        printf("tftp> Réponse inattendue du serveur.\n");
        fclose(fp);
        remove_lock(filename);
        return;
    }
    
    unsigned int block_num = 1;
    char buffer[PACKET_SIZE];
    while (1) {
        memset(buffer, 0, PACKET_SIZE);
        int packet_len, bytes_read;
        if (client_bigfile) {
            buffer[0] = 0;
            buffer[1] = DATA;
            buffer[2] = (block_num >> 24) & 0xFF;
            buffer[3] = (block_num >> 16) & 0xFF;
            buffer[4] = (block_num >> 8) & 0xFF;
            buffer[5] = block_num & 0xFF;
            bytes_read = fread(buffer + 6, 1, DATA_SIZE, fp);
            packet_len = bytes_read + 6;
        } else {
            buffer[0] = 0;
            buffer[1] = DATA;
            buffer[2] = (block_num >> 8) & 0xFF;
            buffer[3] = block_num & 0xFF;
            bytes_read = fread(buffer + 4, 1, DATA_SIZE, fp);
            packet_len = bytes_read + 4;
        }
        int retries = 0;
        while (retries < MAX_RETRIES) {
            sendto(sockfd, buffer, packet_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
            //sleep(1);
            int recv_len = recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&server_addr, &addr_size);
            if (recv_len < header_size) { retries++; continue; }
            int ack_opcode = (buffer[0] << 8) | (unsigned char)buffer[1];
            unsigned int ack_block = parse_block(buffer);
            if (ack_opcode == ACK && ack_block == block_num) break;
            else retries++;
        }
        if (retries == MAX_RETRIES) {
            fprintf(stderr, "tftp> Erreur : retransmissions max atteintes pour le bloc %u\n", block_num);
            fclose(fp);
            remove_lock(filename);
            return;
        }
        block_num++;
        if (bytes_read < DATA_SIZE)
            break;
    }
    fclose(fp);
    remove_lock(filename);
}

// Transfert en GET (téléchargement depuis le serveur)
void do_tftp_get(int sockfd, struct sockaddr_in server_addr, char* filename) {
    if (check_lock(filename)) {
        printf("tftp> Erreur : Un transfert pour '%s' est déjà en cours.\n", filename);
        return;
    }
    add_lock(filename);
    
    char request[PACKET_SIZE];
    int req_len = build_request(request, RRQ, filename);
    sendto(sockfd, request, req_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
    
    // Configure un timeout de 3 secondes
    struct timeval tv; 
    tv.tv_sec = 3; 
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    char buffer[PACKET_SIZE];
    socklen_t addr_size = sizeof(server_addr);
    int n = recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&server_addr, &addr_size);
    if (n < 4) {
        fprintf(stderr, "tftp> Réponse invalide du serveur.\n");
        remove_lock(filename);
        return;
    }
    
    int resp_opcode = (buffer[0] << 8) | (unsigned char)buffer[1];
    
    // Si le serveur renvoie une erreur (par exemple, fichier trop grand)
    if (resp_opcode == ERROR) {
        printf("tftp> Erreur du serveur : %s\n", buffer + 4);
        remove_lock(filename);
        return;
    }
    
    // À ce stade, la réponse est soit DATA, soit OACK
    if (resp_opcode == OACK) {
        client_bigfile = 1;
        header_size = 6;
        printf("tftp> OACK reçu.\n");
        char ack[8];
        ack[0] = 0; 
        ack[1] = ACK;
        ack[2] = 0; ack[3] = 0; ack[4] = 0; ack[5] = 0;
        sendto(sockfd, ack, 6, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
        // On attend le premier paquet DATA après l'OACK
        n = recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&server_addr, &addr_size);
    } else if (resp_opcode == DATA) {
        client_bigfile = 0;
        header_size = 4;
    } else {
        printf("tftp> Réponse inattendue du serveur.\n");
        remove_lock(filename);
        return;
    }
    
    // À présent, nous ouvrons le fichier pour écriture (on ne le fait qu'après avoir vérifié que le transfert peut se faire)
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("tftp> Impossible de créer le fichier local.");
        remove_lock(filename);
        return;
    }
    
    unsigned int expected_block = 1;
    while (1) {
        if (n < header_size) {
            fprintf(stderr, "tftp> Paquet DATA trop court.\n");
            fclose(fp);
            remove(filename);  // Supprime le fichier incomplet
            remove_lock(filename);
            return;
        }
        unsigned int block_num = parse_block(buffer);
        if (block_num == expected_block) {
            int data_len = n - header_size;
            fwrite(buffer + header_size, 1, data_len, fp);
            send_ack_custom(sockfd, server_addr, block_num);
            expected_block++;
            if (data_len < DATA_SIZE)
                break;  // Fin du transfert
        } else if (block_num < expected_block) {
            send_ack_custom(sockfd, server_addr, block_num);
        }
        n = recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&server_addr, &addr_size);
        if (n < 0) {
            fprintf(stderr, "tftp> Timeout ou erreur lors de la réception du bloc %u\n", expected_block);
            fclose(fp);
            remove(filename);
            remove_lock(filename);
            return;
        }
    }
    fclose(fp);
    remove_lock(filename);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage : %s <server_ip>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    char *ip = argv[1];
    const int port = 6969;
    int sockfd;
    struct sockaddr_in server_addr;
    char command[256], filename[256];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("tftp> Échec de la création du socket.");
        exit(EXIT_FAILURE);
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);
    
    while (1) {
        printf("tftp> ");
        if (!fgets(command, sizeof(command), stdin))
            break;
        command[strcspn(command, "\n")] = 0;
        
        if (strncmp(command, "put ", 4) == 0) {
            strcpy(filename, command + 4);
            do_tftp_put(sockfd, server_addr, filename);
        }
        else if (strncmp(command, "get ", 4) == 0) {
            strcpy(filename, command + 4);
            do_tftp_get(sockfd, server_addr, filename);
        }
        else if (strcmp(command, "bigfile") == 0) {
            client_bigfile = !client_bigfile;
            if (client_bigfile) {
                header_size = 6;
                printf("Mode BIGFILE activé.\n");
            } else {
                header_size = 4;
                printf("Mode BIGFILE désactivé.\n");
            }
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