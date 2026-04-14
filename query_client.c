
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TCP_PORT 9001       // Porta TCP do servidor (deve coincidir)
#define BUFSZ    8192       // Tamanho máximo da resposta

int main(int argc, char *argv[]) {
    // Valida argumentos
    if (argc != 4 || strcmp(argv[2], "REPORT") != 0) {
        fprintf(stderr, "Uso: %s <ip_servidor> REPORT <N>\n", argv[0]);
        return 1;
    }

    char *server_ip = argv[1];
    int n = atoi(argv[3]);
    if (n <= 0) {
        fprintf(stderr, "N deve ser positivo\n");
        return 1;
    }

    // Cria socket TCP
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    // Configura endereço do servidor
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TCP_PORT);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return 1;
    }

    // Conecta ao servidor
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    // Envia o comando (com quebra de linha final)
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "REPORT %d\n", n);
    send(sock, cmd, strlen(cmd), 0);

    // Lê a resposta do servidor (até o final ou até encontrar uma quebra de linha)
    char response[BUFSZ];
    ssize_t total = 0;
    ssize_t r;
    while (total < BUFSZ - 1) {
        r = recv(sock, response + total, BUFSZ - 1 - total, 0);
        if (r <= 0) break;
        total += r;
        if (response[total - 1] == '\n') break;   // Resposta completa (servidor envia \n final)
    }

    if (total > 0) {
        response[total] = '\0';
        printf("Resposta do servidor:\n%s", response);
    } else {
        printf("Nenhuma resposta recebida.\n");
    }

    close(sock);
    return 0;
}
