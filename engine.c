#define _GNU_SOURCE
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 50
#define SOCK_PATH "/tmp/engine.sock"

/* ---------------- STATES ---------------- */
#define STATE_STARTING 0
#define STATE_RUNNING  1
#define STATE_STOPPED  2

/* ---------------- Container ---------------- */

typedef struct {
    char id[32];
    pid_t pid;

    int state;
    time_t start_time;
    int exit_status;

    char rootfs[128];
    char log_path[128];

} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- Find ---------------- */

container_t* find_container_by_pid(pid_t pid) {
    for (int i = 0; i < container_count; i++)
        if (containers[i].pid == pid)
            return &containers[i];
    return NULL;
}

container_t* find_container(char *id) {
    for (int i = 0; i < container_count; i++)
        if (strcmp(containers[i].id, id) == 0)
            return &containers[i];
    return NULL;
}

/* ---------------- Container Process ---------------- */

int container_main(void *arg) {

    char **args = (char **)arg;
    char *rootfs = args[0];
    char *cmd = args[1];
    char *log = args[2];

    sethostname("container", 10);

    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    if (chroot(rootfs) != 0) {
        perror("chroot");
        exit(1);
    }

    chdir("/");

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    /* logging */
    freopen(log, "w", stdout);
    freopen(log, "w", stderr);

    char *argv_exec[] = {cmd, NULL};
    execvp(cmd, argv_exec);

    perror("exec failed");
    return 1;
}

/* ---------------- Copy rootfs ---------------- */

void create_rootfs(char *id, char *dest) {
    snprintf(dest, 128, "./rootfs-%s", id);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cp -r rootfs-base %s", dest);
    system(cmd);
}

/* ---------------- Start ---------------- */

void start_container(char *id, char *cmd_exec) {

    pthread_mutex_lock(&lock);

    if (container_count >= MAX_CONTAINERS) {
        pthread_mutex_unlock(&lock);
        return;
    }

    container_t *c = &containers[container_count];

    strcpy(c->id, id);
    c->state = STATE_STARTING;
    c->start_time = time(NULL);

    create_rootfs(id, c->rootfs);

    snprintf(c->log_path, sizeof(c->log_path), "./%s.log", id);

    void *stack = malloc(STACK_SIZE);

    char *args[] = {c->rootfs, cmd_exec, c->log_path};

    pid_t pid = clone(container_main,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD,
                      args);

    if (pid < 0) {
        perror("clone");
        pthread_mutex_unlock(&lock);
        return;
    }

    c->pid = pid;
    c->state = STATE_RUNNING;

    container_count++;

    printf("[started] %s pid=%d\n", id, pid);

    pthread_mutex_unlock(&lock);
}

/* ---------------- Stop ---------------- */

void stop_container(char *id) {

    pthread_mutex_lock(&lock);

    container_t *c = find_container(id);
    if (!c) {
        pthread_mutex_unlock(&lock);
        return;
    }

    kill(c->pid, SIGKILL);
    c->state = STATE_STOPPED;

    printf("[stopped] %s\n", id);

    pthread_mutex_unlock(&lock);
}

/* ---------------- PS ---------------- */

void list_containers(char *out) {

    pthread_mutex_lock(&lock);

    int len = 0;

    len += sprintf(out + len, "ID\tPID\tSTATE\tSTART\n");

    for (int i = 0; i < container_count; i++) {

        char *state =
            containers[i].state == STATE_RUNNING ? "RUNNING" :
            containers[i].state == STATE_STOPPED ? "STOPPED" :
            "STARTING";

        len += sprintf(out + len, "%s\t%d\t%s\t%ld\n",
                       containers[i].id,
                       containers[i].pid,
                       state,
                       containers[i].start_time);
    }

    pthread_mutex_unlock(&lock);
}

/* ---------------- Reaper ---------------- */

void reap_children() {

    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {

        pthread_mutex_lock(&lock);

        container_t *c = find_container_by_pid(pid);
        if (c) {
            c->state = STATE_STOPPED;
            c->exit_status = status;

            printf("[reaped] %s pid=%d\n", c->id, pid);
        }

        pthread_mutex_unlock(&lock);
    }
}

/* ---------------- Supervisor ---------------- */

void supervisor_loop() {

    int server = socket(AF_UNIX, SOCK_STREAM, 0);

    unlink(SOCK_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    bind(server, (struct sockaddr*)&addr, sizeof(addr));
    listen(server, 10);

    printf("[supervisor running]\n");

    while (1) {

        reap_children();

        int client = accept(server, NULL, NULL);

        char buf[512];
        int n = read(client, buf, sizeof(buf) - 1);

        if (n <= 0) {
            close(client);
            continue;
        }

        buf[n] = '\0';

        char *cmd = strtok(buf, " ");
        char *id = strtok(NULL, " ");
        char *exec = strtok(NULL, "\n");

        if (!cmd) {
            write(client, "INVALID\n", 8);
            close(client);
            continue;
        }

        if (strcmp(cmd, "start") == 0 && id && exec) {
            start_container(id, exec);
            write(client, "OK\n", 3);
        }

        else if (strcmp(cmd, "ps") == 0) {
            char out[2048] = {0};
            list_containers(out);
            write(client, out, strlen(out));
        }

        else if (strcmp(cmd, "stop") == 0 && id) {
            stop_container(id);
            write(client, "STOPPED\n", 8);
        }

        else {
            write(client, "INVALID\n", 8);
        }

        close(client);
    }
}

/* ---------------- CLI ---------------- */

void send_msg(char *msg) {

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return;
    }

    write(sock, msg, strlen(msg));

    char buf[2048];
    int n = read(sock, buf, sizeof(buf) - 1);

    if (n > 0) {
        buf[n] = '\0';
        printf("%s\n", buf);
    }

    close(sock);
}

/* ---------------- MAIN ---------------- */

int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("usage:\n");
        printf("  supervisor\n");
        printf("  start <id> <cmd>\n");
        printf("  ps\n");
        printf("  stop <id>\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        supervisor_loop();
    }

    else if (strcmp(argv[1], "start") == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "start %s %s", argv[2], argv[3]);
        send_msg(msg);
    }

    else if (strcmp(argv[1], "ps") == 0) {
        send_msg("ps");
    }

    else if (strcmp(argv[1], "stop") == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "stop %s", argv[2]);
        send_msg(msg);
    }

    return 0;
}