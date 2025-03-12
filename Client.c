#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>

#define DATA_SIZE 512               // Taille maximale des données dans un paquet TFTP
#define PACKET_SIZE (DATA_SIZE + 4)   // 4 octets pour l'en-tête TFTP
#define MAX_RETRIES 5               // Nombre maximal de retransmissions

// Codes d'opération TFTP
#define RRQ 1   // Read Request (demande de lecture)
#define WRQ 2   // Write Request (demande d'écriture)
#define DATA 3  // Paquet DATA
#define ACK 4   // Accusé de réception
#define ERROR 5 // Message d'erreur

// ---------------------- Gestion des verrous sur fichier ----------------------

// Vérifie l'existence d'un fichier de verrou (filename.lock)
int check_lock(const char *filename) {
    char lock_filename[300];
    snprintf(lock_filename, sizeof(lock_filename), "%s.lock", filename);
    return (access(lock_filename, F_OK) == 0);
}

// Crée un fichier de verrou indiquant qu'un transfert est en cours
void add_lock(const char *filename) {
    char lock_filename[300];
    snprintf(lock_filename, sizeof(lock_filename), "%s.lock", filename);
    FILE *fp = fopen(lock_filename, "w");
    if (fp) { fclose(fp); }
}

// Supprime le fichier de verrou à la fin du transfert
void remove_lock(const char *filename) {
    char lock_filename[300];
    snprintf(lock_filename, sizeof(lock_filename), "%s.lock", filename);
    unlink(lock_filename);
}

// ---------------------- Fonctions d'envoi de paquets ----------------------

// Envoi d'un ACK pour un bloc donné
void send_ack(int sockfd, struct sockaddr_in server_addr, int block_num) {
    char ack[4];
    ack[0] = 0;
    ack[1] = ACK;
    ack[2] = (block_num >> 8) & 0xFF;
    ack[3] = block_num & 0xFF;
    sendto(sockfd, ack, sizeof(ack), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
}

// ---------------------- Transfert en PUT (envoi vers le serveur) ----------------------

void do_tftp_put(int sockfd, struct sockaddr_in server_addr, char* filename) {
    // Vérification du verrou pour éviter un transfert simultané sur le même fichier
    if (check_lock(filename)) {
        printf("tftp> Erreur: Un transfert pour '%s' est déjà en cours.\n", filename);
        return;
    }
    add_lock(filename);

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("tftp> Impossible d'ouvrir le fichier en lecture.");
        remove_lock(filename);
        return;
    }
    
    // Construction et envoi de la requête WRQ
    char request[PACKET_SIZE];
    int req_len = sprintf(request, "%c%c%s%c%s%c", 0, WRQ, filename, 0, "octet", 0);
    sendto(sockfd, request, req_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));

    // Configuration d'un timeout de 3 secondes pour la réception
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Attente de l'ACK pour le bloc 0 (confirmation du serveur)
    char response[PACKET_SIZE];
    socklen_t addr_size = sizeof(server_addr);
    int n = recvfrom(sockfd, response, PACKET_SIZE, 0,
                     (struct sockaddr*)&server_addr, &addr_size);
    if (n < 4) {
        printf("tftp> Le serveur n'a pas confirmé l'écriture (ACK(0) non reçu).\n");
        fclose(fp);
        remove_lock(filename);
        return;
    }
    int resp_opcode = (response[0] << 8) | response[1];
    int resp_block  = (response[2] << 8) | response[3];
    if (resp_opcode != ACK || resp_block != 0) {
        printf("tftp> Le serveur n'a pas confirmé l'écriture (ACK(0) attendu).\n");
        fclose(fp);
        remove_lock(filename);
        return;
    }
    
    // Envoi des données en blocs de 512 octets
    int block_num = 1;
    char buffer[PACKET_SIZE];
    while (1) {
        memset(buffer, 0, PACKET_SIZE);
        buffer[0] = 0;
        buffer[1] = DATA;
        buffer[2] = (block_num >> 8) & 0xFF;
        buffer[3] = block_num & 0xFF;
        int bytes_read = fread(buffer + 4, 1, DATA_SIZE, fp);
        int packet_len = bytes_read + 4;
        int retries = 0;
        
        // Boucle de retransmission en cas de non-réception de l'ACK
        while (retries < MAX_RETRIES) {
            sendto(sockfd, buffer, packet_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
            sleep(1);
            int recv_len = recvfrom(sockfd, buffer, PACKET_SIZE, 0,
                                    (struct sockaddr*)&server_addr, &addr_size);
            if (recv_len < 0) {
                retries++;
                continue;
            }
            int ack_opcode = (buffer[0] << 8) | buffer[1];
            int ack_block  = (buffer[2] << 8) | buffer[3];
            if (ack_opcode == ACK && ack_block == block_num)
                break;
            else
                retries++;
        }
        if (retries == MAX_RETRIES) {
            fprintf(stderr, "tftp> Erreur: retransmissions max atteintes pour le bloc %d\n", block_num);
            fclose(fp);
            remove_lock(filename);
            return;
        }
        block_num++;
        if (bytes_read < DATA_SIZE)
            break;  // Fin du fichier
    }
    fclose(fp);
    remove_lock(filename);
}

// ---------------------- Transfert en GET (réception depuis le serveur) ----------------------

void do_tftp_get(int sockfd, struct sockaddr_in server_addr, char* filename) {
    // Vérification du verrou pour éviter un transfert simultané sur le même fichier
    if (check_lock(filename)) {
        printf("tftp> Erreur: Un transfert pour '%s' est déjà en cours.\n", filename);
        return;
    }
    add_lock(filename);
    
    // Construction et envoi de la requête RRQ
    char request[PACKET_SIZE];
    int req_len = sprintf(request, "%c%c%s%c%s%c", 0, RRQ, filename, 0, "octet", 0);
    sendto(sockfd, request, req_len, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
    
    // Configuration d'un timeout de 3 secondes pour la réception
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    // Ouverture du fichier local pour écriture
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("tftp> Impossible de créer le fichier local.");
        remove_lock(filename);
        return;
    }
    
    int expected_block = 1;
    char buffer[PACKET_SIZE];
    socklen_t addr_size = sizeof(server_addr);
    while (1) {
        int n = recvfrom(sockfd, buffer, PACKET_SIZE, 0,
                         (struct sockaddr*)&server_addr, &addr_size);
        if (n < 0) {
            fprintf(stderr, "tftp> Timeout ou erreur lors de la réception du bloc %d\n", expected_block);
            fclose(fp);
            remove(filename);  // Supprime le fichier incomplet
            remove_lock(filename);
            return;
        }
        if (n < 4) {
            fprintf(stderr, "tftp> Paquet DATA trop court.\n");
            fclose(fp);
            remove(filename);
            remove_lock(filename);
            return;
        }
        int opcode = (buffer[0] << 8) | buffer[1];
        int block_num = (buffer[2] << 8) | buffer[3];
        if (opcode == ERROR) {
            printf("tftp> Erreur du serveur : %s\n", buffer + 4);
            fclose(fp);
            remove(filename);
            remove_lock(filename);
            return;
        }
        if (opcode == DATA) {
            if (block_num == expected_block) {
                int data_len = n - 4;
                fwrite(buffer + 4, 1, data_len, fp);
                send_ack(sockfd, server_addr, block_num);
                expected_block++;
                if (data_len < DATA_SIZE)
                    break;  // Fin du transfert
            } else if (block_num < expected_block) {
                // Bloc déjà reçu, renvoyer l'ACK
                send_ack(sockfd, server_addr, block_num);
            } else {
                continue;  // Bloc inattendu, on l'ignore
            }
        }
    }
    fclose(fp);
    remove_lock(filename);
}

// ---------------------- Fonction principale ----------------------

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    char *ip = argv[1];
    const int port = 6969;
    int sockfd;
    struct sockaddr_in server_addr;
    char command[256], filename[256];

    // Création de la socket UDP (unique pour toute la session)
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("tftp> Échec de la création du socket.");
        exit(EXIT_FAILURE);
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);
    
    // Boucle interactive de commandes
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