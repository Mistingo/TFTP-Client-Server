#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/time.h>

#define DATA_SIZE 512               // Taille des données à envoyer dans chaque paquet (512 octets pour les données TFTP)
#define PACKET_SIZE (DATA_SIZE + 4) // Taille totale d'un paquet, incluant les 4 octets d'en-tête TFTP (pour opcode et bloc)
#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERROR 5

#define TFTP_DIR "/var/lib/tftpboot/"  // Répertoire où les fichiers seront sauvegardés ou reçus

// Mutex pour gérer l'accès au fichier de manière sécurisée (empêche plusieurs accès simultanés)
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// Structure pour stocker les informations d'une requête client
typedef struct {
    int sockfd;                  // Socket du client
    struct sockaddr_in client_addr; // Adresse du client
    char filename[256];           // Nom du fichier demandé
    int opcode;                   // Type de la requête (RRQ ou WRQ)
} client_request_t;

// Fonction pour envoyer un ACK (accusé de réception) au client
void send_ack(int sockfd, struct sockaddr_in addr, int block_num) {
    char ack[4];
    ack[0] = 0;
    ack[1] = ACK; // Code de l'ACK
    ack[2] = (block_num >> 8) & 0xFF; // Numéro de bloc (premiers 8 bits)
    ack[3] = block_num & 0xFF; // Numéro de bloc (derniers 8 bits)
    sendto(sockfd, ack, sizeof(ack), 0, (struct sockaddr*)&addr, sizeof(addr));
    printf("[INFO] Serveur: ACK %d envoyé au client.\n", block_num);
}

// Fonction pour envoyer un fichier au client
void send_file(int sockfd, struct sockaddr_in addr, char* filename) {
    int n, block_num = 1;
    char buffer[PACKET_SIZE];
    socklen_t addr_size = sizeof(addr);
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char filepath[1024];

    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename); // Crée le chemin du fichier

    // Ouvrir le fichier en mode lecture
    FILE* fp = fopen(filepath, "rb");
    if (fp == NULL) {
        perror("[ERROR] Fichier introuvable.");
        return;
    }

    // Calculer la taille du fichier
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    rewind(fp);

    if (file_size == 0) {
        printf("[ERROR] Le fichier est vide, envoi annulé.\n");
        fclose(fp);
        return;
    }

    printf("[INFO] Début d'envoi du fichier : %s (%ld octets)\n", filename, file_size);

    // Configurer un timeout pour la réception des ACKs
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    while (1) {
        memset(buffer, 0, PACKET_SIZE); // Nettoyer le buffer
        buffer[0] = 0; // Initialisation du paquet TFTP
        buffer[1] = DATA; // Code pour DATA
        buffer[2] = (block_num >> 8) & 0xFF; // Premier octet du bloc
        buffer[3] = block_num & 0xFF; // Deuxième octet du bloc

        // Lire le fichier et stocker les données dans le buffer
        n = fread(buffer + 4, 1, DATA_SIZE, fp);
        if (n < 0) break;

        // Envoyer le paquet DATA
        sendto(sockfd, buffer, n + 4, 0, (struct sockaddr*)&addr, addr_size);
        printf("[INFO] DATA envoyé - Bloc %d (%d octets)\n", block_num, n);

        // Attente de l'ACK du client
        char ack_buffer[PACKET_SIZE];
        int ack_received, retries = 0;
        while (retries < 3) {
            ack_received = recvfrom(sockfd, ack_buffer, PACKET_SIZE, 0,
                                    (struct sockaddr*)&client_addr, &client_addr_len);
            if (ack_received >= 4) {
                int ack_opcode = (ack_buffer[0] << 8) | ack_buffer[1];
                int ack_block = (ack_buffer[2] << 8) | ack_buffer[3];
                if (ack_opcode == ACK && ack_block == block_num) {
                    printf("[INFO] Serveur: ACK %d reçu de %s:%d\n", block_num,
                        inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                    addr = client_addr; // Mettre à jour l'adresse du client
                    break;
                } else {
                    printf("[ERROR] ACK invalide reçu (opcode: %d, block: %d) pour bloc attendu %d\n",
                        ack_opcode, ack_block, block_num);
                }
            } else {
                printf("[WARNING] Aucun ACK reçu pour le bloc %d, tentative de renvoi (%d/3).\n", block_num, retries + 1);
                sendto(sockfd, buffer, n + 4, 0, (struct sockaddr*)&addr, addr_size);
                retries++;
            }
        }
        if (retries == 3) {
            printf("[ERROR] Abandon de l'envoi du bloc %d après 3 tentatives.\n", block_num);
            break;
        }
        block_num++;
        sleep(1);
        if (n < DATA_SIZE) break; // Fin du fichier si moins de données sont lues
    }
    fclose(fp);
    printf("[INFO] Fin d'envoi du fichier : %s\n", filename);
    printf("[INFO] Fin de transmission.\n");
}

// Fonction pour recevoir un fichier du client
void receive_file(int sockfd, struct sockaddr_in addr, char* filename) {
    int n, block_num = 0;
    char buffer[PACKET_SIZE];
    socklen_t addr_size = sizeof(addr);
    char filepath[1024], temp_filepath[1024];

    snprintf(filepath, sizeof(filepath), "%s%s", TFTP_DIR, filename);
    snprintf(temp_filepath, sizeof(temp_filepath), "%s%s.tmp", TFTP_DIR, filename);

    // Verrouiller l'accès au fichier avec mutex pour éviter les écritures simultanées
    pthread_mutex_lock(&file_mutex);
    FILE* fp = fopen(temp_filepath, "wb");
    pthread_mutex_unlock(&file_mutex);

    if (fp == NULL) {
        perror("[ERROR] Impossible de créer le fichier temporaire.");
        return;
    }

    send_ack(sockfd, addr, block_num); // Envoi de l'ACK initial
    printf("[DEBUG] ACK initial envoyé, attente des blocs DATA...\n");

    while (1) {
        memset(buffer, 0, PACKET_SIZE); // Nettoyage du buffer
        n = recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        printf("[DEBUG] Paquet reçu - Taille: %d octets\n", n);
        if (n < 4) break; // Si le paquet est trop petit, on arrête

        int opcode = (buffer[0] << 8) | buffer[1];
        if (opcode == DATA) { // Si c'est un paquet DATA
            block_num = (buffer[2] << 8) | buffer[3];
            if (n > 4) {
                size_t written = fwrite(buffer + 4, 1, n - 4, fp);
                if (written != (size_t)(n - 4)) {
                    perror("[ERROR] Ecriture du fichier");
                    break;
                }
                printf("[INFO] DATA reçu - Bloc %d (%d octets)\n", block_num, n - 4);
            } else {
                printf("[INFO] Bloc %d reçu (fin de transmission, 0 octets)\n", block_num);
            }
            send_ack(sockfd, addr, block_num); // Envoi de l'ACK pour confirmer la réception
            printf("[DEBUG] ACK %d envoyé\n", block_num);
            if ((n - 4) < DATA_SIZE) break; // Fin de la transmission si le bloc est plus petit que la taille de données
        }
        sleep(1);
    }
    fclose(fp);
    // Renommer le fichier temporaire en fichier final
    if (rename(temp_filepath, filepath) != 0) {
        perror("[ERROR] Renommage du fichier temporaire");
    } else {
        printf("[INFO] Fichier %s reçu correctement.\n", filename);
    }
    printf("[INFO] Fin de réception du fichier : %s\n", filename);
    printf("[INFO] Fin de transmission.\n");
}

// Fonction pour gérer chaque requête client dans un thread
void* handle_client_request(void* arg) {
    client_request_t* request = (client_request_t*) arg;
    char lock_path[1024];
    snprintf(lock_path, sizeof(lock_path), "%s%s.lock", TFTP_DIR, request->filename);

    // Information sur le client
    char client_info[64];
    snprintf(client_info, sizeof(client_info), "%s:%d",
             inet_ntoa(request->client_addr.sin_addr),
             ntohs(request->client_addr.sin_port));

    // Vérification si un transfert de fichier est déjà en cours (lock)
    if (access(lock_path, F_OK) == 0) {
        FILE *lock_fp = fopen(lock_path, "r");
        if (lock_fp) {
            char stored_info[64];
            if (fgets(stored_info, sizeof(stored_info), lock_fp) != NULL) {
                stored_info[strcspn(stored_info, "\n")] = '\0';
                if (strcmp(stored_info, client_info) != 0) {
                    fclose(lock_fp);
                    // Envoi d'un message d'erreur au client
                    char error_packet[PACKET_SIZE];
                    const char *error_msg = "Erreur: un transfert de fichier est déjà en cours";
                    error_packet[0] = 0;
                    error_packet[1] = ERROR;
                    error_packet[2] = 0;
                    error_packet[3] = 0;
                    strcpy(error_packet + 4, error_msg);
                    sendto(request->sockfd, error_packet, strlen(error_msg) + 5, 0,
                           (struct sockaddr*)&request->client_addr, sizeof(request->client_addr));
                    printf("[ERROR] Transfert du fichier %s refusé : déjà en cours (autre client).\n", request->filename);
                    free(request);
                    pthread_exit(NULL);
                }
            }
            fclose(lock_fp);
        }
    } else {
        FILE *lock_fp = fopen(lock_path, "w");
        if (lock_fp == NULL) {
            perror("[ERROR] Création du lock file");
            free(request);
            pthread_exit(NULL);
        }
        fputs(client_info, lock_fp);
        fclose(lock_fp);
    }

    // Création d'une socket pour le transfert des données
    int data_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (data_sockfd < 0) {
        perror("[ERROR] Création de la socket de transfert");
        unlink(lock_path);
        free(request);
        pthread_exit(NULL);
    }

    struct sockaddr_in data_addr;
    memset(&data_addr, 0, sizeof(data_addr));
    data_addr.sin_family = AF_INET;
    data_addr.sin_addr.s_addr = INADDR_ANY;
    data_addr.sin_port = 0;
    if (bind(data_sockfd, (struct sockaddr*)&data_addr, sizeof(data_addr)) < 0) {
        perror("[ERROR] Bind sur la socket de transfert");
        close(data_sockfd);
        unlink(lock_path);
        free(request);
        pthread_exit(NULL);
    }

    socklen_t addr_len = sizeof(data_addr);
    if (getsockname(data_sockfd, (struct sockaddr*)&data_addr, &addr_len) == 0) {
        printf("[INFO] Socket de transfert bindée sur le port %d\n", ntohs(data_addr.sin_port));
    } else {
        perror("[ERROR] getsockname");
    }

    // Traitement de la demande selon l'opcode (lecture ou écriture)
    if (request->opcode == RRQ) {
        printf("[THREAD] Lecture du fichier demandée : %s\n", request->filename);
        send_file(data_sockfd, request->client_addr, request->filename);
    } else if (request->opcode == WRQ) {
        printf("[THREAD] Écriture du fichier demandée : %s\n", request->filename);
        receive_file(data_sockfd, request->client_addr, request->filename);
    }

    // Fermeture de la socket et nettoyage
    close(data_sockfd);
    unlink(lock_path);
    free(request);
    pthread_exit(NULL);
}

// Fonction principale : création du socket serveur et gestion des requêtes clients
int main() {
    const int port = 6969;
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_size = sizeof(client_addr);
    char buffer[PACKET_SIZE];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);  // Créer une socket UDP
    if (sockfd < 0) {
        perror("[ERROR] Échec de la création du socket.");
        exit(1);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[ERROR] Échec du bind.");
        exit(1);
    }

    printf("[STARTING] Serveur TFTP en attente...\n");
    while (1) {
        memset(buffer, 0, PACKET_SIZE);  // Nettoyage du buffer
        recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&client_addr, &addr_size); // Attente d'une requête
        int opcode = (buffer[0] << 8) | buffer[1];
        char *filename = buffer + 2;
        client_request_t* request = malloc(sizeof(client_request_t));
        if (!request) continue;
        request->sockfd = sockfd;
        request->client_addr = client_addr;
        request->opcode = opcode;
        strncpy(request->filename, filename, sizeof(request->filename) - 1);
        pthread_t thread_id;
        pthread_create(&thread_id, NULL, handle_client_request, request);  // Créer un thread pour gérer la requête
        pthread_detach(thread_id);  // Détacher le thread pour qu'il se termine proprement
    }
    close(sockfd);  // Fermer le socket lorsque le serveur termine
    return 0;
}

