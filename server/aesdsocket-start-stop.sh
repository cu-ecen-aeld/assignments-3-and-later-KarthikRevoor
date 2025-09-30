#!/bin/sh

COMMAND=$1
DAEMON_NAME="aesdsocket"
DAEMON_PATH="/usr/bin/aesdsocket"

if [ -z "$COMMAND" ]; then
    echo "Usage: $0 start|stop"
    exit 1
fi

case "$COMMAND" in
    start)
        echo "Starting ${DAEMON_NAME}..."
        if start-stop-daemon -S -n ${DAEMON_NAME} -a ${DAEMON_PATH} -- -d; then
            echo "${DAEMON_NAME} started successfully."
        else
            echo "process already running."
            exit 1
        fi
        ;;

    stop)
        echo "Stopping ${DAEMON_NAME}..."
        if start-stop-daemon -K -n ${DAEMON_NAME} --signal TERM; then
            echo "${DAEMON_NAME} stopped successfully."
        else
            echo "Failed to stop ${DAEMON_NAME}."
            exit 1
        fi
        ;;

    *)
        echo "Invalid command. Usage: $0 start|stop"
        exit 1
        ;;
esac

exit 0

