#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int main(int argc, char *argv[])
{

    // set up syslog with the LOG_USER facility
    // LOG_PERROR ensures that log messages output to stderr as well as /var/log/syslog
    openlog("writer", LOG_PID | LOG_PERROR, LOG_USER);

    // accept user inputs: file path and string to write.
    const char *file = argv[1];
    const char *str = argv[2];

    // both inputs are mandatory
    if (sizeof(file) == 0 || sizeof(str) == 0 || file == NULL || str == NULL)
    {
        syslog(LOG_ERR, "%s", "Usage: writer <file> <write string>");
        return 1;
    }

    // open the file
    // assume that the destination directory already exists, but handle
    // errors when it does not.
    const int fd = creat(file, 0644);
    if (fd == -1)
    {
        syslog(LOG_ERR, "Unable to open %s: %m", file);
        return 1;
    }

    // log “Writing <string> to <file>” to LOG_DEBUG
    syslog(LOG_DEBUG, "Writing \"%s\" to %s", str, file);

    // write the string
    const size_t str_size = strlen(str);
    ssize_t nr = write(fd, str, str_size);
    if (nr == -1)
    {
        syslog(LOG_ERR, "Unable to write to %s: %m", file);
        return 1;
    }
    else if (nr < str_size)
    {
        // partial write, errno won't be set
        syslog(LOG_ERR, "An error occurred and %s was only partially written. Please try again.", file);
        return 1;
    }

    // should be good at this point
    fsync(fd);
    return 0;
}