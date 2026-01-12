#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdio.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/wait.h>
#include <linux/limits.h>

const char *OUTPUT_FILE = "/var/tmp/aesdsocketdata";
const char BIND_PORT[5] = "9000";
const size_t BUF_MAX_LEN = 1024 * 5; // 5kb, why not?

/* i really don't like using globals... */
static int conn_fd;
static int socket_fd;
static int output_fd;
static int sig_caught = 0;

/// @brief Shuts down the socket and flips a flag to allow ongoing work to finish before exiting.
/// @param signo
static void sig_handler(int signo)
{
    /*
     * prevents new sends/receives (connections?) but leaves the fd intact
     * so fd ops can be completed gracefully.
     */
    shutdown(socket_fd, SHUT_RD);
    syslog(LOG_DEBUG, "Caught signal, exiting");
    sig_caught = 1;
}

/// @brief binds socket_fd to port 9000 on any IPv4 interface available.
/// @return -1 on error, 0 on success
static int bind_socket()
{
    /*
     * open stream socket on any:9000, exit with -1 on error
     * got this from beej's guide.
     */
    struct addrinfo hints, *socket_ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, BIND_PORT, &hints, &socket_ai) == -1)
    {
        syslog(LOG_ERR, "getaddrinfo: %m");
        return -1;
    }

    /* open the socket */
    socket_fd = socket(socket_ai->ai_family, socket_ai->ai_socktype, socket_ai->ai_protocol);
    if (socket_fd == -1)
    {
        syslog(LOG_ERR, "socket: %m");
        return -1;
    }

    /*
     * set the SO_REUSEADDR option to prevent "address in use" errors on startup after a crash
     *
     * Source - https://stackoverflow.com/a
     * Posted by mpromonet, modified by community. See post 'Timeline' for change history
     * Retrieved 2026-01-09, License - CC BY-SA 3.0
     */
    int reuse = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse)) < 0)
        perror("setsockopt(SO_REUSEADDR) failed");

#ifdef SO_REUSEPORT
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, (const char *)&reuse, sizeof(reuse)) < 0)
        perror("setsockopt(SO_REUSEPORT) failed");
#endif

    /* bind address+port to the socket */
    if (bind(socket_fd, socket_ai->ai_addr, socket_ai->ai_addrlen) == -1)
    {
        syslog(LOG_ERR, "bind: %m");
        return -1;
    }
    // syslog(LOG_DEBUG, "Bound socket to port 9000");

    freeaddrinfo(socket_ai);

    return 0;
}

/// @brief Blocks until a client connection is established
/// @param client_ip String pointer with the client's IP address
/// @param client_ip_len
/// @return -1 on error, 0 on success
static int accept_connection(char *client_ip, size_t client_ip_len)
{
    struct sockaddr_storage client_addr;
    socklen_t addr_size = sizeof client_addr;

    /* note how this casts *the pointer* for client_addr from the specialized
    struct to the general struct. */
    conn_fd = accept(socket_fd, (struct sockaddr *)&client_addr, &addr_size);
    if (conn_fd == -1)
    {
        // invalid argument indicates the socket has been shutdown
        if (errno != EINVAL)
            syslog(LOG_ERR, "accept: %m");

        // this could trigger an infinite loop if the socket isn't opened properly
        return -1;
    }

    /* convert ip to string.. should work with hostnames, ipv6 and ipv4, but only using
     * ipv4 atm.
     *
     * tried this at first but client_ip ended up being garbage by the end of process_incoming:
     *      struct sockaddr_in *addr_in = (struct sockaddr_in *)&client_addr;
     *      client_ip = inet_ntoa(addr_in->sin_addr);
     *
     */
    client_ip[0] = '\0';
    int rc = getnameinfo((struct sockaddr *)&client_addr, addr_size,
                         client_ip, (socklen_t)client_ip_len,
                         NULL, 0, NI_NUMERICHOST);
    if (rc != 0)
        snprintf(client_ip, client_ip_len, "(unknown)");

    syslog(LOG_DEBUG, "Accepted connection from %s", client_ip);

    return 0;
}

/// @brief Receives data from the client in BUF_MAX_LEN chunks
/// @param buf
/// @return -1 on error, bytes received on success
static ssize_t recieve_data(char *buf)
{
    ssize_t len = 0;
    while (1)
    {
        len = recv(conn_fd, buf, BUF_MAX_LEN, MSG_DONTWAIT);

        /* retry on interruptions, otherwise give up */
        if (len == -1)
        {
            // if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            if (errno == EINTR)
            {
                syslog(LOG_DEBUG, "%m, retrying...");
                continue;
            }
        }

        break;
    }

    return len;
}

/// @brief Reads up to BUF_MAX_LEN bytes from the output file into buf
/// @param buf
/// @param off Starting position for file reads
/// @return -1 on error, bytes read otherwise
static ssize_t read_output(char *buf, off_t off)
{
    /* move to the offset position and read as much as the buffer will hold */
    if (lseek(output_fd, off, SEEK_SET) == -1)
    {
        syslog(LOG_ERR, "Error seeking to the beginning of %s: %m", OUTPUT_FILE);
        return -1;
    };

    ssize_t read_len = 0;
    ssize_t read_ret;
    while (read_len <= sizeof buf && (read_ret = read(output_fd, buf, BUF_MAX_LEN)) != 0)
    {
        if (read_ret == -1)
        {
            /* retry after an interrupt */
            if (errno == EINTR)
                return -1;

            /* otherwise print the error and bail out */
            syslog(LOG_ERR, "Error while reading %s: %m", OUTPUT_FILE);
            break;
        }
        read_len += read_ret;
    }

    return read_len;
}

/// @brief Sends a chunk of data from the output file to the client
/// @param buf
/// @param len
/// @return bytes sent
static size_t send_output(char *buf, size_t len)
{
    // keep sending until the whole buffer has been sent
    size_t sent_len = 0;
    while (sent_len < len)
    {
        size_t send_len = len - sent_len;
        if (send_len > BUF_MAX_LEN)
            send_len = BUF_MAX_LEN;

        int ret = send(conn_fd, buf + sent_len, send_len, MSG_MORE);
        if (ret == -1)
        {
            syslog(LOG_ERR, "Error during send, going to retry: %m");
            continue;
        }

        sent_len += ret;
    }

    return sent_len;
}

/// @brief Writes received data to the output file.
/// @param buf
/// @param len expected number of bytes to write
/// @return -1 on error, bytes written on success.
static ssize_t write_output(char *buf, size_t len)
{
    ssize_t write_len;
    if ((write_len = write(output_fd, buf, len)) == -1)
    {
        syslog(LOG_ERR, "Error during write to %s (%d): %m", OUTPUT_FILE, output_fd);
        return -1;
    }
    fsync(output_fd);

    return write_len;
}

/// @brief receives data in BUF_MAX_LEN increments and writes it to the output file
static void process_incoming()
{
    /* initialize client_ip as a static buffer otherwise it will be empty/garbage by the end */
    char client_ip[INET_ADDRSTRLEN] = {0};
    if (accept_connection(client_ip, sizeof client_ip) == -1)
        return;

    /* loop until all data has been received and written */
    while (1)
    {
        char recv_buf[BUF_MAX_LEN];
        // syslog(LOG_DEBUG, "Waiting to receive...");
        ssize_t recv_len = recieve_data(recv_buf);
        if (recv_len <= 0)
            break;
        syslog(LOG_DEBUG, "Received %zu bytes", recv_len);

        ssize_t write_len;
        if ((write_len = write_output(recv_buf, recv_len)) == -1)
            continue;
        syslog(LOG_DEBUG, "Wrote %zu bytes to %s", write_len, OUTPUT_FILE);
    }

    /* loop until all data in OUTPUT_FILE has been read and sent */
    off_t off = 0;
    while (1)
    {
        /*
         * read in chunks, updating offset as we go.
         */
        char read_buf[BUF_MAX_LEN];
        size_t read_len;
        if ((read_len = read_output(read_buf, off)) == -1)
        {
            syslog(LOG_ERR, "Error while reading from %s: %m", OUTPUT_FILE);
            continue;
        }

        if (read_len == 0)
            break;

        syslog(LOG_DEBUG, "Read %zu bytes from %s.", read_len, OUTPUT_FILE);

        size_t sent_len = send_output(read_buf, read_len);
        syslog(LOG_DEBUG, "Sent %zu bytes to client.", sent_len);

        off += read_len;
    }

    close(conn_fd);
    syslog(LOG_DEBUG, "Closed connection from %s", client_ip);

    close(output_fd);
    // syslog(LOG_DEBUG, "Closed %s", OUTPUT_FILE);
}

/// @brief Handles incoming connections until SIGINT or SIGTERM is caught.
/// @return -1 on error, 0 on success
static int handle_connections()
{
    /* register signal handler */
    if (signal(SIGINT, sig_handler) == SIG_ERR || signal(SIGTERM, sig_handler) == SIG_ERR)
    {
        syslog(LOG_ERR, "signal: %m");
        return -1;
    }

    /* start listening on socket_fd with max 100 queued connections */
    if (listen(socket_fd, 100) == -1)
    {
        syslog(LOG_ERR, "listen: %m");
        return -1;
    }
    syslog(LOG_DEBUG, "Listening for connections on port %s...", BIND_PORT);

    /* loop over incoming connections until SIGINT or SIGTERM are caught */
    while (sig_caught == 0)
    {
        /* open output file for reading and appending, create if necessary */
        output_fd = open(OUTPUT_FILE, O_RDWR | O_APPEND | O_CREAT, 0644);
        if (output_fd == -1)
        {
            syslog(LOG_ERR, "Unable to open %s: %m", OUTPUT_FILE);
            return -1;
        }

        /*
         * receive data from the client and append that data to the output file
         * then read the entire output file and respond with the result
         */
        process_incoming();
    }

    return 0;
}

int main(int argc, char *argv[])
{
    pid_t pid;
    int opt;
    int daemon_mode = 0;
    int exit_code = 0;

    /* getopt is slight overkill here, but it's good practice */
    while ((opt = getopt(argc, argv, "d")) != -1)
    {
        switch (opt)
        {
        case 'd':
            daemon_mode = 1;
            break;
        }
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    if (bind_socket() == -1)
        exit(EXIT_FAILURE);

    /* fork if needed */
    if (daemon_mode == 1)
    {
        pid = fork();

        if (pid > 0)
        {
            syslog(LOG_DEBUG, "Starting in daemon mode...");
            exit(EXIT_SUCCESS);
        }
        else if (!pid)
        {
            /* grab the absolute path to this binary using /proc/self/exe */
            const char *p = "/proc/self/exe";
            /* PATH_MAX has problems but it will suffice here
              see also: https://insanecoding.blogspot.com/2007/11/pathmax-simply-isnt.html) */
            char path[PATH_MAX + 1];
            const int rl_ret = readlink(p, path, PATH_MAX);
            if (rl_ret == -1)
            {
                syslog(LOG_ERR, "Readlink error: %m");
                exit(EXIT_FAILURE);
            }
            /* readlink does *not* null-terminate the buffer, so we need to do that here */
            path[rl_ret] = '\0';

            char *const args[] = {path, NULL};
            int ret;

            ret = execv(path, args);
            if (ret == -1)
            {
                syslog(LOG_ERR, "Exec error: %m");
                exit(EXIT_FAILURE);
            }
        }
        else if (pid == -1)
        {
            syslog(LOG_ERR, "Fork error: %m");
            exit(EXIT_FAILURE);
        }
    }

    if (daemon_mode == 0)
    {
        exit_code = handle_connections();

        close(socket_fd);
        syslog(LOG_DEBUG, "Socket closed");

        remove(OUTPUT_FILE);
        syslog(LOG_DEBUG, "%s removed", OUTPUT_FILE);
    }

    exit(0);
}
