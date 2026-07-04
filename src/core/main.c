#define _XOPEN_SOURCE 700
#include <nsr/telemetry.h>
#include <nsr/ui/tui.h>
#include <nsr/state/topology.h>
#include <nsr/plugin/plugin.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <pwd.h>

#define NSR_MAX_TARGETS 8

typedef struct {
    nsr_shm_ring_t *l2g;
    nsr_shm_ring_t *g2l;
    nsr_shm_ring_large_t *l2t;
    pid_t gk_pid;
    pid_t logic_pid;
    char target_ip[64];
    char target_host[128];
    bool active;
} nsr_session_t;

static nsr_session_t g_sessions[NSR_MAX_TARGETS];
static int g_num_sessions = 0;
static bool g_running = true;

static nsr_plugin_manager_t g_plugin_mgr;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = false;
}

static const char *nsr_config_path(void)
{
    static char path[256];
    const char *home = getenv("HOME");

    if (!home || !home[0]) {
        if (getuid() == 0) {
            const char *sudo_user = getenv("SUDO_USER");
            if (sudo_user && sudo_user[0]) {
                struct passwd *pw = getpwnam(sudo_user);
                if (pw && pw->pw_dir && pw->pw_dir[0])
                    home = pw->pw_dir;
            }
        }
        if (!home || !home[0])
            home = "/tmp";
    }

    snprintf(path, sizeof(path), "%s/.nsrconfig", home);
    return path;
}

static void nsr_plugins_load_from_home(const char *home)
{
    if (!home || !home[0])
        return;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.nsr/plugins", home);
    g_plugin_mgr.vt->load_dir(&g_plugin_mgr, dir);
}

static void nsr_plugins_load_user_dir(void)
{
    const char *home = getenv("HOME");
    nsr_plugins_load_from_home(home);

    if (getuid() == 0) {
        const char *sudo_user = getenv("SUDO_USER");
        if (sudo_user && sudo_user[0]) {
            struct passwd *pw = getpwnam(sudo_user);
            if (pw && pw->pw_dir && pw->pw_dir[0])
                nsr_plugins_load_from_home(pw->pw_dir);
        }
    }
}

static void spawn_gatekeeper(nsr_session_t *sess)
{
    sess->gk_pid = fork();
    if (sess->gk_pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0)
            perror("affinity");
        nsr_gatekeeper_run(sess->l2g, sess->g2l, sess->target_ip);
        exit(0);
    }
}

static bool resolve_target(const char *arg, int family, char *out_ip, size_t out_len)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_RAW;

    if (getaddrinfo(arg, nullptr, &hints, &res) != 0)
        return false;

    if (res->ai_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, out_ip, (socklen_t)out_len);
    } else if (res->ai_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, out_ip, (socklen_t)out_len);
    } else {
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);
    return true;
}

int main(int argc, char **argv)
{
    uint32_t interval_ms = 100;
    bool silent = false;
    int address_family = AF_UNSPEC;

    static struct option long_options[] = {
        {"interval", required_argument, 0, 'i'},
        {"silent",   no_argument,       0, 's'},
        {"ipv4",     no_argument,       0, '4'},
        {"ipv6",     no_argument,       0, '6'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:s46h", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'i':
            interval_ms = atoi(optarg);
            if (interval_ms < 10) {
                fprintf(stderr, "Notice: Adjusted interval from %u ms to 10 ms to properly diagnose.\n", interval_ms);
                interval_ms = 10;
            } else if (interval_ms > 1000) {
                fprintf(stderr, "Notice: Adjusted interval from %u ms to 1000 ms to properly diagnose.\n", interval_ms);
                interval_ms = 1000;
            }
            break;
        case 's':
            silent = true;
            break;
        case '4':
            address_family = AF_INET;
            break;
        case '6':
            address_family = AF_INET6;
            break;
        case 'h':
        default:
            fprintf(stderr, "Usage: %s <target> [<target> ...] [-i ms] [-4|-6] [--silent]\n", argv[0]);
            fprintf(stderr, "Options:\n");
            fprintf(stderr, "  -i, --interval MS   Transmission interval in milliseconds (default: 100)\n");
            fprintf(stderr, "  -4, --ipv4          Resolve hostnames to IPv4 only\n");
            fprintf(stderr, "  -6, --ipv6          Resolve hostnames to IPv6 only\n");
            fprintf(stderr, "  -s, --silent        Run without TUI\n");
            return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: At least one target hostname or IP is required.\n");
        return EXIT_FAILURE;
    }

    g_plugin_mgr.vt = &nsr_plugin_manager_vtable;
    g_plugin_mgr.vt->init(&g_plugin_mgr, nsr_config_path());
    nsr_plugins_load_user_dir();

    for (int i = optind; i < argc && g_num_sessions < NSR_MAX_TARGETS; i++) {
        char ip[64];
        if (!resolve_target(argv[i], address_family, ip, sizeof(ip))) {
            fprintf(stderr, "Could not resolve hostname: %s\n", argv[i]);
            continue;
        }

        nsr_session_t *sess = &g_sessions[g_num_sessions];
        memset(sess, 0, sizeof(*sess));
        strncpy(sess->target_ip, ip, sizeof(sess->target_ip) - 1);
        strncpy(sess->target_host, argv[i], sizeof(sess->target_host) - 1);
        sess->active = true;

        size_t shm_size = sizeof(nsr_shm_ring_t);
        sess->l2g = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        sess->g2l = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        size_t shm_large_size = sizeof(nsr_shm_ring_large_t);
        sess->l2t = mmap(nullptr, shm_large_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);

        if (sess->l2g == MAP_FAILED || sess->g2l == MAP_FAILED || sess->l2t == MAP_FAILED) {
            perror("mmap");
            return EXIT_FAILURE;
        }

        memset(sess->l2g, 0, shm_size);
        memset(sess->g2l, 0, shm_size);
        memset(sess->l2t, 0, shm_large_size);

        spawn_gatekeeper(sess);

        sess->logic_pid = fork();
        if (sess->logic_pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            nsr_config_t *config = mmap(nullptr, sizeof(nsr_config_t), PROT_READ | PROT_WRITE,
                                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            atomic_init(&config->interval_ms, interval_ms);
            strncpy(config->target_ip, sess->target_ip, sizeof(config->target_ip) - 1);
            strncpy(config->target_host, sess->target_host, sizeof(config->target_host) - 1);
            nsr_logic_run(sess->g2l, sess->l2g, sess->l2t, config);
            exit(0);
        }

        g_num_sessions++;
    }

    if (g_num_sessions == 0) {
        fprintf(stderr, "Error: No valid targets.\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, SIG_IGN);

    nsr_topology_manager_t topo_mgr;
    memset(&topo_mgr, 0, sizeof(topo_mgr));
    topo_mgr.vt = &nsr_topology_manager_vtable;
    topo_mgr.vt->init(&topo_mgr);

    nsr_tui_driver_t tui_driver;
    memset(&tui_driver, 0, sizeof(tui_driver));
    tui_driver.state.current_mode = NSR_UI_NORMAL;

    if (!silent) {
        tui_driver.vt = &nsr_tui_driver_vtable;
        tui_driver.vt->init(&tui_driver);
        tui_driver.state.keys.vt = &nsr_key_manager_vtable;
        tui_driver.state.keys.vt->init(&tui_driver.state.keys, &g_plugin_mgr);
    }

    nsr_telemetry_state_t last_state;
    memset(&last_state, 0, sizeof(last_state));

    while (g_running) {
        int status;
        pid_t exited_pid = waitpid(-1, &status, WNOHANG);
        if (exited_pid > 0) {
            for (int s = 0; s < g_num_sessions; s++) {
                nsr_session_t *sess = &g_sessions[s];
                if (exited_pid == sess->gk_pid) {
                    if (WIFEXITED(status)) {
                        if (!silent)
                            fprintf(stderr, "[GATEKEEPER] Process exited; disabling target=%s status=%d\n",
                                    sess->target_ip, WEXITSTATUS(status));
                        sess->active = false;
                        if (sess->logic_pid > 0)
                            kill(sess->logic_pid, SIGTERM);
                    } else {
                        if (!silent)
                            fprintf(stderr, "[GATEKEEPER] Process crashed! target=%s status=%d\n",
                                    sess->target_ip, status);
                        spawn_gatekeeper(sess);
                    }
                } else if (exited_pid == sess->logic_pid) {
                    if (WIFEXITED(status) || !sess->active) {
                        if (!silent && sess->active)
                            fprintf(stderr, "[LOGIC] Process exited; disabling target=%s status=%d\n",
                                    sess->target_ip, WIFEXITED(status) ? WEXITSTATUS(status) : status);
                        sess->active = false;
                    } else {
                        if (!silent)
                            fprintf(stderr, "[LOGIC] Process crashed! target=%s status=%d\n",
                                    sess->target_ip, status);
                        sess->logic_pid = fork();
                        if (sess->logic_pid == 0) {
                            signal(SIGINT, SIG_DFL);
                            signal(SIGTERM, SIG_DFL);
                            nsr_config_t *config = mmap(nullptr, sizeof(nsr_config_t), PROT_READ | PROT_WRITE,
                                                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
                            atomic_init(&config->interval_ms, interval_ms);
                            strncpy(config->target_ip, sess->target_ip, sizeof(config->target_ip) - 1);
                            strncpy(config->target_host, sess->target_host, sizeof(config->target_host) - 1);
                            nsr_logic_run(sess->g2l, sess->l2g, sess->l2t, config);
                            exit(0);
                        }
                    }
                }
            }
        }

        bool any_active = false;
        for (int s = 0; s < g_num_sessions; s++) {
            nsr_session_t *sess = &g_sessions[s];
            if (!sess->active)
                continue;
            any_active = true;
            nsr_telemetry_state_t new_state;
            while (nsr_shm_ring_large_pop(sess->l2t, &new_state, sizeof(new_state))) {
                memcpy(&last_state, &new_state, sizeof(last_state));
                nsr_topology_update_from_telemetry(&topo_mgr.state, &last_state);
            }
        }
        if (!any_active)
            g_running = false;

        g_plugin_mgr.vt->update_telemetry(&g_plugin_mgr, &last_state, &topo_mgr.state);

        if (!silent) {
            tui_driver.vt->render(&tui_driver, &last_state, &topo_mgr.state, &g_plugin_mgr);
            int cmd = tui_driver.vt->update(&tui_driver, &topo_mgr.state, &g_plugin_mgr);
            if (cmd == 1)
                break;
            if (cmd == 2) {
                for (int s = 0; s < g_num_sessions; s++) {
                    if (g_sessions[s].logic_pid > 0)
                        kill(g_sessions[s].logic_pid, SIGUSR1);
                }
            }
            if (cmd == 4) {
                if (interval_ms < 1000)
                    interval_ms += 10;
            }
            if (cmd == 5) {
                if (interval_ms > 10) {
                    interval_ms -= 10;
                } else if (interval_ms > 0) {
                    interval_ms = 10;
                }
            }
            if (cmd == 6) {
                tui_driver.vt->toggle_dashboard(&tui_driver);
            }
        }

        struct timespec ts = {0, 16666666};
        nanosleep(&ts, nullptr);
    }

    if (!silent)
        tui_driver.vt->cleanup(&tui_driver);

    for (int s = 0; s < g_num_sessions; s++) {
        nsr_session_t *sess = &g_sessions[s];
        if (sess->gk_pid > 0)
            kill(sess->gk_pid, SIGTERM);
        if (sess->logic_pid > 0)
            kill(sess->logic_pid, SIGTERM);
    }
    for (int s = 0; s < g_num_sessions; s++) {
        nsr_session_t *sess = &g_sessions[s];
        if (sess->gk_pid > 0)
            waitpid(sess->gk_pid, nullptr, 0);
        if (sess->logic_pid > 0)
            waitpid(sess->logic_pid, nullptr, 0);
    }

    g_plugin_mgr.vt->cleanup(&g_plugin_mgr);

    return 0;
}
