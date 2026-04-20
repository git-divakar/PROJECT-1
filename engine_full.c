#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>

#define MAX_CONTAINERS 32
#define LOG_BUFFER_SIZE 1024
#define LOG_LINE_SIZE 256
#define SOCKET_PATH "/tmp/engine.sock"

struct log_entry {
    char container_id[32];
    char line[LOG_LINE_SIZE];
};

struct container {
    char id[32];
    pid_t pid;
    int state; // 0=starting,1=running,2=stopped
    char rootfs[128];
    int stdout_fd;
    int stderr_fd;
    FILE *log_file;
    int stop_requested;
};

struct container containers[MAX_CONTAINERS];
int container_count = 0;
pthread_mutex_t container_mutex = PTHREAD_MUTEX_INITIALIZER;

// Bounded buffer for logging
struct log_entry log_buffer[LOG_BUFFER_SIZE];
int buf_start = 0, buf_end = 0;
pthread_mutex_t buf_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buf_not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t buf_not_full = PTHREAD_COND_INITIALIZER;
int shutdown_logging = 0;

// Logging functions
void log_produce(const char *id, const char *line) {
    pthread_mutex_lock(&buf_mutex);
    while (((buf_end + 1) % LOG_BUFFER_SIZE) == buf_start) {
        pthread_cond_wait(&buf_not_full, &buf_mutex);
    }
    strncpy(log_buffer[buf_end].container_id, id, 31);
    strncpy(log_buffer[buf_end].line, line, LOG_LINE_SIZE - 1);
    buf_end = (buf_end + 1) % LOG_BUFFER_SIZE;
    pthread_cond_signal(&buf_not_empty);
    pthread_mutex_unlock(&buf_mutex);
}

void *log_consumer(void *arg) {
    while (1) {
        pthread_mutex_lock(&buf_mutex);
        while (buf_start == buf_end && !shutdown_logging) {
            pthread_cond_wait(&buf_not_empty, &buf_mutex);
        }
        if (buf_start == buf_end && shutdown_logging) {
            pthread_mutex_unlock(&buf_mutex);
            break;
        }
        struct log_entry entry = log_buffer[buf_start];
        buf_start = (buf_start + 1) % LOG_BUFFER_SIZE;
        pthread_cond_signal(&buf_not_full);
        pthread_mutex_unlock(&buf_mutex);

        pthread_mutex_lock(&container_mutex);
        for (int i = 0; i < container_count; i++) {
            if (strcmp(containers[i].id, entry.container_id) == 0) {
                fprintf(containers[i].log_file, "%s", entry.line);
                fflush(containers[i].log_file);
                break;
            }
        }
        pthread_mutex_unlock(&container_mutex);
    }
    return NULL;
}

void *pipe_reader(void *arg) {
    struct container *c = (struct container *)arg;
    char buffer[LOG_LINE_SIZE];
    ssize_t n;

    while ((n = read(c->stdout_fd, buffer, LOG_LINE_SIZE - 1)) > 0) {
        buffer[n] = '\0';
        log_produce(c->id, buffer);
    }

    while ((n = read(c->stderr_fd, buffer, LOG_LINE_SIZE - 1)) > 0) {
        buffer[n] = '\0';
        log_produce(c->id, buffer);
    }

    return NULL;
}

void sigchld_handler(int sig) {
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&container_mutex);
        for (int i = 0; i < container_count; i++) {
            if (containers[i].pid == pid) {
                containers[i].state = 2;
                break;
            }
        }
        pthread_mutex_unlock(&container_mutex);
    }
}

// Helper to print container status
void print_ps(int client_fd) {
    pthread_mutex_lock(&container_mutex);
    char buf[1024];
    snprintf(buf, sizeof(buf), "ID\tPID\tSTATE\tROOTFS\n");
    write(client_fd, buf, strlen(buf));
    for (int i = 0; i < container_count; i++) {
        snprintf(buf, sizeof(buf), "%s\t%d\t%d\t%s\n",
                 containers[i].id,
                 containers[i].pid,
                 containers[i].state,
                 containers[i].rootfs);
        write(client_fd, buf, strlen(buf));
    }
    pthread_mutex_unlock(&container_mutex);
}

// Launch container
void launch_container(const char *id, const char *rootfs, const char *cmd) {
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        perror("pipe");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);

        unshare(CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS);
        if (chroot(rootfs) != 0) { perror("chroot"); exit(1); }
        chdir("/");
        execlp(cmd, cmd, NULL);
        perror("execlp"); exit(1);
    } else if (pid > 0) {
        pthread_mutex_lock(&container_mutex);
        struct container *c = &containers[container_count];
        strncpy(c->id, id, 31);
        strncpy(c->rootfs, rootfs, 127);
        c->pid = pid;
        c->state = 1;
        c->stdout_fd = stdout_pipe[0];
        c->stderr_fd = stderr_pipe[0];
        c->stop_requested = 0;

        char log_file[128];
        snprintf(log_file, 128, "%s.log", id);
        c->log_file = fopen(log_file, "w");
        if (!c->log_file) perror("fopen");

        container_count++;
        pthread_mutex_unlock(&container_mutex);

        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        pthread_t t;
        pthread_create(&t, NULL, pipe_reader, c);
        pthread_detach(t);
    } else {
        perror("fork");
    }
}

// Handle IPC client commands
void handle_client(int client_fd) {
    char buf[256];
    int n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    char cmd[32], arg1[64], arg2[64], arg3[64];
    sscanf(buf, "%s %s %s %s", cmd, arg1, arg2, arg3);

    if (strcmp(cmd, "ps") == 0) {
        print_ps(client_fd);
    } else if (strcmp(cmd, "start") == 0) {
        launch_container(arg1, arg2, arg3);
        char reply[64];
        snprintf(reply, sizeof(reply), "Started %s\n", arg1);
        write(client_fd, reply, strlen(reply));
    } else {
        write(client_fd, "Unknown command\n", 16);
    }
}

// Supervisor IPC server
void *ipc_server(void *arg) {
    int server_fd, client_fd;
    struct sockaddr_un addr;
    unlink(SOCKET_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            handle_client(client_fd);
            close(client_fd);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s supervisor\n", argv[0]);
        exit(1);
    }

    signal(SIGCHLD, sigchld_handler);

    pthread_t log_thread, ipc_thread;
    pthread_create(&log_thread, NULL, log_consumer, NULL);
    pthread_create(&ipc_thread, NULL, ipc_server, NULL);

    printf("Supervisor running...\n");
    while (1) pause();

    shutdown_logging = 1;
    pthread_cond_signal(&buf_not_empty);
    pthread_join(log_thread, NULL);

    pthread_mutex_lock(&container_mutex);
    for (int i = 0; i < container_count; i++) fclose(containers[i].log
