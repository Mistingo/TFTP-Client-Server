#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SIZE 516

void send_file_data(FILE* fp, int sockfd, struct sockaddr_in addr, char* filename) {
    int n;
    char buffer[SIZE];

    while (fgets(buffer, SIZE, fp) != NULL) {
        n = sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr*)&addr, sizeof(addr));
        if (n == -1) {
            perror("tftp> Échec de l'envoi des données.");
            exit(1);
        }
    }

    strcpy(buffer, "END");
    sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr*)&addr, sizeof(addr));
    fclose(fp);
}

void receive_file_data(int sockfd, struct sockaddr_in addr, char* filename) {
    int n;
    char buffer[SIZE];
    socklen_t addr_size = sizeof(addr);

    FILE* fp = fopen(filename, "w");
    if (fp == NULL) {
        perror("tftp> Impossible de créer le fichier.");
        return;
    }

    while (1) {
        memset(buffer, 0, SIZE);
        n = recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr*)&addr, &addr_size);
        if (n <= 0 || strcmp(buffer, "END") == 0) {
            break;
        }
        fwrite(buffer, 1, n, fp);
    }

    fclose(fp);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *ip = argv[1];
    const int port = 8080;
    int sockfd;
    struct sockaddr_in server_addr;
    char command[SIZE], filename[SIZE];

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
        fgets(command, sizeof(command), stdin);
        command[strcspn(command, "\n")] = 0;

        if (strncmp(command, "put ", 4) == 0) {
            strcpy(filename, command + 4);
            FILE *fp = fopen(filename, "r");
            if (fp == NULL) {
                perror("tftp> Impossible d'ouvrir le fichier ");
                continue;
            }
            char request[SIZE + 10];
            snprintf(request, sizeof(request), "put %s", filename);
            sendto(sockfd, request, strlen(request), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
            send_file_data(fp, sockfd, server_addr, filename);
        } 
        else if (strncmp(command, "get ", 4) == 0) {
            strcpy(filename, command + 4);
            FILE *fp = fopen(filename, "r");
            if (fp == NULL) {
                perror("tftp> Impossible de trouver le fichier ");
                continue;
            }
            char request[SIZE + 10];
            snprintf(request, sizeof(request), "get %s", filename);
            sendto(sockfd, request, strlen(request), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
            receive_file_data(sockfd, server_addr, filename);
        } 
        else if (strcmp(command, "quit") == 0) {
            char request[SIZE + 10];
            snprintf(request, sizeof(request), "quit");
            sendto(sockfd, request, strlen(request), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
            break;
        } 
        else {
            printf("tftp> Commande invalide.\n");
        }
    }

    close(sockfd);
    return 0;
}
