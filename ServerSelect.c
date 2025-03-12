#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

// Définition des constantes TFTP
#define DATA_SIZE 512
#define PACKET_SIZE (DATA_SIZE + 4)
#define MAX_RETRIES 5

// Définition du nombre maximum de sessions simultanées et du timeout (en secondes)
#define MAX_SESSIONS 10
#define TIMEOUT_SEC 5

// Codes d'opération TFTP
#define RRQ 1    // Read Request (demande de lecture)
#define WRQ 2    // Write Request (demande d'écriture)
#define DATA 3    // Paquet de données
#define ACK 4    // Accusé de réception
#define ERROR 5    // Message d'erreur

// Répertoire de base pour les transferts
#define TFTP_DIR "/var/lib/tftpboot/"

// ----------------------- Structure de session -----------------------

// Les sessions permettent de gérer plusieurs transferts simultanément.
typedef enum {
    ST_UNUSED = 0,   // Session libre
    ST_RRQ,          // Envoi de fichier vers le client (lecture côté client)
    ST_WRQ           // Réception de fichier depuis le client (écriture côté serveur)
} session_state;

typedef struct {
    session_state state;           // État de la session (ST_RRQ ou ST_WRQ)
    struct sockaddr_in client_addr; // Adresse du client associé à la session
    FILE *fp;                      // Fichier en cours de transfert
    int block_num;                 // Bloc courant (envoyé ou attendu)
    int last_data_size;            // Taille du dernier bloc DATA envoyé (pour RRQ)
    time_t last_activity;          // Dernière activité (pour gérer le timeout)
    int retries;                   // Nombre de retransmissions effectuées
} tftp_session;

// Tableau global des sessions et socket principale
static tftp_session sessions[MAX_SESSIONS];
static int sockfd;

// ----------------------- Fonctions d'envoi -----------------------

// Envoi d'un ACK pour le bloc donné
void send_ack(int sockfd, struct sockaddr_in addr, int block_num) {
    char ack[4];
    ack[0] = 0;
    ack[1] = ACK;
    ack[2] = (block_num >> 8) & 0xFF;
    ack[3] = block_num & 0xFF;
    sendto(sockfd, ack, sizeof(ack), 0, (struct sockaddr*)&addr, sizeof(addr));
    printf("[INFO] ACK envoyé - Bloc %d\n", block_num);
}

// Envoi d'un paquet ERROR avec code et message
void send_error(int sockfd, struct sockaddr_in addr, int error_code, char *msg) {
    char buffer[PACKET_SIZE];
    memset(buffer, 0, PACKET_SIZE);
    buffer[0] = 0;
    buffer[1] = ERROR;
    buffer[2] = (error_code >> 8) & 0xFF;
    buffer[3] = error_code & 0xFF;
    strcpy(buffer + 4, msg);
    sendto(sockfd, buffer, 4 + strlen(msg) + 1, 0, (struct sockaddr*)&addr, sizeof(addr));
    printf("[INFO] ERROR envoyé : %s\n", msg);
}

// ----------------------- Gestion des sessions -----------------------

// Recherche d'une session existante pour le client (basée sur l'adresse IP et le port)
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

// Création d'une nouvelle session pour le client
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
            return i;
        }
    }
    return -1; // Pas de slot disponible
}

// Fermeture d'une session et libération des ressources associées
void close_session(int idx) {
    if (sessions[idx].fp) {
        fclose(sessions[idx].fp);
        sessions[idx].fp = NULL;
    }
    sessions[idx].state = ST_UNUSED;
    printf("[INFO] Session %d fermée.\n", idx);
}

// Vérification des sessions inactives et fermeture en cas de timeout
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

// Gestion d'une requête RRQ (lecture) : ouverture du fichier et envoi du premier bloc
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
    sendto(sockfd, buffer, n + 4, 0,
           (struct sockaddr*)&sessions[idx].client_addr, sizeof(sessions[idx].client_addr));
    printf("[INFO] DATA envoyé - Bloc %d (%d octets)\n", sessions[idx].block_num, n);
    sessions[idx].last_activity = time(NULL);
}

// Gestion d'une requête WRQ (écriture) : ouverture du fichier et envoi de l'ACK initial
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
    send_ack(sockfd, sessions[idx].client_addr, 0);
    sessions[idx].last_activity = time(NULL);
}

// Gestion de la réception d'un paquet DATA lors d'un WRQ
void handle_data(int idx, char *buffer, int n) {
    if (n < 4) return;
    int block_num = (buffer[2] << 8) | (unsigned char)buffer[3];
    if (block_num == sessions[idx].block_num + 1) {
        int data_len = n - 4;
        fwrite(buffer + 4, 1, data_len, sessions[idx].fp);
        sessions[idx].block_num = block_num;
        send_ack(sockfd, sessions[idx].client_addr, block_num);
        sessions[idx].last_activity = time(NULL);
        if (data_len < DATA_SIZE) {
            printf("[INFO] Fin WRQ session %d\n", idx);
            close_session(idx);
        }
    }
    else if (block_num == sessions[idx].block_num) {
        // Bloc déjà reçu : renvoyer l'ACK
        send_ack(sockfd, sessions[idx].client_addr, block_num);
    }
    else {
        printf("[WARN] Session %d: bloc inattendu %d (attendu %d)\n",
               idx, block_num, sessions[idx].block_num + 1);
    }
}

// Gestion de la réception d'un ACK lors d'un RRQ
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
            sendto(sockfd, data_buffer, n + 4, 0,
                   (struct sockaddr*)&sessions[idx].client_addr, sizeof(sessions[idx].client_addr));
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

    // Création de la socket UDP
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("[ERROR] Échec de la création du socket.");
        exit(EXIT_FAILURE);
    }

    // Configuration de l'adresse du serveur
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(6969);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[ERROR] Échec du bind.");
        exit(EXIT_FAILURE);
    }

    printf("[STARTING] Serveur TFTP multi‑clients (structure style Serv.c) en attente sur le port 6969...\n");

    // Initialisation des sessions
    for (int i = 0; i < MAX_SESSIONS; i++) {
        sessions[i].state = ST_UNUSED;
        sessions[i].fp = NULL;
    }

    // Boucle principale : utilisation de select() pour gérer l'activité sur la socket
    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        struct timeval tv;
        tv.tv_sec = 1;  // On se réveille toutes les secondes pour vérifier les timeouts
        tv.tv_usec = 0;
        int ret = select(sockfd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (ret > 0 && FD_ISSET(sockfd, &readfds)) {
            memset(buffer, 0, PACKET_SIZE);
            int n = recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&client_addr, &addr_len);
            if (n < 0) continue;
            int opcode = (buffer[0] << 8) | (unsigned char)buffer[1];
            char *filename = buffer + 2;
            // Dispatch des paquets selon l'opcode
            if (opcode == RRQ) {
                printf("[INFO] RRQ reçu - Demande de lecture de fichier : %s\n", filename);
                int idx = find_session_slot(&client_addr);
                if (idx < 0) {
                    idx = create_session(&client_addr, ST_RRQ);
                    if (idx < 0) {
                        send_error(sockfd, client_addr, 3, "Trop de sessions actives");
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
                        send_error(sockfd, client_addr, 3, "Trop de sessions actives");
                        continue;
                    }
                    handle_wrq(idx, filename);
                } else {
                    printf("[WARN] Session existante pour ce client.\n");
                }
            } else if (opcode == DATA) {
                int idx = find_session_slot(&client_addr);
                if (idx >= 0 && sessions[idx].state == ST_WRQ) {
                    handle_data(idx, buffer, n);
                } else {
                    send_error(sockfd, client_addr, 4, "Session inexistante (DATA)");
                }
            } else if (opcode == ACK) {
                int idx = find_session_slot(&client_addr);
                if (idx >= 0 && sessions[idx].state == ST_RRQ) {
                    handle_ack(idx, buffer);
                } else {
                    send_error(sockfd, client_addr, 4, "Session inexistante (ACK)");
                }
            } else if (opcode == ERROR) {
                printf("[ERROR] Paquet ERROR reçu du client.\n");
                int idx = find_session_slot(&client_addr);
                if (idx >= 0) close_session(idx);
            } else {
                send_error(sockfd, client_addr, 4, "Opération non supportée");
            }
        }
        // Vérification régulière des timeouts des sessions
        check_timeouts();
    }

    // Fermeture de toutes les sessions et de la socket avant de quitter
    for (int i = 0; i < MAX_SESSIONS; i++) {
        close_session(i);
    }
    close(sockfd);
    return 0;
}