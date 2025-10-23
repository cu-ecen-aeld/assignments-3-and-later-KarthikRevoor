/**
 * @file aesdsocket.c
 * @brief Multi-threaded socket server for AESD assignment 8
 *
 * Supports both character device mode (/dev/aesdchar)
 * and file mode (/var/tmp/aesdsocketdata) depending on
 * USE_AESD_CHAR_DEVICE define.
 */

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#define FILE_PATH "/dev/aesdchar"
#else
#define FILE_PATH "/var/tmp/aesdsocketdata"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <syslog.h>
#include <signal.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/queue.h>
#include "aesd_ioctl.h"

#define PORT             "9000"
#define BUFFER_SIZE      1024
#define TIMESTAMP_INTSEC 10

static int  g_server_socket = -1;
static bool g_exit_flag     = false;
pthread_mutex_t g_mutex     = PTHREAD_MUTEX_INITIALIZER;

typedef struct client_thread_s {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    SLIST_ENTRY(client_thread_s) entries;
} client_thread_t;

SLIST_HEAD(slisthead, client_thread_s) g_thread_list_head =
    SLIST_HEAD_INITIALIZER(g_thread_list_head);

void* client_thread_func(void* thread_param);
#if !USE_AESD_CHAR_DEVICE
void* timestamp_thread_func(void* arg);
#endif
void  cleanup_and_exit(int signum);
void  graceful_shutdown(void);
int   setup_server_socket(const char* port);
void  daemonize(void);

void cleanup_and_exit(int signum)
{
    syslog(LOG_INFO, "Caught signal %d, requesting exit", signum);
    g_exit_flag = true;
    if (g_server_socket != -1)
        shutdown(g_server_socket, SHUT_RDWR);
}

void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) exit(EXIT_FAILURE);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

int main(int argc, char *argv[])
{
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);

    int daemon_mode = (argc == 2 && strcmp(argv[1], "-d") == 0);
    if (daemon_mode)
        daemonize();

#if !USE_AESD_CHAR_DEVICE
    int fd = open(FILE_PATH, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        syslog(LOG_ERR, "Failed to open/create %s: %s", FILE_PATH, strerror(errno));
        return EXIT_FAILURE;
    }
    close(fd);

    pthread_t timer_thread;
    pthread_create(&timer_thread, NULL, timestamp_thread_func, NULL);
#endif

    g_server_socket = setup_server_socket(PORT);
    if (g_server_socket < 0) {
        syslog(LOG_ERR, "setup_server_socket failed");
        g_exit_flag = true;
    }

    while (!g_exit_flag) {
        struct sockaddr_in client_addr;
        socklen_t addr_size = sizeof(client_addr);
        int client_fd = accept(g_server_socket, (struct sockaddr*)&client_addr, &addr_size);
        if (client_fd < 0) {
            if (errno == EINTR && g_exit_flag) break;
            if (!g_exit_flag)
                syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        client_thread_t *new_node = malloc(sizeof(client_thread_t));
        if (!new_node) {
            syslog(LOG_ERR, "malloc failed for client_thread");
            close(client_fd);
            continue;
        }
        new_node->client_fd  = client_fd;
        new_node->client_addr = client_addr;

        SLIST_INSERT_HEAD(&g_thread_list_head, new_node, entries);
        int rc = pthread_create(&new_node->thread_id, NULL, client_thread_func, (void*)new_node);
        if (rc != 0) {
            syslog(LOG_ERR, "pthread_create failed: %s", strerror(rc));
            SLIST_REMOVE(&g_thread_list_head, new_node, client_thread_s, entries);
            free(new_node);
            close(client_fd);
        }
    }

    graceful_shutdown();

#if !USE_AESD_CHAR_DEVICE
    pthread_join(timer_thread, NULL);
    remove(FILE_PATH);
#endif

    pthread_mutex_destroy(&g_mutex);
    closelog();
    return 0;
}

void* client_thread_func(void* thread_param)
{
    client_thread_t *tinfo = (client_thread_t*)thread_param;
    char *ip_str = inet_ntoa(tinfo->client_addr.sin_addr);
    syslog(LOG_INFO, "Accepted connection from %s", ip_str);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    while (!g_exit_flag) {
        bytes_received = recv(tinfo->client_fd, buffer, BUFFER_SIZE, 0);
        if (bytes_received <= 0)
            break;

        pthread_mutex_lock(&g_mutex);
        int fd = open(FILE_PATH, O_RDWR | O_APPEND);
        if (fd < 0) {
            syslog(LOG_ERR, "Failed to open %s: %s", FILE_PATH, strerror(errno));
            pthread_mutex_unlock(&g_mutex);
            break;
        }
                /* Check for AESDCHAR_IOCSEEKTO:X,Y command */
        if (strncmp(buffer, "AESDCHAR_IOCSEEKTO:", 19) == 0)
        {
            struct aesd_seekto seekto;
            unsigned int write_cmd = 0, write_cmd_offset = 0;

            if (sscanf(buffer + 19, "%u,%u", &write_cmd, &write_cmd_offset) == 2)
            {
                seekto.write_cmd = write_cmd;
                seekto.write_cmd_offset = write_cmd_offset;

                if (ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto) == -1)
                {
                    syslog(LOG_ERR, "ioctl AESDCHAR_IOCSEEKTO failed: %s", strerror(errno));
                }
                else
                {
                    /* Read from same FD to preserve new seek offset */
                    char read_buf[BUFFER_SIZE];
                    ssize_t read_size;
                    lseek(fd, 0, SEEK_CUR); // ensure correct position

                    while ((read_size = read(fd, read_buf, sizeof(read_buf))) > 0)
                    {
                        if (send(tinfo->client_fd, read_buf, read_size, 0) == -1)
                        {
                            syslog(LOG_ERR, "Send failed: %s", strerror(errno));
                            break;
                        }
                    }
                }
            }
            else
            {
                syslog(LOG_ERR, "Malformed AESDCHAR_IOCSEEKTO command");
            }

            close(fd);
            pthread_mutex_unlock(&g_mutex);
            continue; /* Skip normal write path */
        }


        write(fd, buffer, bytes_received);
        fsync(fd);

        if (memchr(buffer, '\n', bytes_received)) {
            close(fd);
            fd = open(FILE_PATH, O_RDONLY);
            if (fd >= 0) {
                ssize_t n;
                while ((n = read(fd, buffer, BUFFER_SIZE)) > 0)
                    send(tinfo->client_fd, buffer, n, 0);
                close(fd);
            }
        } else {
            close(fd);
        }

        pthread_mutex_unlock(&g_mutex);
    }

    shutdown(tinfo->client_fd, SHUT_RDWR);
    close(tinfo->client_fd);
    syslog(LOG_INFO, "Closed connection from %s", ip_str);

    pthread_mutex_lock(&g_mutex);
    SLIST_REMOVE(&g_thread_list_head, tinfo, client_thread_s, entries);
    pthread_mutex_unlock(&g_mutex);

    free(tinfo);
    return NULL;
}

#if !USE_AESD_CHAR_DEVICE
void* timestamp_thread_func(void* arg)
{
    while (!g_exit_flag) {
        sleep(TIMESTAMP_INTSEC);
        if (g_exit_flag) break;

        time_t now = time(NULL);
        struct tm* tmp = localtime(&now);
        if (!tmp) continue;

        char timestr[128];
        strftime(timestr, sizeof(timestr), "timestamp:%a, %d %b %Y %T %z\n", tmp);

        pthread_mutex_lock(&g_mutex);
        FILE* file = fopen(FILE_PATH, "a");
        if (file) {
            fputs(timestr, file);
            fclose(file);
        }
        pthread_mutex_unlock(&g_mutex);
    }
    return NULL;
}
#endif

void graceful_shutdown(void)
{
    if (g_server_socket != -1) {
        close(g_server_socket);
        g_server_socket = -1;
    }

    client_thread_t *tnode;
    while (!SLIST_EMPTY(&g_thread_list_head)) {
        tnode = SLIST_FIRST(&g_thread_list_head);
        SLIST_REMOVE_HEAD(&g_thread_list_head, entries);
        pthread_join(tnode->thread_id, NULL);
        free(tnode);
    }
}

int setup_server_socket(const char* port)
{
    struct addrinfo hints, *servinfo, *p;
    int sockfd = -1, rc, yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    if ((rc = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
        syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(rc));
        return -1;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0)
            continue;

        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == 0)
            break;

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(servinfo);

    if (sockfd < 0) {
        syslog(LOG_ERR, "Could not bind socket");
        return -1;
    }

    if (listen(sockfd, SOMAXCONN) != 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    syslog(LOG_INFO, "Listening on port %s", port);
    return sockfd;
}

