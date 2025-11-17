#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define BASE_PORT 8000
#define MAX_PROCESSES 100

// Types de messages
typedef enum {
    REQ,    // Demande de verrou
    ACK,    // Accusé de réception
    REL     // Libération de verrou
} MsgType;

// Structure d'un message
typedef struct {
    MsgType type;
    int timestamp;
    int pid;
} Message;

// Structure d'une requête dans la file
typedef struct {
    int timestamp;
    int pid;
    int active;  // 1 si la requête est active, 0 sinon
} Request;

// État global du processus
typedef struct {
    int my_pid;
    int num_processes;
    int clock;
    
    Request queue[MAX_PROCESSES * 10];  // File de requêtes
    int queue_size;
    
    int acks_received[MAX_PROCESSES];   // Timestamps des ACKs reçus
    int *sockets;                        // Sockets vers autres processus
    int completed[MAX_PROCESSES];        // Nombre de Lock complétés par chaque processus
    
    pthread_mutex_t mutex;
} State;

State state;

// ============ HORLOGE DE LAMPORT ============

void update_clock(int received_ts) {
    pthread_mutex_lock(&state.mutex);
    if (received_ts > state.clock) {
        state.clock = received_ts;
    }
    state.clock++;
    pthread_mutex_unlock(&state.mutex);
}

int get_clock() {
    pthread_mutex_lock(&state.mutex);
    state.clock++;
    int ts = state.clock;
    pthread_mutex_unlock(&state.mutex);
    return ts;
}

// ============ FILE DE REQUÊTES ============

// Ajouter une requête dans la file (ordre total : timestamp puis pid)
void add_request(int ts, int pid) {
    pthread_mutex_lock(&state.mutex);
    
    // Trouver la position d'insertion
    int pos = state.queue_size;
    for (int i = 0; i < state.queue_size; i++) {
        if (!state.queue[i].active) continue;
        
        if (state.queue[i].timestamp > ts || 
            (state.queue[i].timestamp == ts && state.queue[i].pid > pid)) {
            pos = i;
            break;
        }
    }
    
    // Décaler les éléments
    for (int i = state.queue_size; i > pos; i--) {
        state.queue[i] = state.queue[i - 1];
    }
    
    // Insérer la nouvelle requête
    state.queue[pos].timestamp = ts;
    state.queue[pos].pid = pid;
    state.queue[pos].active = 1;
    state.queue_size++;
    
    pthread_mutex_unlock(&state.mutex);
}

// Retirer une requête de la file
void remove_request(int pid) {
    pthread_mutex_lock(&state.mutex);
    
    for (int i = 0; i < state.queue_size; i++) {
        if (state.queue[i].active && state.queue[i].pid == pid) {
            state.queue[i].active = 0;
            break;
        }
    }
    
    pthread_mutex_unlock(&state.mutex);
}

// Vérifier si je suis en tête de file
int am_i_first() {
    pthread_mutex_lock(&state.mutex);
    
    for (int i = 0; i < state.queue_size; i++) {
        if (state.queue[i].active) {
            int result = (state.queue[i].pid == state.my_pid);
            pthread_mutex_unlock(&state.mutex);
            return result;
        }
    }
    
    pthread_mutex_unlock(&state.mutex);
    return 0;
}

// Obtenir le timestamp de ma requête
int get_my_request_ts() {
    pthread_mutex_lock(&state.mutex);
    
    for (int i = 0; i < state.queue_size; i++) {
        if (state.queue[i].active && state.queue[i].pid == state.my_pid) {
            int ts = state.queue[i].timestamp;
            pthread_mutex_unlock(&state.mutex);
            return ts;
        }
    }
    
    pthread_mutex_unlock(&state.mutex);
    return -1;
}

// Vérifier si tous les ACKs sont reçus
int all_acks_received() {
    int my_ts = get_my_request_ts();
    if (my_ts == -1) return 0;
    
    pthread_mutex_lock(&state.mutex);
    
    for (int i = 0; i < state.num_processes; i++) {
        if (i != state.my_pid && state.acks_received[i] < my_ts) {
            pthread_mutex_unlock(&state.mutex);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&state.mutex);
    return 1;
}

// ============ COMMUNICATION ============

// Envoyer un message à tous
void broadcast(Message *msg) {
    for (int i = 0; i < state.num_processes; i++) {
        if (i != state.my_pid) {
            send(state.sockets[i], msg, sizeof(Message), 0);
        }
    }
}

// Envoyer un message à un processus
void send_to(int target, Message *msg) {
    if (target != state.my_pid) {
        send(state.sockets[target], msg, sizeof(Message), 0);
    }
}

// Thread de réception des messages
void *receiver_thread(void *arg) {
    int sock = *(int *)arg;
    Message msg;
    
    while (recv(sock, &msg, sizeof(Message), 0) > 0) {
        update_clock(msg.timestamp);
        
        if (msg.type == REQ) {
            // Requête reçue : ajouter dans la file et envoyer ACK
            add_request(msg.timestamp, msg.pid);
            
            Message ack;
            ack.type = ACK;
            ack.timestamp = get_clock();
            ack.pid = state.my_pid;
            send_to(msg.pid, &ack);
            
        } else if (msg.type == ACK) {
            // ACK reçu : mémoriser le timestamp
            pthread_mutex_lock(&state.mutex);
            state.acks_received[msg.pid] = msg.timestamp;
            pthread_mutex_unlock(&state.mutex);
            
        } else if (msg.type == REL) {
            // Libération : retirer de la file et compter
            remove_request(msg.pid);
            pthread_mutex_lock(&state.mutex);
            state.completed[msg.pid]++;
            pthread_mutex_unlock(&state.mutex);
        }
    }
    
    return NULL;
}

// ============ ALGORITHME DU VERROU ============

void request_lock() {
    int ts = get_clock();
    
    // Ajouter ma requête dans ma file
    add_request(ts, state.my_pid);
    
    // Broadcast ma requête
    Message req;
    req.type = REQ;
    req.timestamp = ts;
    req.pid = state.my_pid;
    broadcast(&req);
    
    // Attendre que je puisse prendre le verrou
    while (!am_i_first() || !all_acks_received()) {
        usleep(100);   // Attendre en microsecondes
    }
}

void release_lock() {
    int ts = get_clock();
    
    // Retirer ma requête de ma file
    remove_request(state.my_pid);   
    
    // Broadcast la libération
    Message rel;
    rel.type = REL;
    rel.timestamp = ts;
    rel.pid = state.my_pid;
    broadcast(&rel);
    
    // Compter mes Lock complétés
    pthread_mutex_lock(&state.mutex);
    state.completed[state.my_pid]++;
    pthread_mutex_unlock(&state.mutex);
}

// ============ CONFIGURATION RÉSEAU ============

void setup_network() {
    // Créer socket serveur
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(BASE_PORT + state.my_pid);
    
    bind(server_sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_sock, 10);
    
    // Attendre que tous les serveurs soient prêts
    sleep(1);
    
    // Se connecter aux autres processus
    state.sockets = malloc(state.num_processes * sizeof(int));
    
    for (int i = 0; i < state.num_processes; i++) {
        if (i == state.my_pid) continue;
        
        if (i < state.my_pid) {
            // Je me connecte aux processus de plus petit ID
            state.sockets[i] = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in peer = {0};
            peer.sin_family = AF_INET;
            peer.sin_addr.s_addr = inet_addr("127.0.0.1");
            peer.sin_port = htons(BASE_PORT + i);
            
            while (connect(state.sockets[i], (struct sockaddr *)&peer, sizeof(peer)) < 0) {
                usleep(10000);
            }
            
        } else {
            // J'accepte les connexions des processus de plus grand ID
            state.sockets[i] = accept(server_sock, NULL, NULL);
        }
        
        // Lancer thread de réception
        pthread_t tid;
        int *sock_ptr = malloc(sizeof(int));
        *sock_ptr = state.sockets[i];
        pthread_create(&tid, NULL, receiver_thread, sock_ptr);
        pthread_detach(tid);
    }
}

// ============ MAIN ============

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <pid> <test_file>\n", argv[0]);
        return 1;
    }
    
    // Initialisation
    state.my_pid = atoi(argv[1]);
    char *filename = argv[2];
    
    // Lire le nombre de processus
    FILE *f = fopen(filename, "r");
    fscanf(f, "%d", &state.num_processes);
    fclose(f);
    
    // Initialiser l'état
    state.clock = 0;
    state.queue_size = 0;
    pthread_mutex_init(&state.mutex, NULL);
    
    for (int i = 0; i < state.num_processes; i++) {
        state.acks_received[i] = 0;
        state.completed[i] = 0;
    }
    
    // Configuration réseau
    setup_network();
    
    // Exécuter les instructions du fichier
    f = fopen(filename, "r");
    int n;
    fscanf(f, "%d", &n);
    
    char line[256];
    fgets(line, sizeof(line), f);  // Consommer le newline
    
    while (fgets(line, sizeof(line), f)) {
        int pid, value;
        char cmd[10];
        
        if (sscanf(line, "%d %s %d", &pid, cmd, &value) == 3) {
            if (pid != state.my_pid) continue;
            
            if (strcmp(cmd, "Lock") == 0) {
                request_lock();
                
                // Exécuter la section critique
                char critical_cmd[256];
                snprintf(critical_cmd, sizeof(critical_cmd), "./critical %d %d", state.my_pid, value);
                system(critical_cmd);
                
                release_lock();
                
            } else if (strcmp(cmd, "Wait") == 0) {
                // Attendre que le processus 'value' ait complété au moins 1 Lock
                while (state.completed[value] < 1) {
                    usleep(1000);
                }
            }
        }
    }
    fclose(f);
    
    // Attendre un peu avant de terminer
    sleep(2);
    
    return 0;
}
