#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#define DATA_SIZE 512
#define PACKET_SIZE (DATA_SIZE + 4)
#define MAX_RETRIES 5

#define MAX_SESSIONS 10
#define TIMEOUT_SEC 5

// Codes d'opération TFTP
#define RRQ 1    // Read Request
#define WRQ 2    // Write Request
#define DATA 3   // Paquet DATA
#define ACK 4    // Accusé de réception
#define ERROR 5  // Message d'erreur

// Répertoire de base pour les transferts
#define TFTP_DIR "/var/lib/tftpboot/"

// ----------------------- Structure de session -----------------------

typedef enum {
    ST_UNUSED = 0,   // Session libre
    ST_RRQ,          // Envoi de fichier vers le client (lecture côté client)
    ST_WRQ           // Réception de fichier depuis le client (écriture côté serveur)
} session_state;

typedef struct {
    session_state state;           // État de la session (lecture ou écriture)
    struct sockaddr_in client_addr; // Adresse du client associé à la session
    FILE *fp;                      // Fichier en cours de transfert
    int block_num;                 // Bloc courant (envoyé ou attendu)
    int last_data_size;            // Taille du dernier bloc DATA envoyé (pour RRQ)
    time_t last_activity;          // Dernière activité (pour gérer le timeout)
    int retries;                   // Nombre de retransmissions effectuées
    int sockfd_session;            // Socket dédiée à cette session
} tftp_session;

static tftp_session sessions[MAX_SESSIONS];
static int sockfd;  // Socket globale pour l'initialisation

// ----------------------- Fonctions d'envoi utilisant le socket de session -----------------------

void send_ack_session(int session_sockfd, int block_num) {
    char ack[4];
    ack[0] = 0;
    ack[1] = ACK;
    ack[2] = (block_num >> 8) & 0xFF;
    ack[3] = block_num & 0xFF;
    send(session_sockfd, ack, sizeof(ack), 0);
    printf("[INFO] ACK envoyé - Bloc %d\n", block_num);
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

// ----------------------- Gestion des sessions -----------------------

int find_session_slot(struct sockaddr_in *addr) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state != ST_UNUSED) {
            if (sessions[i].client_addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
                sessions[i].client_addr.sin_port == addr->sin_port)
                return i;
        }
    }
    return -1; // Aucune session existante pour ce client
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
            // Création d'un socket dédié pour la session
            sessions[i].sockfd_session = socket(AF_INET, SOCK_DGRAM, 0);
            if (sessions[i].sockfd_session < 0) {
                perror("socket");
                sessions[i].state = ST_UNUSED;
                return -1;
            }
            // Bind sur une adresse locale avec port éphémère (0)
            struct sockaddr_in local_addr;
            memset(&local_addr, 0, sizeof(local_addr));
            local_addr.sin_family = AF_INET;
            local_addr.sin_addr.s_addr = INADDR_ANY;
            local_addr.sin_port = htons(0); // port éphémère
            if (bind(sessions[i].sockfd_session, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
                perror("bind");
                close(sessions[i].sockfd_session);
                sessions[i].state = ST_UNUSED;
                return -1;
            }
            // Connecte le socket à l'adresse du client
            if (connect(sessions[i].sockfd_session, (struct sockaddr*)addr, sizeof(*addr)) < 0) {
                perror("connect");
                close(sessions[i].sockfd_session);
                sessions[i].state = ST_UNUSED;
                return -1;
            }
            return i;
        }
    }
    return -1; // Aucune session disponible
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

// ----------------------- Handlers pour les transferts -----------------------

void handle_rrq(int idx, char *filename) {
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("[ERROR] Fichier introuvable");
        close_session(idx);
        return;
    }
    sessions[idx].fp = fp;
    sessions[idx].block_num = 1;
    sessions[idx].state = ST_RRQ;
    
    // Envoi immédiat du premier bloc DATA
    char buffer[PACKET_SIZE];
    memset(buffer, 0, PACKET_SIZE);
    buffer[0] = 0;
    buffer[1] = DATA;
    buffer[2] = (sessions[idx].block_num >> 8) & 0xFF;
    buffer[3] = sessions[idx].block_num & 0xFF;
    int n = fread(buffer + 4, 1, DATA_SIZE, fp);
    sessions[idx].last_data_size = n;
    send(sessions[idx].sockfd_session, buffer, n + 4, 0);
    printf("[INFO] DATA envoyé - Bloc %d (%d octets)\n", sessions[idx].block_num, n);
    sessions[idx].last_activity = time(NULL);
}

void handle_wrq(int idx, char *filename) {
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        perror("[ERROR] Impossible de créer le fichier");
        close_session(idx);
        return;
    }
    sessions[idx].fp = fp;
    sessions[idx].block_num = 0;  // On attend le bloc 1
    sessions[idx].state = ST_WRQ;
    send_ack_session(sessions[idx].sockfd_session, 0);
    sessions[idx].last_activity = time(NULL);
}

void handle_data(int idx, char *buffer, int n) {
    if (n < 4) return;
    int block_num = (buffer[2] << 8) | (unsigned char)buffer[3];
    if (block_num == sessions[idx].block_num + 1) {
        int data_len = n - 4;
        fwrite(buffer + 4, 1, data_len, sessions[idx].fp);
        sessions[idx].block_num = block_num;
        send_ack_session(sessions[idx].sockfd_session, block_num);
        sessions[idx].last_activity = time(NULL);
        if (data_len < DATA_SIZE) {
            printf("[INFO] Fin WRQ session %d\n", idx);
            close_session(idx);
        }
    } else if (block_num == sessions[idx].block_num) {
        // Bloc déjà reçu, renvoie de l'ACK
        send_ack_session(sessions[idx].sockfd_session, block_num);
    } else {
        printf("[WARN] Session %d: bloc inattendu %d (attendu %d)\n",
               idx, block_num, sessions[idx].block_num + 1);
    }
}

void handle_ack(int idx, char *buffer) {
    int block_num = (buffer[2] << 8) | (unsigned char)buffer[3];
    if (block_num == sessions[idx].block_num) {
        if (sessions[idx].last_data_size < DATA_SIZE) {
            printf("[INFO] Fin RRQ session %d\n", idx);
            close_session(idx);
        } else {
            sessions[idx].block_num++;
            char data_buffer[PACKET_SIZE];
            memset(data_buffer, 0, PACKET_SIZE);
            data_buffer[0] = 0;
            data_buffer[1] = DATA;
            data_buffer[2] = (sessions[idx].block_num >> 8) & 0xFF;
            data_buffer[3] = sessions[idx].block_num & 0xFF;
            int n = fread(data_buffer + 4, 1, DATA_SIZE, sessions[idx].fp);
            sessions[idx].last_data_size = n;
            send(sessions[idx].sockfd_session, data_buffer, n + 4, 0);
            printf("[INFO] DATA envoyé - Bloc %d (%d octets)\n", sessions[idx].block_num, n);
            sessions[idx].last_activity = time(NULL);
        }
    } else if (block_num < sessions[idx].block_num) {
        printf("[WARN] ACK en double pour bloc %d (session %d)\n", block_num, idx);
    } else {
        printf("[WARN] ACK inattendu bloc %d (session %d, current %d)\n",
               block_num, idx, sessions[idx].block_num);
    }
}

// ----------------------- Boucle principale -----------------------

int main(void) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[PACKET_SIZE];

    // Création du socket global pour l'initialisation
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

    printf("[STARTING] Serveur TFTP multi‑clients modifié avec sockets par session sur le port 6969...\n");

    // Initialisation des sessions
    for (int i = 0; i < MAX_SESSIONS; i++) {
        sessions[i].state = ST_UNUSED;
        sessions[i].fp = NULL;
        sessions[i].sockfd_session = -1;
    }

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        // Ajout du socket global pour les nouvelles connexions
        FD_SET(sockfd, &readfds);
        int maxfd = sockfd;
        // Ajout des sockets de session actifs
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (sessions[i].state != ST_UNUSED) {
                FD_SET(sessions[i].sockfd_session, &readfds);
                if (sessions[i].sockfd_session > maxfd)
                    maxfd = sessions[i].sockfd_session;
            }
        }
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ret = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }
        // Gestion des nouvelles requêtes sur le socket global
        if (FD_ISSET(sockfd, &readfds)) {
            memset(buffer, 0, PACKET_SIZE);
            int n = recvfrom(sockfd, buffer, PACKET_SIZE, 0,
                             (struct sockaddr*)&client_addr, &addr_len);
            if (n < 0)
                continue;
            int opcode = (buffer[0] << 8) | (unsigned char)buffer[1];
            char *filename = buffer + 2;
            if (opcode == RRQ) {
                printf("[INFO] RRQ reçu - Demande de lecture de fichier : %s\n", filename);
                int idx = find_session_slot(&client_addr);
                if (idx < 0) {
                    idx = create_session(&client_addr, ST_RRQ);
                    if (idx < 0) {
                        send_error_session(sockfd, 3, "Trop de sessions actives");
                        continue;
                    }
                    handle_rrq(idx, filename);
                } else {
                    printf("[WARN] Session existante pour ce client.\n");
                }
            } else if (opcode == WRQ) {
                printf("[INFO] WRQ reçu - Demande d'écriture de fichier : %s\n", filename);
                int idx = find_session_slot(&client_addr);
                if (idx < 0) {
                    idx = create_session(&client_addr, ST_WRQ);
                    if (idx < 0) {
                        send_error_session(sockfd, 3, "Trop de sessions actives");
                        continue;
                    }
                    handle_wrq(idx, filename);
                } else {
                    printf("[WARN] Session existante pour ce client.\n");
                }
            } else {
                send_error_session(sockfd, 4, "Opération non supportée");
            }
        }
        // Traitement des paquets sur les sockets de session
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
        // Vérification régulière des timeouts des sessions
        check_timeouts();
    }

    // Fermeture de toutes les sessions et du socket global avant de quitter
    for (int i = 0; i < MAX_SESSIONS; i++) {
        close_session(i);
    }
    close(sockfd);
    return 0;
}