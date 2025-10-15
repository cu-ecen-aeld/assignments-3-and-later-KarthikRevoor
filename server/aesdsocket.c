#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>     

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#define PORT        9000
#if USE_AESD_CHAR_DEVICE
#define DATAFILE    "/dev/aesdchar"
#else
#define DATAFILE    "/var/tmp/aesdsocketdata"
#endif
#define BACKLOG     10
#define BUFSIZE     1024
#define TS_INTERVAL 10

static volatile sig_atomic_t exit_requested = 0;
static int sockfd_global = -1;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
#if !USE_AESD_CHAR_DEVICE
static pthread_t timestamp_thread;
#endif

struct client_thread {
    pthread_t tid;
    int clientfd;
    struct sockaddr_in client_addr;
    SLIST_ENTRY(client_thread) entries;
};

SLIST_HEAD(thread_list, client_thread) thread_head = SLIST_HEAD_INITIALIZER(thread_head);

void handle_signal(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        exit_requested = 1;
        if (sockfd_global != -1) {
            shutdown(sockfd_global, SHUT_RDWR);
        }
    }
}

int create_server_socket()
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        syslog(LOG_ERR, "socket failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, BACKLOG) < 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    return sockfd;
}

void *client_handler(void *arg)
{
    struct client_thread *info = (struct client_thread *)arg;
    int clientfd = info->clientfd;
    char buf[BUFSIZE];
    ssize_t bytes;

    syslog(LOG_INFO, "Accepted connection from %s",
           inet_ntoa(info->client_addr.sin_addr));

    while (!exit_requested) {
        bytes = recv(clientfd, buf, sizeof(buf), 0);
        if (bytes <= 0) break;

        pthread_mutex_lock(&file_mutex);
        int fd = open(DATAFILE, O_RDWR | O_APPEND);
        if (fd < 0) {
            syslog(LOG_ERR, "open failed: %s", strerror(errno));
            pthread_mutex_unlock(&file_mutex);
            break;
        }

        write(fd, buf, bytes);

        if (memchr(buf, '\n', bytes)) {
            lseek(fd, 0, SEEK_SET);
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0) {
                send(clientfd, buf, n, 0);
            }
        }

        close(fd);
        pthread_mutex_unlock(&file_mutex);
    }

    shutdown(clientfd, SHUT_RDWR);
    close(clientfd);

    syslog(LOG_INFO, "Closed connection from %s",
           inet_ntoa(info->client_addr.sin_addr));

    pthread_mutex_lock(&file_mutex);
    SLIST_REMOVE(&thread_head, info, client_thread, entries);
    pthread_mutex_unlock(&file_mutex);

    free(info);
    return NULL;
}

#if !USE_AESD_CHAR_DEVICE
void *timestamp_func(void *arg)
{
    while (!exit_requested) {
        sleep(TS_INTERVAL);
        if (exit_requested) break;

        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        if (!tm_info) continue;

        char ts[128];
        strftime(ts, sizeof(ts), "timestamp:%a, %d %b %Y %T %z\n", tm_info);

        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(DATAFILE, "a");
        if (fp) {
            fputs(ts, fp);
            fflush(fp);
            fsync(fileno(fp));
            fclose(fp);
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}
#endif

void shutdown_all()
{
    if (sockfd_global != -1) {
        close(sockfd_global);
        sockfd_global = -1;
    }

    while (!SLIST_EMPTY(&thread_head)) {
        struct client_thread *node = SLIST_FIRST(&thread_head);
        SLIST_REMOVE_HEAD(&thread_head, entries);
        pthread_join(node->tid, NULL);
        free(node);
    }
}

int main(int argc, char *argv[])
{
    int daemon_mode = (argc == 2 && strcmp(argv[1], "-d") == 0);

    openlog("aesdsocket", LOG_PID, LOG_USER);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

#if !USE_AESD_CHAR_DEVICE
    FILE *fp = fopen(DATAFILE, "w");
    if (fp) fclose(fp);
#endif

    sockfd_global = create_server_socket();
    if (sockfd_global < 0) return EXIT_FAILURE;

    if (daemon_mode) {
        if (fork() > 0) exit(0);
        setsid();
        if (fork() > 0) exit(0);
        chdir("/");
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }

#if !USE_AESD_CHAR_DEVICE
    if (pthread_create(&timestamp_thread, NULL, timestamp_func, NULL) != 0) {
        syslog(LOG_ERR, "Failed to create timestamp thread");
        exit(EXIT_FAILURE);
    }
#endif

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int clientfd = accept(sockfd_global, (struct sockaddr *)&client_addr, &len);
        if (clientfd < 0) {
            if (exit_requested) break;
            continue;
        }

        struct client_thread *node = malloc(sizeof(*node));
        if (!node) {
            syslog(LOG_ERR, "malloc failed");
            close(clientfd);
            continue;
        }
        node->clientfd = clientfd;
        node->client_addr = client_addr;

        SLIST_INSERT_HEAD(&thread_head, node, entries);
        if (pthread_create(&node->tid, NULL, client_handler, node) != 0) {
            syslog(LOG_ERR, "pthread_create failed");
            SLIST_REMOVE(&thread_head, node, client_thread, entries);
            free(node);
            close(clientfd);
        }
    }

    shutdown_all();

#if !USE_AESD_CHAR_DEVICE
    pthread_join(timestamp_thread, NULL);
    remove(DATAFILE);
#endif

    closelog();
    return 0;
}

