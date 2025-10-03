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

#define PORT        9000
#define DATAFILE    "/var/tmp/aesdsocketdata"
#define BACKLOG     10
#define BUFSIZE     1024
#define TS_INTERVAL 10

static volatile sig_atomic_t exit_requested = 0;
static int sockfd_global = -1;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t timestamp_thread;

/* ---- Thread structure ---- */
struct client_thread {
    pthread_t tid;
    int clientfd;
    struct sockaddr_in client_addr;
    SLIST_ENTRY(client_thread) entries;
};

SLIST_HEAD(thread_list, client_thread) thread_head = SLIST_HEAD_INITIALIZER(thread_head);

/* ---- Signal handler ---- */
void handle_signal(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        exit_requested = 1;
        if (sockfd_global != -1) {
            shutdown(sockfd_global, SHUT_RDWR);   // unblock accept()
        }
    }
}

/* ---- Create server socket ---- */
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

/* ---- Client thread ---- */
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
        FILE *fp = fopen(DATAFILE, "a+");
        if (!fp) {
            syslog(LOG_ERR, "fopen failed: %s", strerror(errno));
            pthread_mutex_unlock(&file_mutex);
            break;
        }

        fwrite(buf, 1, bytes, fp);
        fflush(fp);
        fsync(fileno(fp));   //

        if (memchr(buf, '\n', bytes)) {
            rewind(fp);
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
                send(clientfd, buf, n, 0);
            }
        }
        fclose(fp);
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

/* ---- Timestamp thread ---- */
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

/* ---- Graceful shutdown ---- */
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

/* ---- Main ---- */
int main(int argc, char *argv[])
{
    int daemon_mode = (argc == 2 && strcmp(argv[1], "-d") == 0);

    openlog("aesdsocket", LOG_PID, LOG_USER);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    FILE *fp = fopen(DATAFILE, "w");
    if (fp) fclose(fp);

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

    if (pthread_create(&timestamp_thread, NULL, timestamp_func, NULL) != 0) {
        syslog(LOG_ERR, "Failed to create timestamp thread");
        exit(EXIT_FAILURE);
    }

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
    pthread_join(timestamp_thread, NULL);

    remove(DATAFILE);
    closelog();
    return 0;
}

