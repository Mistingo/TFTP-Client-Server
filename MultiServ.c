#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

// =================== CONSTANTES TFTP ===================
#define DATA_SIZE     512
#define PACKET_SIZE   (DATA_SIZE + 4) // 4 octets d'en-tête TFTP
#define MAX_SESSIONS  10             // nombre max de transferts simultanés
#define TIMEOUT_SEC   5              // délai de 5s avant la fermeture d'une session inactive
#define MAX_RETRIES   5

#define RRQ   1
#define WRQ   2
#define DATA  3
#define ACK   4
#define ERROR 5

#define TFTP_DIR "/var/lib/tftpboot/"

// =================== STRUCTURE DE SESSION ===================
typedef enum {
    ST_UNUSED = 0,   // session libre
    ST_RRQ,          // envoi de fichier au client
    ST_WRQ           // réception de fichier du client
} session_state;

typedef struct {
    session_state state;           // RRQ ou WRQ
    struct sockaddr_in client_addr;
    FILE *fp;
    int block_num;                 // prochain bloc attendu ou envoyé
    int last_data_size;            // taille du dernier bloc DATA envoyé (ST_RRQ)
    time_t last_activity;          // pour gérer le timeout
    int retries;                   // nombre de retransmissions
} tftp_session;

// =================== VARIABLES GLOBALES ===================
static tftp_session g_sessions[MAX_SESSIONS];
static int g_sockfd;  // socket principal

// =================== FONCTIONS UTILITAIRES ===================
static void send_error(int sockfd, struct sockaddr_in *addr, int error_code, const char *msg) {
    char buffer[PACKET_SIZE];
    memset(buffer, 0, PACKET_SIZE);
    buffer[0] = 0;
    buffer[1] = ERROR;
    buffer[2] = (error_code >> 8) & 0xFF;
    buffer[3] = error_code & 0xFF;
    strcpy(buffer + 4, msg);
    sendto(sockfd, buffer, 4 + strlen(msg) + 1, 0,
           (struct sockaddr*)addr, sizeof(*addr));
    fprintf(stderr, "[INFO] ERROR envoyé (%d): %s\n", error_code, msg);
}

static void send_ack(int sockfd, struct sockaddr_in *addr, int block_num) {
    char ack[4];
    ack[0] = 0;
    ack[1] = ACK;
    ack[2] = (block_num >> 8) & 0xFF;
    ack[3] = block_num & 0xFF;
    sendto(sockfd, ack, 4, 0, (struct sockaddr*)addr, sizeof(*addr));
    printf("[INFO] ACK envoyé - Bloc %d\n", block_num);
}

// Envoi d'un bloc DATA (pour RRQ)
static void send_data_block(tftp_session *sess) {
    char buffer[PACKET_SIZE];
    memset(buffer, 0, PACKET_SIZE);

    // En-tête TFTP
    buffer[0] = 0;
    buffer[1] = DATA;
    buffer[2] = (sess->block_num >> 8) & 0xFF;
    buffer[3] = sess->block_num & 0xFF;

    // Lecture d'un bloc dans le fichier
    int n = fread(buffer + 4, 1, DATA_SIZE, sess->fp);
    sess->last_data_size = n;

    int packet_len = n + 4;
    sendto(g_sockfd, buffer, packet_len, 0,
           (struct sockaddr*)&sess->client_addr, sizeof(sess->client_addr));

    printf("[INFO] DATA envoyé - Bloc %d (%d octets)\n", sess->block_num, n);
    sess->last_activity = time(NULL);
}

// =================== GESTION DES SESSIONS ===================
static int find_session_slot(const struct sockaddr_in *addr) {
    // Cherche si on a déjà une session pour ce client (IP+port)
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].state != ST_UNUSED) {
            if (g_sessions[i].client_addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
                g_sessions[i].client_addr.sin_port == addr->sin_port) {
                return i;
            }
        }
    }
    // Sinon, retourne -1
    return -1;
}

static int create_session(const struct sockaddr_in *addr, session_state st) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].state == ST_UNUSED) {
            g_sessions[i].state = st;
            g_sessions[i].client_addr = *addr;
            g_sessions[i].fp = NULL;
            g_sessions[i].block_num = 0;
            g_sessions[i].last_data_size = 0;
            g_sessions[i].last_activity = time(NULL);
            g_sessions[i].retries = 0;
            return i;
        }
    }
    return -1; // pas de place
}

static void close_session(int idx) {
    if (g_sessions[idx].fp) {
        fclose(g_sessions[idx].fp);
        g_sessions[idx].fp = NULL;
    }
    g_sessions[idx].state = ST_UNUSED;
    printf("[INFO] Session %d fermée.\n", idx);
}

// Vérifie les timeouts et ferme les sessions inactives
static void check_timeouts() {
    time_t now = time(NULL);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].state != ST_UNUSED) {
            double diff = difftime(now, g_sessions[i].last_activity);
            if (diff > TIMEOUT_SEC) {
                fprintf(stderr, "[WARN] Timeout session %d (bloc %d)\n",
                        i, g_sessions[i].block_num);
                close_session(i);
            }
        }
    }
}

// =================== HANDLERS DE PACKETS ===================
static void handle_rrq(int idx, const char *filename) {
    // Ouvre le fichier en lecture
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);

    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("[ERROR] Fichier introuvable");
        send_error(g_sockfd, &g_sessions[idx].client_addr, 1, "Fichier introuvable");
        close_session(idx);
        return;
    }
    g_sessions[idx].fp = fp;
    g_sessions[idx].block_num = 1; // On enverra le bloc 1 en premier
    g_sessions[idx].state = ST_RRQ;

    // On envoie immédiatement le premier bloc
    send_data_block(&g_sessions[idx]);
}

static void handle_wrq(int idx, const char *filename) {
    // Ouvre le fichier en écriture
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        perror("[ERROR] Impossible de créer le fichier");
        send_error(g_sockfd, &g_sessions[idx].client_addr, 2, "Impossible de créer le fichier");
        close_session(idx);
        return;
    }
    g_sessions[idx].fp = fp;
    g_sessions[idx].block_num = 0; // On attend le bloc 1
    g_sessions[idx].state = ST_WRQ;

    // Envoi ACK bloc 0
    send_ack(g_sockfd, &g_sessions[idx].client_addr, 0);
    g_sessions[idx].last_activity = time(NULL);
}

static void handle_data(int idx, const char *buffer, int recv_len) {
    // Vérifie la taille
    if (recv_len < 4) return; // paquet invalide
    tftp_session *sess = &g_sessions[idx];
    int block_num = (buffer[2] << 8) | (unsigned char)buffer[3];

    // On s'attend à block_num == sess->block_num+1 (ou bloc déjà reçu => renvoyer ack)
    if (block_num == sess->block_num + 1) {
        int data_len = recv_len - 4;
        fwrite(buffer + 4, 1, data_len, sess->fp);
        sess->block_num = block_num;
        send_ack(g_sockfd, &sess->client_addr, block_num);
        sess->last_activity = time(NULL);
        // Fin si < 512
        if (data_len < DATA_SIZE) {
            printf("[INFO] Fin WRQ session %d\n", idx);
            close_session(idx);
        }
    }
    else if (block_num == sess->block_num) {
        // Bloc en double, renvoyer l'ACK
        send_ack(g_sockfd, &sess->client_addr, block_num);
    }
    else {
        // Bloc inattendu, on ignore
        fprintf(stderr, "[WARN] Session %d: bloc inattendu %d (attendu %d)\n",
                idx, block_num, sess->block_num + 1);
    }
}

static void handle_ack(int idx, const char *buffer) {
    tftp_session *sess = &g_sessions[idx];
    int block_num = (buffer[2] << 8) | (unsigned char)buffer[3];

    // On s'attend à ACK du bloc qu'on vient d'envoyer
    if (block_num == sess->block_num) {
        // On envoie le bloc suivant
        if (sess->last_data_size < DATA_SIZE) {
            // C'était le dernier bloc
            printf("[INFO] Fin RRQ session %d\n", idx);
            close_session(idx);
        } else {
            sess->block_num++;
            send_data_block(sess);
        }
    }
    else if (block_num < sess->block_num) {
        // ACK en double, on ignore
        printf("[WARN] ACK en double pour bloc %d (session %d)\n", block_num, idx);
    }
    else {
        // ACK inattendu
        printf("[WARN] ACK inattendu bloc %d (session %d, current %d)\n",
               block_num, idx, sess->block_num);
    }
}

// =================== DISPATCH DU PAQUET ===================
static void handle_incoming_packet(char *buffer, int recv_len, struct sockaddr_in *addr) {
    if (recv_len < 2) return;

    int opcode = (buffer[0] << 8) | (unsigned char)buffer[1];
    int idx = find_session_slot(addr);

    switch(opcode) {
    case RRQ: {
        // Nouvelle session
        char *filename = buffer + 2;
        int new_idx = create_session(addr, ST_RRQ);
        if (new_idx < 0) {
            fprintf(stderr, "[ERROR] Plus de sessions disponibles\n");
            send_error(g_sockfd, addr, 3, "Trop de sessions actives");
            return;
        }
        handle_rrq(new_idx, filename);
    } break;

    case WRQ: {
        // Nouvelle session
        char *filename = buffer + 2;
        int new_idx = create_session(addr, ST_WRQ);
        if (new_idx < 0) {
            fprintf(stderr, "[ERROR] Plus de sessions disponibles\n");
            send_error(g_sockfd, addr, 3, "Trop de sessions actives");
            return;
        }
        handle_wrq(new_idx, filename);
    } break;

    case DATA: {
        // Doit correspondre à une session existante (WRQ)
        if (idx < 0) {
            // Pas de session => erreur
            send_error(g_sockfd, addr, 4, "Session inexistante (DATA)");
            return;
        }
        if (g_sessions[idx].state != ST_WRQ) {
            send_error(g_sockfd, addr, 4, "Mauvais état (DATA)");
            return;
        }
        handle_data(idx, buffer, recv_len);
    } break;

    case ACK: {
        // Doit correspondre à une session existante (RRQ)
        if (idx < 0) {
            // Pas de session => erreur
            send_error(g_sockfd, addr, 4, "Session inexistante (ACK)");
            return;
        }
        if (g_sessions[idx].state != ST_RRQ) {
            send_error(g_sockfd, addr, 4, "Mauvais état (ACK)");
            return;
        }
        handle_ack(idx, buffer);
    } break;

    case ERROR: {
        // Le client nous signale une erreur
        fprintf(stderr, "[ERROR] Reçu un paquet ERROR du client.\n");
        // Fermer la session si elle existe
        if (idx >= 0) {
            close_session(idx);
        }
    } break;

    default:
        // Opcode inconnu
        send_error(g_sockfd, addr, 4, "Opération non supportée");
        break;
    }
}

int main(void) {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    // Création socket UDP
    g_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sockfd < 0) {
        perror("[ERROR] Échec de la création du socket.");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(6969);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[ERROR] Échec du bind.");
        close(g_sockfd);
        exit(EXIT_FAILURE);
    }

    printf("[STARTING] Serveur TFTP multi-clients (select) en attente sur le port 6969...\n");

    // Initialise nos sessions
    for (int i = 0; i < MAX_SESSIONS; i++) {
        g_sessions[i].state = ST_UNUSED;
        g_sessions[i].fp = NULL;
    }

    // Boucle principale
    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(g_sockfd, &readfds);

        struct timeval tv;
        tv.tv_sec = 1;  // on se réveille toutes les 1s pour gérer timeouts
        tv.tv_usec = 0;

        int ret = select(g_sockfd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue; // interrompu par un signal, on réessaie
            perror("select");
            break;
        }
        if (ret == 0) {
            // Pas d'activité => check timeouts
            check_timeouts();
            continue;
        }
        // Si on arrive ici, c'est qu'il y a un paquet à lire
        if (FD_ISSET(g_sockfd, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            char buffer[PACKET_SIZE];
            int n = recvfrom(g_sockfd, buffer, PACKET_SIZE, 0,
                             (struct sockaddr*)&client_addr, &addr_len);
            if (n > 0) {
                handle_incoming_packet(buffer, n, &client_addr);
            }
        }
        // Après avoir géré le paquet, on vérifie encore les timeouts
        check_timeouts();
    }

    // Nettoyage final
    for (int i = 0; i < MAX_SESSIONS; i++) {
        close_session(i);
    }
    close(g_sockfd);
    return 0;
}