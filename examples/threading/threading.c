#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg, ...)
// #define DEBUG_LOG(msg, ...) printf("threading: " msg "\n", ##__VA_ARGS__)
#define ERROR_LOG(msg, ...) printf("threading ERROR: " msg "\n", ##__VA_ARGS__)

void *threadfunc(void *thread_param)
{
    struct thread_data *td = (struct thread_data *)thread_param;

    usleep(td->wait_to_obtain_ms);
    const int ret_lock = pthread_mutex_lock(td->mutex);
    if (ret_lock)
    {
        errno = ret_lock;
        perror("obtain lock");
        td->thread_complete_success = false;
    }

    usleep(td->wait_to_release_ms);
    const int ret_unlock = pthread_mutex_unlock(td->mutex);
    if (ret_unlock)
    {
        errno = ret_unlock;
        perror("release lock");
        td->thread_complete_success = false;
    }

    return td;
}

bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex, int wait_to_obtain_ms, int wait_to_release_ms)
{
    /**
     * TODO: allocate memory for thread_data, setup mutex and wait arguments, pass thread_data to created thread
     * using threadfunc() as entry point.
     *
     * return true if successful.
     *
     * See implementation details in threading.h file comment block
     */

    struct thread_data *td = malloc(sizeof *td);
    if (!td)
        return false;

    td->wait_to_obtain_ms = wait_to_obtain_ms;
    td->wait_to_release_ms = wait_to_release_ms;
    td->mutex = mutex;
    td->thread_complete_success = true;

    const int ret = pthread_create(thread, NULL, threadfunc, td);
    if (ret)
    {
        errno = ret;
        perror("start_thread_obtaining_mutex");
        return false;
    }

    return true;
}
