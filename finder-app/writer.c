#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    openlog("writer", LOG_PID, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Invalid arguments: expected <file> <string>");
        closelog();
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr  = argv[2];

    syslog(LOG_DEBUG, "Writing \"%s\" to %s", writestr, writefile);

    int fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        syslog(LOG_ERR, "Error %s: %s is not opening", writefile, strerror(errno));
        closelog();
        return 1;
    }

    ssize_t written = write(fd, writestr, strlen(writestr));
    if (written < 0) {
        syslog(LOG_ERR, "Error writing to %s: %s", writefile, strerror(errno));
        close(fd);
        closelog();
        return 1;
    }

    if (close(fd) < 0) {
        syslog(LOG_ERR, "Error:  %s: %s is not closing", writefile, strerror(errno));
        closelog();
        return 1;
    }

    closelog();
    return 0;
}

