/*
 * proxy.c - Concurrent Web Proxy (Process- & Thread-based)
 * Supports:
 *   - Multi-process concurrency (default)
 *   - Multi-thread concurrency (-t or --thread)
 *   - HTTPS Tunneling (CONNECT method)
 *   - Synchronized logging (fcntl file-lock for processes, mutex for threads)
 *   - Logging of all response codes and CONNECT bytes transferred
 */

#include "csapp.h"
#include <getopt.h>
#include <fcntl.h>
#include <sys/file.h>

#define LOGFILE "proxy.log"
#define IDLE_TIMEOUT 60 // seconds for HTTPS tunnel timeout

/* Concurrency modes */
enum
{
    MODE_PROCESS = 0,
    MODE_THREAD
} mode = MODE_PROCESS;

// Thread logging mutex 
pthread_mutex_t log_mutex;

//Function defintions 
void usage(char *progname);
void run_proxy(int listenfd);
void *thread_handler(void *vargp);
void process_handler(int connfd, struct sockaddr_in clientaddr);
void handle_request(int connfd, struct sockaddr_in *clientaddr, rio_t *rio);
int forward_http(int clientfd, const char *hostname, int port, const char *path,
                 rio_t *rio, const char *method, char *uri, int *response_size);
void handle_connect(int clientfd, const char *hostname, int port,
                    struct sockaddr_in *clientaddr, rio_t *rio);
void relay(int clientfd, int serverfd, long *sent_bytes, long *recv_bytes);
void format_http_log(char *logstring, struct sockaddr_in *sockaddr,
                     const char *uri, int size);
void format_connect_log(char *logstring, struct sockaddr_in *sockaddr,
                        const char *hostname, int port,
                        long sent_bytes, long recv_bytes);
void parse_uri(const char *uri, char *hostname, int *port, char *path);
void clienterror(int fd, const char *errnum, const char *shortmsg, const char *longmsg);

//Main function to handle the user input
int main(int argc, char **argv)
{
    int listenfd, opt;
    short port;
    struct option long_opts[] = {
        {"thread", no_argument, NULL, 't'},
        {"process", no_argument, NULL, 'p'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}};

    //parse the option calls
    while ((opt = getopt_long(argc, argv, "tph", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
        case 't':
            mode = MODE_THREAD;
            break;
        case 'p':
            mode = MODE_PROCESS;
            break;
        case 'h':
            usage(argv[0]);
            break;
        default:
            usage(argv[0]);
        }
    }
    if (optind >= argc)
        usage(argv[0]);

    port = atoi(argv[optind]);
    if (port <= 0)
    {
        fprintf(stderr, "Invalid port number: %s\n", argv[optind]);
        exit(1);
    }

    /* Initialize logging mutex for threads */
    if (mode == MODE_THREAD)
    {
        if (pthread_mutex_init(&log_mutex, NULL) != 0)
        {
            perror("pthread_mutex_init");
            exit(1);
        }
    }

    /* Open listening socket */
    listenfd = Open_listenfd(port);
    printf("Proxy listening on port %d [%s mode]...\n",
           port, mode == MODE_THREAD ? "THREAD" : "PROCESS");

    run_proxy(listenfd);
    return 0;
}

/* Print usage and exit */
void usage(char *progname)
{
    fprintf(stderr,
            "Usage: %s [-t|--thread] [-p|--process] <port>\n"
            "    -t, --thread    Run in multi-threaded mode\n"
            "    -p, --process   Run in multi-process mode (default)\n"
            "    -h, --help      Show this help message\n",
            progname);
    exit(1);
}

/* Accept connections and dispatch to worker (process or thread) */
void run_proxy(int listenfd)
{
    int connfd;
    socklen_t clientlen;
    struct sockaddr_in clientaddr;

    while (1)
    {
        clientlen = sizeof(clientaddr);
        connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        if (connfd < 0)
        {
            perror("accept");
            continue;
        }

        if (mode == MODE_PROCESS)
        {
            if (fork() == 0)
            { // Child process
                Close(listenfd);
                process_handler(connfd, clientaddr);
                Close(connfd);
                exit(0);
            }
            Close(connfd); // Parent closes
        }
        else
        {
            /* Threaded mode */
            pthread_t tid;
            struct
            {
                int fd;
                struct sockaddr_in addr;
            } *arg = malloc(sizeof(*arg));
            if (!arg)
            {
                perror("malloc");
                Close(connfd);
                continue;
            }
            arg->fd = connfd;
            arg->addr = clientaddr;
            if (pthread_create(&tid, NULL, thread_handler, arg) != 0)
            {
                perror("pthread_create");
                free(arg);
                Close(connfd);
                continue;
            }
            pthread_detach(tid);
        }
    }
}

/* Entry point for each thread */
void *thread_handler(void *vargp)
{
    struct
    {
        int fd;
        struct sockaddr_in addr;
    } *arg = vargp;

    int connfd = arg->fd;
    struct sockaddr_in clientaddr = arg->addr;
    free(arg);

    rio_t rio;
    Rio_readinitb(&rio, connfd);
    handle_request(connfd, &clientaddr, &rio);
    Close(connfd);
    return NULL;
}

/* Entry point for each child process */
void process_handler(int connfd, struct sockaddr_in clientaddr)
{
    rio_t rio;
    Rio_readinitb(&rio, connfd);
    handle_request(connfd, &clientaddr, &rio);
}

/* Handle one HTTP/HTTPS request */
void handle_request(int connfd, struct sockaddr_in *clientaddr, rio_t *rio)
{
    char buf[MAXLINE], method[16], uri[MAXLINE], version[16];
    char hostname[MAXLINE], path[MAXLINE];
    int port = 80;

    /* Read request line */
    if (Rio_readlineb(rio, buf, MAXLINE) <= 0)
        return;
    sscanf(buf, "%15s %8191s %15s", method, uri, version);

    /* Read and discard request headers until CRLF */
    while (Rio_readlineb(rio, buf, MAXLINE) > 0)
    {
        if (strcmp(buf, "\r\n") == 0)
            break;
    }

    /* Parse host, port, and path */
    parse_uri(uri, hostname, &port, path);

    if (strcasecmp(method, "CONNECT") == 0)
    {
        /* HTTPS Tunnel */
        handle_connect(connfd, hostname, port, clientaddr, rio);
    }
    else
    {
        /* Regular HTTP request */
        int response_size = 0;
        forward_http(connfd, hostname, port, path, rio, method, uri, &response_size);

        /* Log the HTTP request */
        char logstr[MAXLINE];
        format_http_log(logstr, clientaddr, uri, response_size);

        /* Write log with synchronization */
        if (mode == MODE_PROCESS)
        {
            int fd = open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
            struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, 0};
            fcntl(fd, F_SETLKW, &lock);
            dprintf(fd, "%s\n", logstr);
            lock.l_type = F_UNLCK;
            fcntl(fd, F_SETLK, &lock);
            close(fd);
        }
        else
        {
            pthread_mutex_lock(&log_mutex);
            FILE *f = fopen(LOGFILE, "a");
            fprintf(f, "%s\n", logstr);
            fclose(f);
            pthread_mutex_unlock(&log_mutex);
        }
    }
}

/* Parse a URI into hostname, port, and path */
void parse_uri(const char *uri, char *hostname, int *port, char *path)
{
    *port = 80;
    char *hostbegin = strstr(uri, "//");
    hostbegin = hostbegin ? hostbegin + 2 : (char *)uri;
    char *pathbegin = strchr(hostbegin, '/');
    if (pathbegin)
    {
        strcpy(path, pathbegin);
        *pathbegin = '\0';
    }
    else
    {
        strcpy(path, "/");
    }
    char *portbegin = strchr(hostbegin, ':');
    if (portbegin)
    {
        *port = atoi(portbegin + 1);
        *portbegin = '\0';
    }
    strncpy(hostname, hostbegin, MAXLINE);
}

/* Forward an HTTP request, collect response size */
int forward_http(int clientfd, const char *hostname, int port, const char *path,
                 rio_t *rio, const char *method, char *uri, int *response_size)
{
    int serverfd = Open_clientfd(hostname, port);
    if (serverfd < 0)
    {
        clienterror(clientfd, "502", "Bad Gateway", "Could not connect");
        return 0;
    }

    char buf[MAXLINE];
    sprintf(buf, "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
            method, path, hostname);
    Rio_writen(serverfd, buf, strlen(buf));

    rio_t server_rio;
    Rio_readinitb(&server_rio, serverfd); // Initialize for server

    int n;
    *response_size = 0;
    while ((n = Rio_readnb(&server_rio, buf, MAXLINE)) > 0)
    {
        Rio_writen(clientfd, buf, n);
        *response_size += n;
    }
    Close(serverfd);
    return *response_size;
}

/* Handle CONNECT method: establish tunnel and relay */
void handle_connect(int clientfd, const char *hostname, int port,
                    struct sockaddr_in *clientaddr, rio_t *rio)
{
    int serverfd = Open_clientfd(hostname, port);
    if (serverfd < 0)
    {
        clienterror(clientfd, "502", "Bad Gateway", "Tunnel failed");
        return;
    }
    /* Acknowledge tunnel creation */
    const char *ok_resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
    Rio_writen(clientfd, ok_resp, strlen(ok_resp));


    long sent_bytes = 0, recv_bytes = 0;
    relay(clientfd, serverfd, &sent_bytes, &recv_bytes);

    /* Log CONNECT metadata */
    char logstr[MAXLINE];
    format_connect_log(logstr, clientaddr, hostname, port, sent_bytes, recv_bytes);
    if (mode == MODE_PROCESS)
    {
        int fd = open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        struct flock lock = {F_WRLCK, SEEK_SET, 0, 0, 0};
        fcntl(fd, F_SETLKW, &lock);
        dprintf(fd, "%s\n", logstr);
        lock.l_type = F_UNLCK;
        fcntl(fd, F_SETLK, &lock);
        close(fd);
    }
    else
    {
        pthread_mutex_lock(&log_mutex);
        FILE *f = fopen(LOGFILE, "a");
        if (f)
        {
            fprintf(f, "%s\n", logstr);
            fclose(f);
        }
        pthread_mutex_unlock(&log_mutex);
    }
    Close(serverfd);
}

/* Relay data between client and server with timeout */
void relay(int clientfd, int serverfd, long *sent_bytes, long *recv_bytes)
{
    fd_set read_set;
    struct timeval timeout;
    char buf[MAXLINE];
    int maxfd = clientfd > serverfd ? clientfd : serverfd;

    while (1)
    {
        FD_ZERO(&read_set);
        FD_SET(clientfd, &read_set);
        FD_SET(serverfd, &read_set);
        timeout.tv_sec = IDLE_TIMEOUT;
        timeout.tv_usec = 0;
        int ready = select(maxfd + 1, &read_set, NULL, NULL, &timeout);
        if (ready <= 0)
            break; /* timeout or error */

        if (FD_ISSET(clientfd, &read_set))
        {
            int n = read(clientfd, buf, MAXLINE);
            if (n <= 0)
                break;
            write(serverfd, buf, n);
            *sent_bytes += n;
        }
        if (FD_ISSET(serverfd, &read_set))
        {
            int n = read(serverfd, buf, MAXLINE);
            if (n <= 0)
                break;
            write(clientfd, buf, n);
            *recv_bytes += n;
        }
    }
}

/* Format HTTP log entry */
void format_http_log(char *logstring, struct sockaddr_in *sockaddr,
                     const char *uri, int size)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%a %d %b %Y %H:%M:%S %Z", &tm);
    unsigned long h = ntohl(sockaddr->sin_addr.s_addr);
    sprintf(logstring, "%s: %lu.%lu.%lu.%lu %s %d",
            tbuf,
            (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff,
            uri, size);
}

/* Format CONNECT log entry */
void format_connect_log(char *logstring, struct sockaddr_in *sockaddr,
                        const char *hostname, int port,
                        long sent_bytes, long recv_bytes)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%a %d %b %Y %H:%M:%S %Z", &tm);
    unsigned long h = ntohl(sockaddr->sin_addr.s_addr);
    sprintf(logstring,
            "%s: CONNECT from %lu.%lu.%lu.%lu to %s:%d, Data Sent: %ld / Received: %ld",
            tbuf,
            (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff,
            hostname, port, sent_bytes, recv_bytes);
}

/* clienterror helper for CONNECT/HTTP errors */
void clienterror(int fd, const char *errnum, const char *shortmsg, const char *longmsg)
{
    char buf[MAXLINE], body[MAXLINE];
    sprintf(body,
            "<html><title>%s %s</title>"
            "<body bgcolor=\"white\">"
            "<center><h1>%s: %s</h1></center>"
            "<hr><center>Proxy Server</center>"
            "</body></html>",
            errnum, shortmsg, errnum, longmsg);
    sprintf(buf, "HTTP/1.1 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}
