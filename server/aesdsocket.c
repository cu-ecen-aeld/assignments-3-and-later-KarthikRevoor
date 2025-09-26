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

#define PORT 9000
#define DATAFILE "/var/tmp/aesdsocketdata"

static volatile sig_atomic_t exit_requested = 0;
static int sockfd_global = -1;   // global socket descriptor

void handle_signal(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        exit_requested = 1;
        if (sockfd_global != -1) {
            close(sockfd_global);  // unblock accept()
            sockfd_global = -1;
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

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "bind failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, 5) < 0) {
        syslog(LOG_ERR, "listen failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    return sockfd;
}

void handle_client(int clientfd, struct sockaddr_in *client_addr)
{
    char buffer[1024];
    char *packet = NULL;
    size_t packet_size = 0;
    int bytes;

    FILE *fp = fopen(DATAFILE, "a+"); // append and allow read
    if (!fp) {
        syslog(LOG_ERR, "fopen failed: %s", strerror(errno));
        return;
    }

    syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(client_addr->sin_addr));

    while ((bytes = recv(clientfd, buffer, sizeof(buffer), 0)) > 0) {
        for (int i = 0; i < bytes; i++) {
            char c = buffer[i];
            char *new_packet = realloc(packet, packet_size + 1);
            if (!new_packet) {
                syslog(LOG_ERR, "realloc failed");
                free(packet);
                fclose(fp);
                return;
            }
            packet = new_packet;
            packet[packet_size++] = c;

            if (c == '\n') {
                // Write full packet
                fwrite(packet, 1, packet_size, fp);
                fflush(fp);

                // Send full file back
                fseek(fp, 0, SEEK_SET);
                int n;
                while ((n = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
                    send(clientfd, buffer, n, 0);
                }
                fseek(fp, 0, SEEK_END);

                free(packet);
                packet = NULL;
                packet_size = 0;
            }
        }
    }

    free(packet);
    fclose(fp);
    syslog(LOG_INFO, "Closed connection from %s", inet_ntoa(client_addr->sin_addr));
}

void daemonize()
{
    pid_t pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        // parent exits
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    chdir("/");

    // redirect stdio to /dev/null
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
}

int main(int argc, char *argv[])
{
    int daemon_mode = 0;
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // clear file at program start
    FILE *fp = fopen(DATAFILE, "w");
    if (fp) fclose(fp);

    int sockfd = create_server_socket();
    if (sockfd < 0) return -1;
    sockfd_global = sockfd;

    if (daemon_mode) {
        daemonize();
    }

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int clientfd = accept(sockfd, (struct sockaddr *)&client_addr, &client_len);
        if (clientfd < 0) {
            if (exit_requested) break;
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        handle_client(clientfd, &client_addr);
        close(clientfd);
    }

    if (sockfd_global != -1) {
        close(sockfd_global);
    }

    remove(DATAFILE);
    closelog();

    return 0;
}

