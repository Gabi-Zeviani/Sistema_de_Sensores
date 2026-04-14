
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

//matrícula (6234) 
#define NUM_SENSORES       5    // (6234 % 4) + 3 = 2 + 3 = 5
#define MAX_CONSULTAS      4    // ((6234/10) % 3) + 2 = (623 % 3)+2 = 2+2=4
#define TAM_BUFFER_CIRC    150  // ((6234/100)%5+1)*50 = (62%5+1)*50 = (2+1)*50=150
#define INTERVALO_LEIT_MS  500  // ((6234/1000)%3+1)*500 = (6%3+1)*500 = (0+1)*500=500

#define UDP_PORT           9000   // Porta para receber dados dos sensores
#define TCP_PORT           9001   // Porta para atender clientes de consulta

/* Estrutura que representa uma leitura enviada por um sensor */
typedef struct {
    int sensor_id;                // Identificador do sensor (1..NUM_SENSORES)
    struct timeval timestamp;     // Momento da coleta (preenchido pelo sensor)
    float temperatura;            // Em graus Celsius
    float umidade;                // Percentual (0-100)
    float pressao;                // Em hPa (aprox. 950-1050)
} Leitura;

/* Buffer circular - Armazena as últimas TAM_BUFFER_CIRC leituras. Quando o buffer enche, as leituras mais antigas são sobrescritas.*/
static Leitura buffer[TAM_BUFFER_CIRC];
static int buffer_write_idx = 0;   // Próxima posição a escrever
static int buffer_count = 0;       // Número atual de leituras armazenadas

/* Sincronização do buffer: mutex protege as variáveis acima, e a condição serve para que threads de consulta possam esperar até que pelo menos uma leitura esteja disponível (evita busy waiting).*/
static pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t buffer_cond = PTHREAD_COND_INITIALIZER;

/* Semáforo contador para limitar o número de conexões TCP simultâneas.*/
static sem_t conn_sem;

/* Estatísticas globais: total de leituras recebidas, consultas atendidas, bytes enviados em respostas. Protegidas por um mutex próprio.*/
static unsigned long total_readings = 0;
static unsigned long total_queries = 0;
static unsigned long total_bytes_sent = 0;
static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Controle de encerramento do servidor.*/
static volatile int server_running = 1;

/* Socket TCP usado para aceitar conexões. Tornado global para que o signal_handler possa chamar shutdown() nele, desbloqueando o accept().*/
static int tcp_server_sock = -1;

/* IDs das threads principais (para aguardar término) */
static pthread_t udp_thread, tcp_acceptor_thread;

/* Tempo de início do servidor (usado para calcular tempo total de execução) */
static time_t start_time;

void *thread_udp_receiver(void *arg);
void *thread_tcp_acceptor(void *arg);
void *handle_query_client(void *arg);
void add_reading(Leitura *r);
int get_report(int n, char *out_buf, size_t out_size);
void signal_handler(int sig);
void print_stats_and_cleanup(void);

/* Implementação do buffer circular */

/** add_reading - Insere uma nova leitura no buffer circular.
 * @r: Ponteiro para a leitura a ser inserida.
 * Protegida por mutex. Após inserir, sinaliza (broadcast) para todas as threads de consulta que podem estar esperando dados.*/
void add_reading(Leitura *r) {
    pthread_mutex_lock(&buffer_mutex);
    buffer[buffer_write_idx] = *r;
    buffer_write_idx = (buffer_write_idx + 1) % TAM_BUFFER_CIRC;
    if (buffer_count < TAM_BUFFER_CIRC)
        buffer_count++;
    // Acorda todas as threads bloqueadas em get_report (caso haja)
    pthread_cond_broadcast(&buffer_cond);
    pthread_mutex_unlock(&buffer_mutex);
}

/**
 * get_report - Gera um relatório com média, mínimo e máximo das últimas n leituras.
 * @n: Número de leituras a considerar (se maior que o disponível, usa todas).
 * @out_buf: Buffer de saída onde o relatório será escrito.
 * @out_size: Tamanho do buffer de saída.
 * Retorna o número de caracteres escritos (como snprintf).
* Se não houver nenhuma leitura, espera pela condição (bloqueia até que alguma leitura chegue ou o servidor seja encerrado).*/
int get_report(int n, char *out_buf, size_t out_size) {
    pthread_mutex_lock(&buffer_mutex);
    // Enquanto não houver dados e o servidor ainda estiver rodando, aguarda
    while (buffer_count == 0 && server_running) {
        pthread_cond_wait(&buffer_cond, &buffer_mutex);
    }
    // Se o servidor encerrou sem dados, retorna mensagem informativa
    if (!server_running && buffer_count == 0) {
        pthread_mutex_unlock(&buffer_mutex);
        return snprintf(out_buf, out_size, "Servidor encerrando, sem dados.\n");
    }

    int available = buffer_count;
    int use_n = (n > available) ? available : n;   // Não pedir mais do que existe
    if (use_n == 0) {
        pthread_mutex_unlock(&buffer_mutex);
        return snprintf(out_buf, out_size, "Nenhuma leitura disponível ainda.\n");
    }

    // Calcula o índice da leitura mais antiga disponível (início do buffer circular)
    int start = (buffer_write_idx - buffer_count + TAM_BUFFER_CIRC) % TAM_BUFFER_CIRC;
    float sum_temp = 0, sum_umid = 0, sum_press = 0;
    float max_temp = -1e9, min_temp = 1e9;
    float max_umid = -1e9, min_umid = 1e9;
    float max_press = -1e9, min_press = 1e9;

    // Percorre as últimas 'use_n' leituras (em ordem cronológica)
    for (int i = 0; i < use_n; i++) {
        int idx = (start + i) % TAM_BUFFER_CIRC;
        Leitura *r = &buffer[idx];
        sum_temp += r->temperatura;
        sum_umid += r->umidade;
        sum_press += r->pressao;

        if (r->temperatura > max_temp) max_temp = r->temperatura;
        if (r->temperatura < min_temp) min_temp = r->temperatura;
        if (r->umidade > max_umid) max_umid = r->umidade;
        if (r->umidade < min_umid) min_umid = r->umidade;
        if (r->pressao > max_press) max_press = r->pressao;
        if (r->pressao < min_press) min_press = r->pressao;
    }

    pthread_mutex_unlock(&buffer_mutex);

    // Formata o relatório (médias, mínimos, máximos)
    return snprintf(out_buf, out_size,
        "RELATÓRIO (últimas %d leituras de %d armazenadas):\n"
        "Temperatura: média=%.2f  min=%.2f  max=%.2f\n"
        "Umidade:     média=%.2f  min=%.2f  max=%.2f\n"
        "Pressão:     média=%.2f  min=%.2f  max=%.2f\n",
        use_n, available,
        sum_temp / use_n, min_temp, max_temp,
        sum_umid / use_n, min_umid, max_umid,
        sum_press / use_n, min_press, max_press);
}

/*Thread UDP: recebe leituras dos sensores */
void *thread_udp_receiver(void *arg) {
    // Cria socket UDP1
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("socket UDP");
        server_running = 0;
        return NULL;
    }

    // Configura endereço local para bind (todas interfaces, porta UDP_PORT)
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(UDP_PORT);

    if (bind(udp_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind UDP");
        close(udp_sock);
        server_running = 0;
        return NULL;
    }

    printf("[UDP] Thread iniciada, aguardando dados na porta %d\n", UDP_PORT);

    Leitura leitura;
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);

    // Loop principal: recebe datagramas e insere no buffer
    while (server_running) {
        ssize_t n = recvfrom(udp_sock, &leitura, sizeof(leitura), 0,
                             (struct sockaddr*)&sender_addr, &sender_len);
        if (n == sizeof(leitura)) {
            add_reading(&leitura);

            // Atualiza estatística de leituras (protegida por mutex)
            pthread_mutex_lock(&stats_mutex);
            total_readings++;
            pthread_mutex_unlock(&stats_mutex);

            printf("[UDP] Leitura sensor=%d temp=%.2f umid=%.2f press=%.2f (total=%lu)\n",
                   leitura.sensor_id, leitura.temperatura, leitura.umidade,
                   leitura.pressao, total_readings);
        } else if (n < 0 && errno != EINTR) {
            perror("recvfrom UDP");
        }
    }

    close(udp_sock);
    printf("[UDP] Thread encerrada.\n");
    return NULL;
}

/* Thread de atendimento de um cliente TCP: Cada cliente é tratado em uma thread separada; Esta função é executada por cada thread criada a partir do acceptor.*/
void *handle_query_client(void *arg) {
    int client_fd = *(int*)arg;
    free(arg);   // Libera o ponteiro alocado no acceptor

    char buffer[1024];
    ssize_t total = 0;
    // Lê o comando do cliente até encontrar '\n' (proteção contra fragmentação TCP)
    while (total < sizeof(buffer)-1) {
        ssize_t n = recv(client_fd, buffer + total, sizeof(buffer)-1 - total, 0);
        if (n <= 0) break;
        total += n;
        if (strchr(buffer, '\n')) break;   // Comando completo
    }
    if (total == 0) {
        close(client_fd);
        sem_post(&conn_sem);   // Libera uma vaga no semáforo
        return NULL;
    }
    buffer[total] = '\0';

    printf("[TCP Cliente] Comando recebido: %s", buffer);

    int n_readings = 0;
    // Formato esperado: "REPORT <N>" (case-sensitive)
    if (sscanf(buffer, "REPORT %d", &n_readings) == 1 && n_readings > 0) {
        char report[4096];
        int len = get_report(n_readings, report, sizeof(report));
        send(client_fd, report, len, 0);   // Envia o relatório ao cliente

        // Atualiza estatísticas de consultas e bytes enviados
        pthread_mutex_lock(&stats_mutex);
        total_queries++;
        total_bytes_sent += len;
        pthread_mutex_unlock(&stats_mutex);

        printf("[TCP Cliente] Relatório enviado (%d bytes). Consultas=%lu, Bytes=%lu\n",
               len, total_queries, total_bytes_sent);
    } else {
        char *err_msg = "Comando inválido. Use: REPORT <N>\n";
        send(client_fd, err_msg, strlen(err_msg), 0);
    }

    close(client_fd);
    sem_post(&conn_sem);   // Libera uma vaga no semáforo
    return NULL;
}

/*Thread TCP acceptor 
 * Cria o socket de escuta, aceita conexões e cria uma thread de atendimento para cada cliente, respeitando o limite de conexões simultâneas.*/
void *thread_tcp_acceptor(void *arg) {
    tcp_server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_server_sock < 0) {
        perror("socket TCP");
        server_running = 0;
        return NULL;
    }

    // Permite reutilizar a porta rapidamente após encerramento
    int opt = 1;
    setsockopt(tcp_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(TCP_PORT);

    if (bind(tcp_server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind TCP");
        close(tcp_server_sock);
        tcp_server_sock = -1;
        server_running = 0;
        return NULL;
    }

    if (listen(tcp_server_sock, 10) < 0) {
        perror("listen");
        close(tcp_server_sock);
        tcp_server_sock = -1;
        server_running = 0;
        return NULL;
    }

    printf("[TCP] Thread acceptor iniciada, escutando na porta %d\n", TCP_PORT);

    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(tcp_server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || !server_running) {
                // Encerramento solicitado: sai do loop
                break;
            }
            perror("accept");
            continue;
        }

        // Bloqueia até que haja uma vaga (semáforo contador)
        sem_wait(&conn_sem);

        // Prepara argumento para a thread de atendimento
        int *pclient = malloc(sizeof(int));
        *pclient = client_fd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_query_client, pclient) != 0) {
            perror("pthread_create");
            free(pclient);
            close(client_fd);
            sem_post(&conn_sem);   // Devolve a vaga pois a thread não foi criada
        } else {
            pthread_detach(tid);   // Não precisamos esperar explicitamente
            // Exibe quantas conexões ainda podem ser aceitas
            int remaining;
            sem_getvalue(&conn_sem, &remaining);
            printf("[TCP] Nova conexão de %s:%d (conexões restantes: %d)\n",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), remaining);
        }
    }

    // Encerramento: fecha o socket TCP
    if (tcp_server_sock != -1) {
        close(tcp_server_sock);
        tcp_server_sock = -1;
    }
    printf("[TCP] Thread acceptor encerrada.\n");
    return NULL;
}

/* Tratamento de sinal (Ctrl+C, kill) - Força o encerramento gracioso do servidor, desbloqueando threads bloqueadas em accept() e em cond_wait().*/
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\n[SERVER] Sinal de término recebido. Encerrando...\n");
        server_running = 0;
        // Desbloqueia o accept() fazendo shutdown do socket TCP
        if (tcp_server_sock != -1) {
            shutdown(tcp_server_sock, SHUT_RDWR);
        }
        // Acorda threads de consulta que estão esperando dados
        pthread_cond_broadcast(&buffer_cond);
    }
}

/* Exibe estatísticas finais e calcula tempo total*/
void print_stats_and_cleanup(void) {
    time_t end_time = time(NULL);
    double elapsed = difftime(end_time, start_time);

    pthread_mutex_lock(&stats_mutex);
    printf("\n========== ESTATÍSTICAS FINAIS ==========\n");
    printf("Leituras recebidas:      %lu\n", total_readings);
    printf("Consultas atendidas:     %lu\n", total_queries);
    printf("Bytes enviados via TCP:  %lu\n", total_bytes_sent);
    printf("Tempo total de execução: %.2f segundos\n", elapsed);
    printf("========================================\n");
    pthread_mutex_unlock(&stats_mutex);
}


int main(void) {
    start_time = time(NULL);   // Marca o início da execução

    printf("[SERVER] Iniciando servidor agregador (Cenário B)\n");
    printf("Parâmetros: NUM_SENSORES=%d, MAX_CONSULTAS=%d, TAM_BUFFER=%d, INTERVALO_LEIT=%dms\n",
           NUM_SENSORES, MAX_CONSULTAS, TAM_BUFFER_CIRC, INTERVALO_LEIT_MS);

    // Inicializa o semáforo com o número máximo de conexões simultâneas
    sem_init(&conn_sem, 0, MAX_CONSULTAS);

    // Registra tratadores de sinal para encerramento limpo
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Cria a thread UDP (recepção de sensores)
    if (pthread_create(&udp_thread, NULL, thread_udp_receiver, NULL) != 0) {
        perror("pthread_create UDP");
        return 1;
    }
    // Cria a thread TCP acceptor (escuta e aceita clientes)
    if (pthread_create(&tcp_acceptor_thread, NULL, thread_tcp_acceptor, NULL) != 0) {
        perror("pthread_create TCP");
        server_running = 0;
        pthread_cancel(udp_thread);
        return 1;
    }

    // Loop principal: aguarda até que o sinal de término seja recebido
    while (server_running) {
        sleep(1);
    }

    // Aguarda o término das threads principais
    pthread_join(udp_thread, NULL);
    pthread_join(tcp_acceptor_thread, NULL);

    // Exibe estatísticas finais
    print_stats_and_cleanup();

    // Libera os recursos de sincronização
    sem_destroy(&conn_sem);
    pthread_mutex_destroy(&buffer_mutex);
    pthread_cond_destroy(&buffer_cond);
    pthread_mutex_destroy(&stats_mutex);

    printf("[SERVER] Encerrado.\n");
    return 0;
}
