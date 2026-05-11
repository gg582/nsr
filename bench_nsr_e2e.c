#define _GNU_SOURCE
#include <nsr/omni.h>
#include <ttak/timing/timing.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>
#include <sched.h>
#include <fcntl.h>

static pid_t g_gk_pid = 0;
static pid_t g_logic_pid = 0;
static bool g_running = true;

void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

void spawn_gatekeeper(nsr_shm_ring_t *l2g, nsr_shm_ring_t *g2l, const char *target) {
    g_gk_pid = fork();
    if (g_gk_pid == 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        sched_setaffinity(0, sizeof(cpuset), &cpuset);
        nsr_omni_gatekeeper_omega(l2g, g2l, target);
        exit(0);
    }
}

void spawn_logic(nsr_shm_ring_t *g2l, nsr_shm_ring_t *l2g, nsr_shm_ring_large_t *l2t) {
    g_logic_pid = fork();
    if (g_logic_pid == 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(2, &cpuset);
        sched_setaffinity(0, sizeof(cpuset), &cpuset);
        nsr_omni_logic_omega(g2l, l2g, l2t);
        exit(0);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <target_ip> [duration_s]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int duration = (argc > 2) ? atoi(argv[2]) : 10;
    signal(SIGINT, signal_handler);
    signal(SIGALRM, signal_handler);
    alarm(duration);

    size_t shm_size = sizeof(nsr_shm_ring_t);
    size_t shm_large_size = sizeof(nsr_shm_ring_large_t);
    nsr_shm_ring_t *l2g = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    nsr_shm_ring_t *g2l = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    nsr_shm_ring_large_t *l2t = mmap(NULL, shm_large_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    memset(l2g, 0, shm_size);
    memset(g2l, 0, shm_size);
    memset(l2t, 0, shm_large_size);

    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    spawn_gatekeeper(l2g, g2l, argv[1]);
    spawn_logic(g2l, l2g, l2t);

    nsr_omni_state_t last_state;
    memset(&last_state, 0, sizeof(last_state));

    while (g_running) {
        nsr_omni_state_t new_state;
        while (nsr_shm_ring_large_pop(l2t, &new_state, sizeof(new_state))) {
            memcpy(&last_state, &new_state, sizeof(last_state));
        }
        struct timespec ts = {0, 100000000}; // 100ms
        nanosleep(&ts, &ts);
    }

    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    kill(g_gk_pid, SIGTERM);
    kill(g_logic_pid, SIGTERM);
    waitpid(g_gk_pid, NULL, 0);
    waitpid(g_logic_pid, NULL, 0);

    uint32_t total_sent = 0;
    uint32_t total_recv = 0;
    for (int i = 0; i < NSR_MAX_HOPS; i++) {
        total_sent += last_state.hops[i].sent;
        total_recv += last_state.hops[i].recv;
    }

    double elapsed = (end_ts.tv_sec - start_ts.tv_sec) + (end_ts.tv_nsec - start_ts.tv_nsec) / 1e9;
    printf("\n--- NSR E2E BENCHMARK RESULT ---\n");
    printf("Probes Sent: %u\n", total_sent);
    printf("Observations Recv: %u\n", total_recv);
    printf("Elapsed Time: %.2f s\n", elapsed);
    printf("Throughput: %.2f probes/s\n", total_sent / elapsed);

    return 0;
}
