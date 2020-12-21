#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <liburing.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>

#include "http.h"
#include "logger.h"

#define Queue_Depth 2048
#define MAXLINE 8192
#define SHORTLINE 512
#define WEBROOT "./www"
#define TIMEOUT_MSEC 500

#define accept 0
#define read 1
#define write 2
#define prov_buf 3
#define uring_timer 4

#define MAX_CONNECTIONS 2048
#define MAX_MESSAGE_LEN 8192
char bufs[MAX_CONNECTIONS][MAX_MESSAGE_LEN] = {0};
int group_id = 8888;

struct io_uring ring;

#define PoolLength Queue_Depth
#define BitmapSize PoolLength/32
uint32_t bitmap[BitmapSize];
http_request_t *pool_ptr;


static void add_read_request(http_request_t *request);
static void add_write_request(int fd, void *usrbuf, size_t n, http_request_t *r);
static void add_provide_buf(int bid);

void sigint_handler(int signo) {
    printf("^C pressed. Shutting down.\n");
    free(pool_ptr);
    io_uring_queue_exit(&ring);
    exit(1);
}

static void msec_to_ts(struct __kernel_timespec *ts, unsigned int msec)
{
	ts->tv_sec = msec / 1000;
	ts->tv_nsec = (msec % 1000) * 1000000;
}

int init_memorypool() {
    pool_ptr = calloc(PoolLength ,sizeof(http_request_t));
    for (int i=0 ; i <PoolLength ; i++)
    {
        if(! &pool_ptr[i])
        {
            printf("Memory %d calloc fai\n",i);
            exit(1);
        }
    }
    return 0;
}

http_request_t *get_request() {
    int pos;
    uint32_t bitset ;

    for (int i = 0 ; i < BitmapSize ; i++) {
        bitset = bitmap[i];
        if(!(bitset ^ 0xffffffff))
            continue;
        
        for(int k = 0 ; k < 32 ; k++) {      
            if (!((bitset >> k) & 0x1)) {
                bitmap[i] ^= (0x1 << k);
                pos = 32*i + k;
                (&pool_ptr[pos])->pool_id = pos;
                return &pool_ptr[pos];
            }
        }
    }
    return NULL;
}

int free_request(http_request_t *req)
{
    int pos = req->pool_id;
    bitmap[pos/32] ^= (0x1 << (pos%32));
    return 0;
}

static char *webroot = NULL;

typedef struct {
    const char *type;
    const char *value;
} mime_type_t;

static mime_type_t mime[] = {{".html", "text/html"},
                             {".xml", "text/xml"},
                             {".xhtml", "application/xhtml+xml"},
                             {".txt", "text/plain"},
                             {".pdf", "application/pdf"},
                             {".png", "image/png"},
                             {".gif", "image/gif"},
                             {".jpg", "image/jpeg"},
                             {".css", "text/css"},
                             {NULL, "text/plain"}};

void init_ring()
{
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    int ret = io_uring_queue_init_params(Queue_Depth, &ring, &params);
    assert(ret >= 0 && "io_uring_queue_init");
    
    if (!(params.features & IORING_FEAT_FAST_POLL)) {
        printf("IORING_FEAT_FAST_POLL not available in the kernel, quiting...\n");
        exit(0);
    }
    
    struct io_uring_probe *probe;
    probe = io_uring_get_probe_ring(&ring);
    if (!probe || !io_uring_opcode_supported(probe, IORING_OP_PROVIDE_BUFFERS)) {
        printf("Buffer select not supported, skipping...\n");
        exit(0);
    }
    free(probe);

    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;

    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_provide_buffers(sqe, bufs, MAX_MESSAGE_LEN, MAX_CONNECTIONS, group_id, 0);

    io_uring_submit(&ring);
    io_uring_wait_cqe(&ring, &cqe);
    if (cqe->res < 0) {
        printf("cqe->res = %d\n", cqe->res);
        exit(1);
    }
    io_uring_cqe_seen(&ring, cqe);
}

void io_uring_loop() {
    while(1)
    {
        struct io_uring_cqe *cqe ;
        unsigned head;
        unsigned count = 0;

        //io_uring_submit(&ring);
        //io_uring_wait_cqe(&ring, &cqe);

        io_uring_for_each_cqe(&ring, head, cqe){
            ++count;
            http_request_t *req = io_uring_cqe_get_data(cqe);
            int type = req->event_type ;
            printf("event type = %d\n",type);
            if ( type == accept ) {
                add_accept_request(req->fd, req);
                if (&(cqe->res)) {
                    int fd = cqe->res;
                    if (fd >= 0) {
                        http_request_t *request = get_request();
                        assert(request && "request memory malloc fault");
                        init_http_request(request, fd, WEBROOT);
                        add_read_request(request);
                    }
                }
            }

            else if ( type == read ) {
                int len = cqe->res ;
                if(len <= 0) { 
                    http_close_conn(req);
                }
                else {
                    req->bid = ( cqe->flags >> IORING_CQE_BUFFER_SHIFT );
                    int len = cqe->res;
                    handle_request(req, len);
                }
            }

            else if ( type == write ) {
                add_provide_buf(req->bid);
                int len = cqe->res ;
                if(len <= 0) {
                    printf("write err close conn\n");
                    http_close_conn(req);
                }
                else
                    add_read_request(req);
            }

            else if ( type == prov_buf ) {
                assert( req->res < 0 && "Provide buffer error" );
                free_request(req);
            }

            else if ( type == uring_timer ) {
                free_request(req);
            }
        }
        io_uring_cq_advance(&ring, count);
    }
}

static void parse_uri(char *uri, int uri_length, char *filename)
{
    assert(uri && "parse_uri: uri is NULL");
    uri[uri_length] = '\0';

    /* TODO: support query string, i.e.
     *       https://example.com/over/there?name=ferret
     * Reference: https://en.wikipedia.org/wiki/Query_string
     */
    char *question_mark = strchr(uri, '?');
    int file_length;
    if (question_mark) {
        file_length = (int) (question_mark - uri);
        debug("file_length = (question_mark - uri) = %d", file_length);
    } else {
        file_length = uri_length;
        debug("file_length = uri_length = %d", file_length);
    }

    /* uri_length can not be too long */
    if (uri_length > (SHORTLINE >> 1)) {
        log_err("uri too long: %.*s", uri_length, uri);
        return;
    }

    strcpy(filename, webroot);
    debug("before strncat, filename = %s, uri = %.*s, file_len = %d", filename,
          file_length, uri, file_length);
    strncat(filename, uri, file_length);

    char *last_comp = strrchr(filename, '/');
    char *last_dot = strrchr(last_comp, '.');
    if (!last_dot && filename[strlen(filename) - 1] != '/')
        strcat(filename, "/");

    if (filename[strlen(filename) - 1] == '/')
        strcat(filename, "index.html");

    debug("served filename = %s", filename);
}

static void do_error(int fd,
                     char *cause,
                     char *errnum,
                     char *shortmsg,
                     char *longmsg,
                     http_request_t *r)
{
    char header[MAXLINE], body[MAXLINE];

    sprintf(body,
            "<html><title>Server Error</title>"
            "<body>\n%s: %s\n<p>%s: %s\n</p>"
            "<hr><em>web server</em>\n</body></html>",
            errnum, shortmsg, longmsg, cause);

    sprintf(header,
            "HTTP/1.1 %s %s\r\n"
            "Server: seHTTPd\r\n"
            "Content-type: text/html\r\n"
            "Connection: close\r\n"
            "Content-length: %d\r\n\r\n",
            errnum, shortmsg, (int) strlen(body));

    add_write_request(fd, header, strlen(header), r);
    add_write_request(fd, body, strlen(body), r);
}

static const char *get_file_type(const char *type)
{
    if (!type)
        return "text/plain";

    int i;
    for (i = 0; mime[i].type; ++i) {
        if (!strcmp(type, mime[i].type))
            return mime[i].value;
    }
    return mime[i].value;
}

static const char *get_msg_from_status(int status_code)
{
    if (status_code == HTTP_OK)
        return "OK";

    if (status_code == HTTP_NOT_MODIFIED)
        return "Not Modified";

    if (status_code == HTTP_NOT_FOUND)
        return "Not Found";

    return "Unknown";
}

static void serve_static(int fd,
                         char *filename,
                         size_t filesize,
                         http_out_t *out,
                         http_request_t *r)
{
    char header[MAXLINE];

    const char *dot_pos = strrchr(filename, '.');
    const char *file_type = get_file_type(dot_pos);

    sprintf(header, "HTTP/1.1 %d %s\r\n", out->status,
            get_msg_from_status(out->status));

    if (out->keep_alive) {
        sprintf(header, "%sConnection: keep-alive\r\n", header);
        sprintf(header, "%sKeep-Alive: timeout=%d\r\n", header,
                TIMEOUT_MSEC);
    }

    if (out->modified) {
        char buf[SHORTLINE];
        sprintf(header, "%sContent-type: %s\r\n", header, file_type);
        sprintf(header, "%sContent-length: %zu\r\n", header, filesize);
        struct tm tm;
        localtime_r(&(out->mtime), &tm);
        strftime(buf, SHORTLINE, "%a, %d %b %Y %H:%M:%S GMT", &tm);
        sprintf(header, "%sLast-Modified: %s\r\n", header, buf);
    }

    sprintf(header, "%sServer: seHTTPd\r\n", header);
    sprintf(header, "%s\r\n", header);

    add_write_request(fd, header, strlen(header), r);

    if (!out->modified)
        return;

    int srcfd = open(filename, O_RDONLY, 0);
    assert(srcfd > 2 && "open error");

    int n = sendfile(fd, srcfd, 0, filesize);
    assert(n == filesize && "sendfile");

    close(srcfd);
}

static inline int init_http_out(http_out_t *o, int fd)
{
    o->fd = fd;
    o->keep_alive = false;
    o->modified = true;
    o->status = 0;
    return 0;
}

void add_accept_request(int sockfd, http_request_t *request)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring) ;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)&client_addr, &client_addr_len, 0);
    request->event_type = accept ;
    request->fd = sockfd ;
    io_uring_sqe_set_data(sqe, request);
    io_uring_submit(&ring);
}

static void add_read_request(http_request_t *request)
{
    int clientfd = request->fd ;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring) ;
    io_uring_prep_recv(sqe, clientfd, NULL, MAX_MESSAGE_LEN, 0);
    io_uring_sqe_set_flags(sqe, (IOSQE_BUFFER_SELECT | IOSQE_IO_LINK) );
    sqe->buf_group = group_id;

    request->event_type = read ;
    io_uring_sqe_set_data(sqe, request);

    struct __kernel_timespec ts;
    msec_to_ts(&ts, TIMEOUT_MSEC);
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_link_timeout(sqe, &ts, 0);
    http_request_t *timeout_req = get_request();
    assert(timeout_req && "malloc fault");
    timeout_req->event_type = uring_timer ;
    io_uring_sqe_set_data(sqe, timeout_req);
    io_uring_submit(&ring);
}

static void add_write_request(int fd, void *usrbuf, size_t n, http_request_t *r)
{
    char *bufp = usrbuf;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring) ;
    http_request_t *request = r;
    request->event_type = write ;
    unsigned long len = strlen(bufp);

    io_uring_prep_send(sqe, fd, bufp, len, 0);
    io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
    io_uring_sqe_set_data(sqe, request);

    struct __kernel_timespec ts;
    msec_to_ts(&ts, 1000);
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_link_timeout(sqe, &ts, 0);
    http_request_t *timeout_req = get_request();
    assert(timeout_req && "malloc fault");
    timeout_req->event_type = uring_timer ;
    io_uring_sqe_set_data(sqe, timeout_req);
    io_uring_submit(&ring);
}

static void add_provide_buf(int bid) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_provide_buffers(sqe, bufs[bid], MAX_MESSAGE_LEN, 1, group_id, bid);
    http_request_t *req = get_request();
    assert(req && "malloc fault");
    req->event_type = prov_buf ;
    io_uring_sqe_set_data(sqe, req);
    io_uring_submit(&ring);
}

void handle_request(void *ptr, int n)
{
    http_request_t *r = ptr;
    int fd = r->fd ;
    int rc;
    char filename[SHORTLINE];
    webroot = r->root;

    //clock_t t1, t2;
    //t1 = clock();

    r->buf = &bufs[r->bid];
    r->pos = 0;
    r->last = n;

    rc = http_parse_request_line(r);

    debug("uri = %.*s", (int) (r->uri_end - r->uri_start),
        (char *) r->uri_start);

    rc = http_parse_request_body(r);
    http_out_t *out = malloc(sizeof(http_out_t));
    init_http_out(out, fd);
    parse_uri(r->uri_start, r->uri_end - r->uri_start, filename);
        
    struct stat sbuf;
    if (stat(filename, &sbuf) < 0) {
        do_error(fd, filename, "404", "Not Found", "Can't find the file", r);
        return;
    }
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
        do_error(fd, filename, "403", "Forbidden", "Can't read the file", r);
        return;
    }

    out->mtime = sbuf.st_mtime;
    http_handle_header(r, out);

    if (!out->status) 
        out->status = HTTP_OK;

    serve_static(fd, filename, sbuf.st_size, out, r); 
    free(out);

    //t2 = clock();
    //printf("%lf\n", (t2-t1)/(double)(CLOCKS_PER_SEC);
    return ;
}

