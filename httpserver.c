// Asgn 2: A simple HTTP server.
// By: Eugene Chou
//     Andrew Quinn
//     Brian Zhao

#include "queue.h"
#include "asgn2_helper_funcs.h"
#include "connection.h"
#include "debug.h"
#include "response.h"
#include "request.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <assert.h>

#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>

void handle_connection(int connfd);

void handle_get(conn_t *);
void handle_put(conn_t *);
void handle_unsupported(conn_t *);

queue_t *queue;
pthread_mutex_t mutex;
void do_work() {
    while (1) {
        int connfd;
        queue_pop(queue, (void **) &connfd);
        handle_connection(connfd);
    }
}

void audit_logger(conn_t *conn, int16_t status) {
    // const Response_t *res = conn_parse(conn);
    const Request_t *req = conn_get_request(conn);
    const char *request = request_get_str(req);
    char *uri = conn_get_uri(conn);
    // int16_t status = response_get_code(res);
    char *header = conn_get_header(conn, "Request-Id");

    header = (!header ? "0" : header);
    fprintf(stderr, "%s,/%s,%hu,%s\n", request, uri, status, header);
}
int main(int argc, char **argv) {
    size_t port = 0;

    int opt = 0;
    int threads = 4;
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        switch (opt) {
        case 't': threads = strtol(optarg, NULL, 10); break;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "invalid arguments\n");
    }

    port = strtol(argv[optind], NULL, 10);
    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sock;
    listener_init(&sock, port);

    // init queues and threads
    queue = queue_new(16);
    // Initialize threads
    pthread_t workers[threads];

    for (int i = 0; i < threads; i++) {
        pthread_create(&workers[i], NULL, (void *(*) (void *) ) do_work, NULL);
    }

    int rc = pthread_mutex_init(&mutex, NULL);
    assert(!rc);

    while (1) {
        int connfd = listener_accept(&sock);
        queue_push(queue, (void *) ((uintptr_t) connfd));
    }
    queue_delete(&queue);
    pthread_mutex_destroy(&mutex);
    return EXIT_SUCCESS;
}

void handle_connection(int connfd) {

    conn_t *conn = conn_new(connfd);
    const Response_t *res = conn_parse(conn);

    if (res != NULL) {
        conn_send_response(conn, res);
    } else {
        debug("%s", conn_str(conn));
        const Request_t *req = conn_get_request(conn);
        if (req == &REQUEST_GET) {
            handle_get(conn);
        } else if (req == &REQUEST_PUT) {
            handle_put(conn);
        } else {
            handle_unsupported(conn);
        }
    }

    conn_delete(&conn);
    close(connfd);
}

void handle_get(conn_t *conn) {

    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;

    // What are the steps in here?

    // 1. Open the file.
    // If  open it returns < 0, then use the result appropriately
    //   a. Cannot access -- use RESPONSE_FORBIDDEN
    //   b. Cannot find the file -- use RESPONSE_NOT_FOUND
    //   c. other error? -- use RESPONSE_INTERNAL_SERVER_ERROR
    // (hint: check errno for these cases)!

    // pthread_mutex_lock(&mutex);
    int fd = open(uri, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES) {
            res = &RESPONSE_FORBIDDEN;
        } else if (errno == ENOENT) {
            res = &RESPONSE_NOT_FOUND;
        } else {
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
        }
        conn_send_response(conn, res);
        // pthread_mutex_unlock(&mutex);
        audit_logger(conn, response_get_code(res));
        return;
    }
    flock(fd, LOCK_EX);
    // pthread_mutex_unlock(&mutex);

    // 2. Get the size of the file.
    // (hint: checkout the function fstat)!

    // Get the size of the file.
    struct stat stats;
    fstat(fd, &stats);
    off_t size = stats.st_size;

    // 3. Check if the file is a directory, because directories *will*
    // open, but are not valid.
    // (hint: checkout the macro "S_IFDIR", which you can use after you call fstat!)

    if (S_ISDIR(stats.st_mode)) {
        res = &RESPONSE_FORBIDDEN;
        conn_send_response(conn, res);
        audit_logger(conn, response_get_code(res));
        close(fd);
        return;
    }

    // 4. Send the file
    // (hint: checkout the conn_send_file function!)
    res = &RESPONSE_OK;
    conn_send_file(conn, fd, size);
    audit_logger(conn, response_get_code(res));
    close(fd);
    return;
}

void handle_unsupported(conn_t *conn) {
    debug("handling unsupported request");

    // send responses
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
}

void handle_put(conn_t *conn) {

    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;
    debug("handling put request for %s", uri);

    pthread_mutex_lock(&mutex);
    // Check if file already exists before opening it.
    bool existed = access(uri, F_OK) == 0;
    debug("%s existed? %d", uri, existed);

    // Open the file..

    int fd = open(uri, O_CREAT | O_WRONLY, 0600);
    if (fd < 0) {
        debug("%s: %d", uri, errno);
        if (errno == EACCES || errno == EISDIR || errno == ENOENT) {
            res = &RESPONSE_FORBIDDEN;
            conn_send_response(conn, res);
        } else {
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
            conn_send_response(conn, res);
        }
        pthread_mutex_unlock(&mutex);
        audit_logger(conn, response_get_code(res));
        return;
    }

    int a = flock(fd, LOCK_EX);
    pthread_mutex_unlock(&mutex);
    ftruncate(fd, 0);

    if (a < 0) {
        fprintf(stderr, "error locking file\n");
    }
    res = conn_recv_file(conn, fd);

    if (res == NULL && existed) {
        res = &RESPONSE_OK;
    } else if (res == NULL && !existed) {
        res = &RESPONSE_CREATED;
    }
    audit_logger(conn, response_get_code(res));
    conn_send_response(conn, res);
    close(fd);
}
