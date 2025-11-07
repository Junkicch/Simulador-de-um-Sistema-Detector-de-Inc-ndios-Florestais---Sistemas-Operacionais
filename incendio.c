// Simulação detector de incêndio com sensores (threads), thread central e bombeiro
// Compilar: gcc -O2 -pthread -o incendio incendio.c

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <signal.h>

#define N 30
#define SENSOR_SPACING 5      // espaçamento entre sensores (ajuste)
#define FIRE_INTERVAL_SEC 5   // gera fogo a cada 5s
#define SENSOR_POLL_SEC 1     // cada sensor verifica vizinhança 1s
#define FIREFIGHTER_TIME_SEC 2// tempo para apagar fogo (2s)
#define MSG_HISTORY 128       // quantos msg_ids lembrar por sensor
#define MAX_SENSORS 1024      // limite superior (segurança)

typedef struct message_s {
    long msg_id;
    int origin_sensor_id;
    int fire_r, fire_c;
    char time_str[9];
    struct message_s *next;
} message_t;

// map: 0 = árvore '.', 1 = fogo '@'
char grid[N][N];
pthread_mutex_t grid_mutex = PTHREAD_MUTEX_INITIALIZER;

// sensor map: -1 se não há sensor, senão sensor id
int sensor_id_map[N][N];

// sensor structure
typedef struct sensor_s {
    int id;
    int r, c;
    pthread_t tid;
    int alive;
    message_t *queue_head;
    message_t *queue_tail;
    pthread_mutex_t qmutex;
    pthread_cond_t qcond;
    // histórico de mensagens já processadas (msg_id)
    long processed[MSG_HISTORY];
    int proc_idx;
} sensor_t;

sensor_t *sensors[MAX_SENSORS];
int sensor_count = 0;
pthread_mutex_t sensors_mutex = PTHREAD_MUTEX_INITIALIZER;

// central thread queue (messages vindo dos sensores da borda)
message_t *central_q_head = NULL;
message_t *central_q_tail = NULL;
pthread_mutex_t central_q_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t central_q_cond = PTHREAD_COND_INITIALIZER;

// firefighter queue (coords a apagar)
typedef struct ff_node {
    int r, c;
    struct ff_node *next;
} ff_node_t;
ff_node_t *ff_head = NULL, *ff_tail = NULL;
pthread_mutex_t ff_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ff_cond = PTHREAD_COND_INITIALIZER;

// msg id generator
long global_msg_id = 1;
pthread_mutex_t global_msg_mutex = PTHREAD_MUTEX_INITIALIZER;

// control
volatile int running = 1;

// forward declarations
void send_message_to_sensor(sensor_t *s, message_t *msg);
void forward_message_from_sensor(sensor_t *s, message_t *msg);
void send_message_to_central(message_t *msg);
void enqueue_firefighter(int r, int c);

// util: now time hh:mm:ss
void now_time_str(char *buf) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    sprintf(buf, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
}

// create a message
message_t *create_message(int origin_id, int fr, int fc) {
    message_t *m = malloc(sizeof(message_t));
    if (!m) { perror("malloc"); exit(1); }
    pthread_mutex_lock(&global_msg_mutex);
    m->msg_id = global_msg_id++;
    pthread_mutex_unlock(&global_msg_mutex);
    m->origin_sensor_id = origin_id;
    m->fire_r = fr;
    m->fire_c = fc;
    now_time_str(m->time_str);
    m->next = NULL;
    return m;
}

// push to sensor queue (copia mensagem)
void send_message_to_sensor(sensor_t *s, message_t *msg) {
    if (!s || !s->alive) return;
    message_t *node = malloc(sizeof(message_t));
    if (!node) { perror("malloc"); return; }
    memcpy(node, msg, sizeof(message_t));
    node->next = NULL;

    pthread_mutex_lock(&s->qmutex);
    if (s->queue_tail == NULL) {
        s->queue_head = s->queue_tail = node;
    } else {
        s->queue_tail->next = node;
        s->queue_tail = node;
    }
    pthread_cond_signal(&s->qcond);
    pthread_mutex_unlock(&s->qmutex);
}

// pop message from sensor queue, returns allocated message_t*
message_t *sensor_queue_pop(sensor_t *s) {
    pthread_mutex_lock(&s->qmutex);
    while (running && s->queue_head == NULL && s->alive) {
        pthread_cond_wait(&s->qcond, &s->qmutex);
    }
    if (!running || !s->alive) {
        // allow draining if desired; here just return NULL
    }
    message_t *node = NULL;
    if (s->queue_head) {
        node = s->queue_head;
        s->queue_head = node->next;
        if (s->queue_head == NULL) s->queue_tail = NULL;
        node->next = NULL;
    }
    pthread_mutex_unlock(&s->qmutex);
    return node;
}

// helper: check if sensor already processed msg_id
int sensor_processed(sensor_t *s, long msg_id) {
    for (int i = 0; i < MSG_HISTORY; ++i)
        if (s->processed[i] == msg_id) return 1;
    return 0;
}
void sensor_mark_processed(sensor_t *s, long msg_id) {
    s->processed[s->proc_idx] = msg_id;
    s->proc_idx = (s->proc_idx + 1) % MSG_HISTORY;
}

// forward message: send to sensors in same row/col within distance 3
void forward_message_from_sensor(sensor_t *s, message_t *msg) {
    pthread_mutex_lock(&sensors_mutex);
    for (int d = 1; d <= 3; ++d) {
        int c1 = s->c - d;
        if (c1 >= 0 && sensor_id_map[s->r][c1] != -1) {
            int sid = sensor_id_map[s->r][c1];
            send_message_to_sensor(sensors[sid], msg);
        }
        int c2 = s->c + d;
        if (c2 < N && sensor_id_map[s->r][c2] != -1) {
            int sid = sensor_id_map[s->r][c2];
            send_message_to_sensor(sensors[sid], msg);
        }
        int r1 = s->r - d;
        if (r1 >= 0 && sensor_id_map[r1][s->c] != -1) {
            int sid = sensor_id_map[r1][s->c];
            send_message_to_sensor(sensors[sid], msg);
        }
        int r2 = s->r + d;
        if (r2 < N && sensor_id_map[r2][s->c] != -1) {
            int sid = sensor_id_map[r2][s->c];
            send_message_to_sensor(sensors[sid], msg);
        }
    }
    pthread_mutex_unlock(&sensors_mutex);
}

// central queue push
void send_message_to_central(message_t *msg) {
    message_t *m = malloc(sizeof(message_t));
    if (!m) { perror("malloc"); return; }
    memcpy(m, msg, sizeof(message_t));
    m->next = NULL;
    pthread_mutex_lock(&central_q_mutex);
    if (central_q_tail == NULL) {
        central_q_head = central_q_tail = m;
    } else {
        central_q_tail->next = m;
        central_q_tail = m;
    }
    pthread_cond_signal(&central_q_cond);
    pthread_mutex_unlock(&central_q_mutex);
}

// firefighter queue push
void enqueue_firefighter(int r, int c) {
    ff_node_t *n = malloc(sizeof(ff_node_t));
    if (!n) { perror("malloc"); return; }
    n->r = r; n->c = c; n->next = NULL;
    pthread_mutex_lock(&ff_mutex);
    if (ff_tail == NULL) { ff_head = ff_tail = n; }
    else { ff_tail->next = n; ff_tail = n; }
    pthread_cond_signal(&ff_cond);
    pthread_mutex_unlock(&ff_mutex);
}

// Utility to check border sensor
int is_border_sensor(sensor_t *s) {
    return (s->r == 0 || s->r == N-1 || s->c == 0 || s->c == N-1);
}

// sensor thread function
void *sensor_thread_func(void *arg) {
    sensor_t *s = (sensor_t*)arg;
    while (running && s->alive) {
        // 1) check neighbors (8-neighborhood) for '@'
        int detected = 0;
        pthread_mutex_lock(&grid_mutex);
        for (int di = -1; di <= 1 && !detected; ++di) {
            for (int dj = -1; dj <= 1; ++dj) {
                int ni = s->r + di, nj = s->c + dj;
                if (ni < 0 || ni >= N || nj < 0 || nj >= N) continue;
                if (grid[ni][nj] == '@') { detected = 1; break; }
            }
        }
        pthread_mutex_unlock(&grid_mutex);

        if (detected) {
            // create message and send to sensors at distance <=3 in h/v
            message_t *m = create_message(s->id, -1, -1); // placeholder
            m->fire_r = -1; m->fire_c = -1;
            // find any fire neighbor and set coords (first found)
            pthread_mutex_lock(&grid_mutex);
            for (int di = -1; di <= 1 && m->fire_r==-1; ++di) {
                for (int dj = -1; dj <= 1; ++dj) {
                    int ni = s->r + di, nj = s->c + dj;
                    if (ni < 0 || ni >= N || nj < 0 || nj >= N) continue;
                    if (grid[ni][nj] == '@') { m->fire_r = ni; m->fire_c = nj; break; }
                }
            }
            pthread_mutex_unlock(&grid_mutex);
            if (m->fire_r == -1) {
                free(m);
            } else {
                // set origin sensor id properly
                m->origin_sensor_id = s->id;
                // sensor processes own message immediately (mark processed) to avoid loops
                sensor_mark_processed(s, m->msg_id);
                // forward to neighbors (h/v distance<=3)
                forward_message_from_sensor(s, m);
                // if this sensor is on border, send directly to central as well
                if (is_border_sensor(s)) {
                    send_message_to_central(m);
                }
                free(m);
            }
        }

        // 2) process incoming messages (if any)
        // pop and handle each message: if not seen, mark processed, forward, and if border forward to central
        message_t *node = NULL;
        while ((node = sensor_queue_pop(s)) != NULL) {
            message_t msg = *node;
            free(node);
            if (sensor_processed(s, msg.msg_id)) continue; // already processed
            sensor_mark_processed(s, msg.msg_id);

            // if this sensor is on border, forward to central
            if (is_border_sensor(s)) {
                send_message_to_central(&msg);
            }
            // forward to other sensors horizontally/vertically distance<=3
            forward_message_from_sensor(s, &msg);
        }

        // sleep until next poll
        for (int i = 0; i < SENSOR_POLL_SEC && running && s->alive; ++i) sleep(1);
    }

    // clean-up: if destroyed or exiting, free any queued messages
    pthread_mutex_lock(&s->qmutex);
    message_t *cur = s->queue_head;
    while (cur) {
        message_t *n = cur->next;
        free(cur);
        cur = n;
    }
    s->queue_head = s->queue_tail = NULL;
    pthread_mutex_unlock(&s->qmutex);

    return NULL;
}

// central thread
void *central_thread_func(void *arg) {
    (void)arg;
    #define CENTRAL_HISTORY 2048
    long processed[CENTRAL_HISTORY];
    int pidx = 0;
    memset(processed, 0, sizeof(processed));

    for (;;) {
        pthread_mutex_lock(&central_q_mutex);
        /* aguarda enquanto estiver rodando e fila vazia */
        while (running && central_q_head == NULL) {
            pthread_cond_wait(&central_q_cond, &central_q_mutex);
        }
        /* se não há mensagens e running==0, sair (fila já vazia) */
        if (central_q_head == NULL && !running) {
            pthread_mutex_unlock(&central_q_mutex);
            break;
        }
        /* retira uma mensagem (pode ser NULL se race, verificar) */
        message_t *m = central_q_head;
        if (m) {
            central_q_head = m->next;
            if (central_q_head == NULL) central_q_tail = NULL;
        }
        pthread_mutex_unlock(&central_q_mutex);

        if (!m) continue;

        int seen = 0;
        for (int i = 0; i < CENTRAL_HISTORY; ++i) if (processed[i] == m->msg_id) { seen = 1; break; }
        if (!seen) {
            processed[pidx] = m->msg_id; pidx = (pidx+1)%CENTRAL_HISTORY;
            FILE *f = fopen("incendios.log", "a");
            if (f) {
                fprintf(f, "MSG %ld | sensor %d | fire (%d,%d) | %s\n",
                        m->msg_id, m->origin_sensor_id, m->fire_r, m->fire_c, m->time_str);
                fclose(f);
            } else {
                perror("fopen incendios.log");
            }
            enqueue_firefighter(m->fire_r, m->fire_c);
        }
        free(m);
    }
    return NULL;
}

// firefighter thread: takes coords, waits 2s, then extinguishes
void *firefighter_thread_func(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&ff_mutex);
        while (running && ff_head == NULL) pthread_cond_wait(&ff_cond, &ff_mutex);
        /* se fila vazia e running==0, sair */
        if (ff_head == NULL && !running) { pthread_mutex_unlock(&ff_mutex); break; }
        ff_node_t *n = ff_head;
        if (n) {
            ff_head = n->next;
            if (ff_head == NULL) ff_tail = NULL;
        }
        pthread_mutex_unlock(&ff_mutex);

        if (!n) continue;

        int r = n->r, c = n->c;
        free(n);
        sleep(FIREFIGHTER_TIME_SEC);
        pthread_mutex_lock(&grid_mutex);
        int was_fire = 0;
        if (r >= 0 && r < N && c >= 0 && c < N && grid[r][c] == '@') {
            grid[r][c] = '.';
            was_fire = 1;
        }
        pthread_mutex_unlock(&grid_mutex);

        if (was_fire) {
            char tbuf[9];
            now_time_str(tbuf);
            FILE *f = fopen("incendios.log", "a");
            if (f) {
                fprintf(f, "EXT %s | extinguished (%d,%d)\n", tbuf, r, c);
                fclose(f);
            }
            printf("Fogo apagado em (%d,%d) às %s\n", r, c, tbuf);
            fflush(stdout);
        }
    }
    return NULL;
}

// display function
void print_grid() {
    // lock sensors first, then grid (same order used elsewhere) to avoid deadlock
    pthread_mutex_lock(&sensors_mutex);
    pthread_mutex_lock(&grid_mutex);

    // clear terminal
    printf("\033[H\033[J");
    printf("Mapa 30x30 - '.' árvore, '@' fogo, 'T' sensor, 'x' sensor destruído\n");
    printf("Incêndios gerados a cada %d s. Pressione Ctrl+C para sair.\n\n", FIRE_INTERVAL_SEC);
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (sensor_id_map[i][j] != -1) {
                sensor_t *s = sensors[sensor_id_map[i][j]];
                if (!s->alive) putchar('x');
                else putchar('T');
            } else {
                if (grid[i][j] == '@') putchar('@');
                else putchar('.');
            }
            putchar(' ');
        }
        putchar('\n');
    }

    pthread_mutex_unlock(&grid_mutex);
    pthread_mutex_unlock(&sensors_mutex);
    fflush(stdout);
}

// initialize sensors with spacing
void setup_sensors() {
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) sensor_id_map[i][j] = -1;

    for (int i = 0; i < N; i += SENSOR_SPACING) {
        for (int j = 0; j < N; j += SENSOR_SPACING) {
            sensor_t *s = malloc(sizeof(sensor_t));
            if (!s) { perror("malloc"); exit(1); }
            s->id = sensor_count;
            s->r = i; s->c = j;
            s->alive = 1;
            s->queue_head = s->queue_tail = NULL;
            pthread_mutex_init(&s->qmutex, NULL);
            pthread_cond_init(&s->qcond, NULL);
            s->proc_idx = 0;
            for (int k = 0; k < MSG_HISTORY; ++k) s->processed[k] = 0;
            sensors[sensor_count] = s;
            sensor_id_map[i][j] = sensor_count;
            sensor_count++;
        }
    }
}

// generate a random fire at an empty cell or on sensor
void generate_random_fire() {
    int r = rand() % N;
    int c = rand() % N;

    // lock sensors first, then grid to maintain a consistent lock order and avoid deadlock
    pthread_mutex_lock(&sensors_mutex);
    pthread_mutex_lock(&grid_mutex);

    // place fire regardless of what's there
    grid[r][c] = '@';

    // if there's a sensor at (r,c), destroy it
    int sid = sensor_id_map[r][c];
    if (sid != -1) {
        sensor_t *s = sensors[sid];
        // mark dead
        s->alive = 0;
        // remove from sensor map so others know it's not a sensor anymore
        sensor_id_map[r][c] = -1;
        // wake its queue to let thread exit
        pthread_mutex_lock(&s->qmutex);
        pthread_cond_signal(&s->qcond);
        pthread_mutex_unlock(&s->qmutex);
    }

    pthread_mutex_unlock(&grid_mutex);
    pthread_mutex_unlock(&sensors_mutex);
}

// signal-waiting thread: aguarda SIGINT com sigwait e acorda as threads com segurança
void *signal_thread_func(void *arg) {
    (void)arg;
    sigset_t set;
    int sig;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);

    if (sigwait(&set, &sig) == 0) {
        if (sig == SIGINT) {
            running = 0;

            // acordar central e bombeiro
            pthread_mutex_lock(&central_q_mutex);
            pthread_cond_broadcast(&central_q_cond);
            pthread_mutex_unlock(&central_q_mutex);

            pthread_mutex_lock(&ff_mutex);
            pthread_cond_broadcast(&ff_cond);
            pthread_mutex_unlock(&ff_mutex);

            // avisar sensores (marca alive=0 e acorda filas)
            pthread_mutex_lock(&sensors_mutex);
            for (int i = 0; i < sensor_count; ++i) {
                sensor_t *s = sensors[i];
                if (!s) continue;
                pthread_mutex_lock(&s->qmutex);
                s->alive = 0;
                pthread_cond_broadcast(&s->qcond);
                pthread_mutex_unlock(&s->qmutex);
            }
            pthread_mutex_unlock(&sensors_mutex);
        }
    }
    return NULL;
}

int main() {
    srand(time(NULL));

    // bloquear SIGINT em todas as threads criadas a seguir e tratar via sigwait em thread dedicada
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    // criar thread que fará sigwait(SIGINT)
    pthread_t sig_tid;
    pthread_create(&sig_tid, NULL, signal_thread_func, NULL);

    // init grid
    pthread_mutex_lock(&grid_mutex);
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) grid[i][j] = '.';
    pthread_mutex_unlock(&grid_mutex);

    setup_sensors();

    // create sensor threads
    for (int i = 0; i < sensor_count; ++i) {
        pthread_create(&sensors[i]->tid, NULL, sensor_thread_func, sensors[i]);
    }

    // create central and firefighter threads
    pthread_t central_tid, ff_tid;
    pthread_create(&central_tid, NULL, central_thread_func, NULL);
    pthread_create(&ff_tid, NULL, firefighter_thread_func, NULL);

    // main loop: display every 1s and generate fire each FIRE_INTERVAL_SEC
    int elapsed = 0;
    while (running) {
        print_grid();
        sleep(1);
        elapsed++;
        if (elapsed % FIRE_INTERVAL_SEC == 0) {
            generate_random_fire();
        }
    }

    // join threads
    // sensors may have been marked dead; still join their threads
    for (int i = 0; i < sensor_count; ++i) {
        pthread_join(sensors[i]->tid, NULL);
    }
    pthread_join(central_tid, NULL);
    pthread_join(ff_tid, NULL);

    // cleanup
    for (int i = 0; i < sensor_count; ++i) {
        pthread_mutex_destroy(&sensors[i]->qmutex);
        pthread_cond_destroy(&sensors[i]->qcond);
        free(sensors[i]);
    }
    pthread_mutex_destroy(&grid_mutex);
    pthread_mutex_destroy(&sensors_mutex);
    pthread_mutex_destroy(&central_q_mutex);
    pthread_mutex_destroy(&ff_mutex);
    pthread_cond_destroy(&central_q_cond);
    pthread_cond_destroy(&ff_cond);
    pthread_mutex_destroy(&global_msg_mutex); // destroy the msg id mutex

    printf("\nSimulação terminada.\n");
    return 0;
}
