
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define UDP_PORT       9000          // Porta do servidor (deve coincidir)
#define INTERVALO_MS   500           // Intervalo entre envios (500 ms)

/* Estrutura idêntica à usada no servidor (deve ter o mesmo layout) */
typedef struct {
    int sensor_id;
    struct timeval timestamp;
    float temperatura;
    float umidade;
    float pressao;
} Leitura;

/* Gera um número float pseudo-aleatório no intervalo [min, max]. */
float rand_range(float min, float max) {
    return min + (float)rand() / RAND_MAX * (max - min);
}

int main(int argc, char *argv[]) {
    // Verifica argumento (ID do sensor)
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <id_sensor>\n", argv[0]);
        return 1;
    }
    int sensor_id = atoi(argv[1]);
    if (sensor_id < 1 || sensor_id > 5) {   // Máximo NUM_SENSORES = 5
        fprintf(stderr, "ID do sensor deve estar entre 1 e 5\n");
        return 1;
    }

    // Inicializa gerador de números aleatórios com uma semente diferente por sensor
    srand(time(NULL) ^ (sensor_id << 16));

    // Cria socket UDP
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    // Configura endereço do servidor (localhost, porta UDP_PORT)
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    printf("[Sensor %d] Iniciando, enviando a cada %d ms\n", sensor_id, INTERVALO_MS);

    Leitura leitura;
    while (1) {
        // Preenche a estrutura com dados aleatórios realistas
        leitura.sensor_id = sensor_id;
        gettimeofday(&leitura.timestamp, NULL);           // Timestamp atual
        leitura.temperatura = rand_range(-10.0, 40.0);    // Faixa de temperatura
        leitura.umidade     = rand_range(0.0, 100.0);     // Umidade relativa
        leitura.pressao     = rand_range(950.0, 1050.0);  // Pressão atmosférica

        // Envia o datagrama via UDP
        ssize_t n = sendto(sock, &leitura, sizeof(leitura), 0,
                           (struct sockaddr*)&server_addr, sizeof(server_addr));
        if (n != sizeof(leitura)) {
            perror("sendto");
        } else {
            printf("[Sensor %d] Enviado: T=%.2f U=%.2f P=%.2f\n",
                   sensor_id, leitura.temperatura, leitura.umidade, leitura.pressao);
        }

        // Aguarda o intervalo definido (INTERVALO_MS milissegundos)
        usleep(INTERVALO_MS * 1000);
    }

    close(sock);   // Nunca alcançado (loop infinito), mas mantido por boas práticas
    return 0;
}
