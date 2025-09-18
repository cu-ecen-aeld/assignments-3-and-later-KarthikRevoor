#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{

    // TODO: wait, obtain mutex, wait, release mutex as described by thread_data structure
    // hint: use a cast like the one below to obtain thread arguments from your parameter
    //struct thread_data* thread_func_args = (struct thread_data *) thread_param;
        if (thread_param == NULL) {
        return NULL;
    }

    struct thread_data* tdata = (struct thread_data*) thread_param;

    // wait before attempting to obtain the mutex
    usleep(tdata->wait_to_obtain_ms * 1000);

    if (pthread_mutex_lock(tdata->mutex) != 0) {
        ERROR_LOG("Failed to lock mutex\n");
        tdata->thread_complete_success = false;
        return tdata;
    }

    // once mutex is locked, wait again
    usleep(tdata->wait_to_release_ms * 1000);

    if (pthread_mutex_unlock(tdata->mutex) != 0) {
        ERROR_LOG("Failed to unlock mutex\n");
        tdata->thread_complete_success = false;
        return tdata;
    }

    // mark success
    tdata->thread_complete_success = true;

    return tdata;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    /**
     * TODO: allocate memory for thread_data, setup mutex and wait arguments, pass thread_data to created thread
     * using threadfunc() as entry point.
     *
     * return true if successful.
     *
     * See implementation details in threading.h file comment block
     */
        struct thread_data* tdata = malloc(sizeof(struct thread_data));
    if (tdata == NULL) {
        ERROR_LOG("Failed to allocate memory for thread_data\n");
        return false;
    }

    tdata->mutex = mutex;
    tdata->wait_to_obtain_ms = wait_to_obtain_ms;
    tdata->wait_to_release_ms = wait_to_release_ms;
    tdata->thread_complete_success = false;

    int rc = pthread_create(thread, NULL, threadfunc, tdata);
    if (rc != 0) {
        ERROR_LOG("pthread_create failed: %d\n", rc);
        free(tdata);
        return false;
    }

    return true;
}

