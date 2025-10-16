#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#define DATA_SIZE 512                 
#define PACKET_SIZE 518               // Suffisant pour un en-tête de 6 octets + données
#define MAX_RETRIES 5               

#define MAX_SESSIONS 10
#define TIMEOUT_SEC 5

// OpCodes TFTP
#define RRQ 1    // Read Request
#define WRQ 2    // Write Request
#define DATA 3   // Paquet DATA
#define ACK 4    // Accusé de réception
#define ERROR 5  // Message d'erreur
#define OACK 6   // Option Acknowledgment

// Répertoire de base pour les transferts
#define TFTP_DIR "/var/lib/tftpboot/"

// ----------------------- Structure de session -----------------------
typedef enum {
    ST_UNUSED = 0,
    ST_RRQ,
    ST_WRQ
} session_state;

typedef struct {
    session_state state;
    struct sockaddr_in client_addr;
    FILE *fp;
    unsigned int block_num;   // Bloc en 32 bits pour BIGFILE
    int last_data_size;
    time_t last_activity;
    int retries;
    int sockfd_session;
    int bigfile_enabled;      // 0 = mode standard, 1 = BIGFILE activé
} tftp_session;

static tftp_session sessions[MAX_SESSIONS];
static int sockfd;  // Socket globale

// ----------------------- Fonctions d'envoi -----------------------
void send_ack_session_custom(int session_sockfd, unsigned int block_num, int bigfile_enabled) {
    char ack[8];
    if (bigfile_enabled) {
        ack[0] = 0;
        ack[1] = ACK;
        ack[2] = (block_num >> 24) & 0xFF;
        ack[3] = (block_num >> 16) & 0xFF;
        ack[4] = (block_num >> 8) & 0xFF;
        ack[5] = block_num & 0xFF;
        send(session_sockfd, ack, 6, 0);
        printf("[INFO] ACK envoyé - Bloc %u (BIGFILE)\n", block_num);
    } else {
        ack[0] = 0;
        ack[1] = ACK;
        ack[2] = (block_num >> 8) & 0xFF;
        ack[3] = block_num & 0xFF;
        send(session_sockfd, ack, 4, 0);
        printf("[INFO] ACK envoyé - Bloc %u\n", block_num);
    }
}

void send_error_session(int session_sockfd, int error_code, char *msg) {
    char buffer[PACKET_SIZE];
    memset(buffer, 0, PACKET_SIZE);
    buffer[0] = 0;
    buffer[1] = ERROR;
    buffer[2] = (error_code >> 8) & 0xFF;
    buffer[3] = error_code & 0xFF;
    strcpy(buffer + 4, msg);
    send(session_sockfd, buffer, 4 + strlen(msg) + 1, 0);
    printf("[INFO] ERROR envoyé : %s\n", msg);
}

// ----------------------- Analyse de la requête RRQ/WRQ -----------------------
void parse_request(char *buffer, int n, char *filename_out, int *bigfile_requested) {
    (void)n; // On ignore n
    *bigfile_requested = 0;
    char *ptr = buffer + 2; 
    strcpy(filename_out, ptr);
    ptr += strlen(filename_out) + 1;
    char mode[16];
    strcpy(mode, ptr);
    ptr += strlen(mode) + 1;
    while (*ptr) {
        char option[64], value[64];
        strcpy(option, ptr);
        ptr += strlen(option) + 1;
        strcpy(value, ptr);
        ptr += strlen(value) + 1;
        if (strcasecmp(option, "bigfile") == 0 &&
            (strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0))
            *bigfile_requested = 1;
    }
}

// Envoi d'un OACK pour BIGFILE
void send_oack_bigfile(int sockfd_session) {
    char buffer[256];
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 0;
    buffer[1] = OACK;
    char *ptr = buffer + 2;
    strcpy(ptr, "bigfile");
    ptr += strlen("bigfile") + 1;
    strcpy(ptr, "yes");
    ptr += strlen("yes") + 1;
    int packet_len = ptr - buffer;
    send(sockfd_session, buffer, packet_len, 0);
    printf("[INFO] OACK (BIGFILE) envoyé\n");
}

// ----------------------- Gestion des sessions -----------------------
int find_session_slot(struct sockaddr_in *addr) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state != ST_UNUSED) {
            if (sessions[i].client_addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
                sessions[i].client_addr.sin_port == addr->sin_port)
                return i;
        }
    }
    return -1;
}

int create_session(struct sockaddr_in *addr, session_state st) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state == ST_UNUSED) {
            sessions[i].state = st;
            sessions[i].client_addr = *addr;
            sessions[i].fp = NULL;
            sessions[i].block_num = 0;
            sessions[i].last_data_size = 0;
            sessions[i].last_activity = time(NULL);
            sessions[i].retries = 0;
            sessions[i].bigfile_enabled = 0;
            sessions[i].sockfd_session = socket(AF_INET, SOCK_DGRAM, 0);
            if (sessions[i].sockfd_session < 0) {
                perror("socket");
                sessions[i].state = ST_UNUSED;
                return -1;
            }
            struct sockaddr_in local_addr;
            memset(&local_addr, 0, sizeof(local_addr));
            local_addr.sin_family = AF_INET;
            local_addr.sin_addr.s_addr = INADDR_ANY;
            local_addr.sin_port = htons(0);
            if (bind(sessions[i].sockfd_session, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
                perror("bind");
                close(sessions[i].sockfd_session);
                sessions[i].state = ST_UNUSED;
                return -1;
            }
            if (connect(sessions[i].sockfd_session, (struct sockaddr*)addr, sizeof(*addr)) < 0) {
                perror("connect");
                close(sessions[i].sockfd_session);
                sessions[i].state = ST_UNUSED;
                return -1;
            }
            return i;
        }
    }
    return -1;
}

void close_session(int idx) {
    if (sessions[idx].fp) {
        fclose(sessions[idx].fp);
        sessions[idx].fp = NULL;
    }
    if (sessions[idx].sockfd_session > 0) {
        close(sessions[idx].sockfd_session);
        sessions[idx].sockfd_session = -1;
    }
    sessions[idx].state = ST_UNUSED;
    printf("[INFO] Session %d fermée.\n", idx);
}

void check_timeouts() {
    time_t now = time(NULL);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state != ST_UNUSED) {
            if (difftime(now, sessions[i].last_activity) > TIMEOUT_SEC) {
                printf("[WARN] Timeout session %d\n", i);
                close_session(i);
            }
        }
    }
}

// ----------------------- Handlers pour RRQ/WRQ -----------------------

// handle_rrq : envoi du fichier au client (GET)
void handle_rrq(int idx, char *filename) {
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("[ERROR] Fichier introuvable");
        send_error_session(sessions[idx].sockfd_session, 1, "Fichier introuvable");
        close_session(idx);
        return;
    }
    // Vérification de la taille du fichier
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);
    if (fsize > 65000 && sessions[idx].bigfile_enabled == 0) {
        send_error_session(sessions[idx].sockfd_session, 5, "Fichier trop grand, activez le mode BIGFILE");
        fclose(fp);
        close_session(idx);
        return;
    }
    sessions[idx].fp = fp;
    sessions[idx].block_num = 1;
    sessions[idx].state = ST_RRQ;

    if (sessions[idx].bigfile_enabled) {
        send_oack_bigfile(sessions[idx].sockfd_session);
        char ack_buf[8];
        int n = recv(sessions[idx].sockfd_session, ack_buf, sizeof(ack_buf), 0);
        if (n < 6) {
            printf("[ERROR] OACK ACK non reçu\n");
            close_session(idx);
            return;
        }
        int ack_opcode = (ack_buf[0] << 8) | (unsigned char)ack_buf[1];
        unsigned int ack_block = 0;
        ack_block |= ((unsigned char)ack_buf[2]) << 24;
        ack_block |= ((unsigned char)ack_buf[3]) << 16;
        ack_block |= ((unsigned char)ack_buf[4]) << 8;
        ack_block |= ((unsigned char)ack_buf[5]);
        if (ack_opcode != ACK || ack_block != 0) {
            printf("[ERROR] ACK(0) incorrect après OACK\n");
            close_session(idx);
            return;
        }
        printf("[INFO] OACK ACK reçu, début de transfert BIGFILE\n");
    }

    char buffer[PACKET_SIZE];
    memset(buffer, 0, PACKET_SIZE);
    if (sessions[idx].bigfile_enabled) {
        buffer[0] = 0;
        buffer[1] = DATA;
        buffer[2] = (sessions[idx].block_num >> 24) & 0xFF;
        buffer[3] = (sessions[idx].block_num >> 16) & 0xFF;
        buffer[4] = (sessions[idx].block_num >> 8) & 0xFF;
        buffer[5] = sessions[idx].block_num & 0xFF;
        int n = fread(buffer + 6, 1, DATA_SIZE, fp);
        sessions[idx].last_data_size = n;
        send(sessions[idx].sockfd_session, buffer, n + 6, 0);
        printf("[INFO] DATA envoyé - Bloc %u (%d octets) [BIGFILE]\n", sessions[idx].block_num, n);
    } else {
        buffer[0] = 0;
        buffer[1] = DATA;
        buffer[2] = (sessions[idx].block_num >> 8) & 0xFF;
        buffer[3] = sessions[idx].block_num & 0xFF;
        int n = fread(buffer + 4, 1, DATA_SIZE, fp);
        sessions[idx].last_data_size = n;
        send(sessions[idx].sockfd_session, buffer, n + 4, 0);
        printf("[INFO] DATA envoyé - Bloc %u (%d octets)\n", sessions[idx].block_num, n);
    }
    sessions[idx].last_activity = time(NULL);
}

// handle_wrq : réception d'un fichier depuis le client (PUT)
// Ici, nous ne vérifions pas la taille côté serveur car le client doit déjà l'avoir fait.
void handle_wrq(int idx, char *filename) {
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        perror("[ERROR] Impossible de créer le fichier");
        send_error_session(sessions[idx].sockfd_session, 2, "Impossible de créer le fichier");
        close_session(idx);
        return;
    }
    sessions[idx].fp = fp;
    sessions[idx].block_num = 0;
    sessions[idx].state = ST_WRQ;
    if (sessions[idx].bigfile_enabled) {
        send_oack_bigfile(sessions[idx].sockfd_session);
        char ack_buf[8];
        int n = recv(sessions[idx].sockfd_session, ack_buf, sizeof(ack_buf), 0);
        if (n < 6) {
            printf("[ERROR] OACK ACK non reçu en WRQ\n");
            close_session(idx);
            return;
        }
        int ack_opcode = (ack_buf[0] << 8) | (unsigned char)ack_buf[1];
        unsigned int ack_block = 0;
        ack_block |= ((unsigned char)ack_buf[2]) << 24;
        ack_block |= ((unsigned char)ack_buf[3]) << 16;
        ack_block |= ((unsigned char)ack_buf[4]) << 8;
        ack_block |= ((unsigned char)ack_buf[5]);
        if (ack_opcode != ACK || ack_block != 0) {
            printf("[ERROR] ACK(0) incorrect après OACK en WRQ\n");
            close_session(idx);
            return;
        }
        printf("[INFO] OACK ACK reçu en WRQ, début de réception BIGFILE\n");
    } else {
        send_ack_session_custom(sessions[idx].sockfd_session, 0, 0);
    }
    sessions[idx].last_activity = time(NULL);
}

// handle_data : réception d'un paquet DATA lors d'une WRQ
void handle_data(int idx, char *buffer, int n) {
    int expected_header = sessions[idx].bigfile_enabled ? 6 : 4;
    if (n < expected_header) return;
    unsigned int block_num;
    if (sessions[idx].bigfile_enabled) {
        block_num = 0;
        block_num |= ((unsigned char)buffer[2]) << 24;
        block_num |= ((unsigned char)buffer[3]) << 16;
        block_num |= ((unsigned char)buffer[4]) << 8;
        block_num |= ((unsigned char)buffer[5]);
    } else {
        block_num = ((unsigned char)buffer[2] << 8) | ((unsigned char)buffer[3]);
    }
    if (block_num == sessions[idx].block_num + 1) {
        int data_len = n - expected_header;
        fwrite(buffer + expected_header, 1, data_len, sessions[idx].fp);
        sessions[idx].block_num = block_num;
        send_ack_session_custom(sessions[idx].sockfd_session, block_num, sessions[idx].bigfile_enabled);
        sessions[idx].last_activity = time(NULL);
        if (data_len < DATA_SIZE) {
            printf("[INFO] Fin WRQ session %d\n", idx);
            close_session(idx);
        }
    } else if (block_num == sessions[idx].block_num) {
        send_ack_session_custom(sessions[idx].sockfd_session, block_num, sessions[idx].bigfile_enabled);
    } else {
        printf("[WARN] Session %d: bloc inattendu %u (attendu %u)\n",
               idx, block_num, sessions[idx].block_num + 1);
    }
}

// handle_ack : réception d'un ACK lors d'une RRQ
void handle_ack(int idx, char *buffer) {
    unsigned int ack_block;
    if (sessions[idx].bigfile_enabled) {
        ack_block = 0;
        ack_block |= ((unsigned char)buffer[2]) << 24;
        ack_block |= ((unsigned char)buffer[3]) << 16;
        ack_block |= ((unsigned char)buffer[4]) << 8;
        ack_block |= ((unsigned char)buffer[5]);
    } else {
        ack_block = ((unsigned char)buffer[2] << 8) | ((unsigned char)buffer[3]);
    }
    if (ack_block == sessions[idx].block_num) {
        if (sessions[idx].last_data_size < DATA_SIZE) {
            printf("[INFO] Fin RRQ session %d\n", idx);
            close_session(idx);
        } else {
            sessions[idx].block_num++;
            char data_buffer[PACKET_SIZE];
            memset(data_buffer, 0, PACKET_SIZE);
            data_buffer[0] = 0;
            data_buffer[1] = DATA;
            if (sessions[idx].bigfile_enabled) {
                data_buffer[2] = (sessions[idx].block_num >> 24) & 0xFF;
                data_buffer[3] = (sessions[idx].block_num >> 16) & 0xFF;
                data_buffer[4] = (sessions[idx].block_num >> 8) & 0xFF;
                data_buffer[5] = sessions[idx].block_num & 0xFF;
                int n = fread(data_buffer + 6, 1, DATA_SIZE, sessions[idx].fp);
                sessions[idx].last_data_size = n;
                send(sessions[idx].sockfd_session, data_buffer, n + 6, 0);
                printf("[INFO] DATA envoyé - Bloc %u (%d octets) [BIGFILE]\n", sessions[idx].block_num, n);
            } else {
                data_buffer[2] = (sessions[idx].block_num >> 8) & 0xFF;
                data_buffer[3] = sessions[idx].block_num & 0xFF;
                int n = fread(data_buffer + 4, 1, DATA_SIZE, sessions[idx].fp);
                sessions[idx].last_data_size = n;
                send(sessions[idx].sockfd_session, data_buffer, n + 4, 0);
                printf("[INFO] DATA envoyé - Bloc %u (%d octets)\n", sessions[idx].block_num, n);
            }
            sessions[idx].last_activity = time(NULL);
        }
    } else if (ack_block < sessions[idx].block_num) {
        printf("[WARN] ACK en double pour bloc %u (session %d)\n", ack_block, idx);
    } else {
        printf("[WARN] ACK inattendu bloc %u (session %d, current %u)\n",
               ack_block, idx, sessions[idx].block_num);
    }
}

int main(void) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[PACKET_SIZE];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("[ERROR] Échec de la création du socket.");
        exit(EXIT_FAILURE);
    }
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(6969);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[ERROR] Échec du bind.");
        exit(EXIT_FAILURE);
    }

    printf("[STARTING] Serveur TFTP multi‑clients (BIGFILE option) sur le port 6969...\n");

    for (int i = 0; i < MAX_SESSIONS; i++) {
        sessions[i].state = ST_UNUSED;
        sessions[i].fp = NULL;
        sessions[i].sockfd_session = -1;
        sessions[i].bigfile_enabled = 0;
    }

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        int maxfd = sockfd;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (sessions[i].state != ST_UNUSED) {
                FD_SET(sessions[i].sockfd_session, &readfds);
                if (sessions[i].sockfd_session > maxfd)
                    maxfd = sessions[i].sockfd_session;
            }
        }
        struct timeval tv;
        tv.tv_sec = 1; tv.tv_usec = 0;
        int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }
        if (FD_ISSET(sockfd, &readfds)) {
            memset(buffer, 0, PACKET_SIZE);
            int n = recvfrom(sockfd, buffer, PACKET_SIZE, 0,
                             (struct sockaddr*)&client_addr, &addr_len);
            if (n < 0)
                continue;
            int opcode = (buffer[0] << 8) | (unsigned char)buffer[1];
            char filename[256];
            int bigfile_req = 0;
            parse_request(buffer, n, filename, &bigfile_req);
            if (opcode == RRQ) {
                printf("[INFO] RRQ reçu - Fichier : %s\n", filename);
                int idx = find_session_slot(&client_addr);
                if (idx < 0) {
                    idx = create_session(&client_addr, ST_RRQ);
                    if (idx < 0) {
                        send_error_session(sockfd, 3, "Trop de sessions actives");
                        continue;
                    }
                    sessions[idx].bigfile_enabled = bigfile_req;
                    handle_rrq(idx, filename);
                } else {
                    printf("[WARN] Session existante pour ce client.\n");
                }
            } else if (opcode == WRQ) {
                printf("[INFO] WRQ reçu - Fichier : %s\n", filename);
                int idx = find_session_slot(&client_addr);
                if (idx < 0) {
                    idx = create_session(&client_addr, ST_WRQ);
                    if (idx < 0) {
                        send_error_session(sockfd, 3, "Trop de sessions actives");
                        continue;
                    }
                    sessions[idx].bigfile_enabled = bigfile_req;
                    handle_wrq(idx, filename);
                } else {
                    printf("[WARN] Session existante pour ce client.\n");
                }
            } else {
                send_error_session(sockfd, 4, "Opération non supportée");
            }
        }
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (sessions[i].state != ST_UNUSED &&
                FD_ISSET(sessions[i].sockfd_session, &readfds)) {
                memset(buffer, 0, PACKET_SIZE);
                int n = recv(sessions[i].sockfd_session, buffer, PACKET_SIZE, 0);
                if (n < 0)
                    continue;
                int opcode = (buffer[0] << 8) | (unsigned char)buffer[1];
                switch (opcode) {
                    case DATA:
                        if (sessions[i].state == ST_WRQ)
                            handle_data(i, buffer, n);
                        else
                            send_error_session(sessions[i].sockfd_session, 4, "Session inexistante (DATA)");
                        break;
                    case ACK:
                        if (sessions[i].state == ST_RRQ)
                            handle_ack(i, buffer);
                        else
                            send_error_session(sessions[i].sockfd_session, 4, "Session inexistante (ACK)");
                        break;
                    case ERROR:
                        printf("[ERROR] Paquet ERROR reçu du client.\n");
                        close_session(i);
                        break;
                    default:
                        send_error_session(sessions[i].sockfd_session, 4, "Opération non supportée");
                        break;
                }
            }
        }
        check_timeouts();
    }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        close_session(i);
    }
    close(sockfd);
    return 0;
}