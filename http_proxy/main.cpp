#include <iostream>
#include <string>
#include <map>

extern "C" {
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
// #include <sys/types.h>
// #include <sys/stat.h>
// #include <sys/stat.h>
// #include <sys/socket.h>
// #include <fcntl.h>
// #include <unistd.h>
// #include <dirent.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/listener.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>
#include <event2/bufferevent.h>
#include <event2/dns.h>
}

using namespace std;

class LibeventContext
{
    private:
        string ip;
        int port;
        int verbose;
        struct event_base *base;
        struct evdns_base *dnsbase;
        struct evhttp *http;

    public:
        map<struct evhttp_request *, struct evhttp_request *> mapConn;
        LibeventContext();
        ~LibeventContext();

        struct event_base *GetEventBase() {
            return base;
        }
        struct evdns_base *GetEvdnsBase() {
            return dnsbase;
        }
        struct evhttp *Gethttp() {
            return http;
        }

        int ParseOpts(int argc, char **argv);
        int InitLibevent();
        void RegisterHttpHandler();

};

LibeventContext LibeventCtx;
LibeventContext::LibeventContext()
{
    ip = "";
    port = 0;
    base = NULL;
    dnsbase = NULL;
    http = NULL;

    cout << "LibeventContext" << endl;
}

LibeventContext::~LibeventContext()
{
    if (http)
        evhttp_free(http);
    if (dnsbase)
        evdns_base_free(dnsbase, 1);
    if (base)
		event_base_free(base);
    
    cout << "~LibeventContext" << endl;
}

static int display_listen_sock(struct evhttp_bound_socket *handle)
{
	struct sockaddr_storage ss;
	evutil_socket_t fd;
	ev_socklen_t socklen = sizeof(ss);
	char addrbuf[128];
	void *inaddr;
	const char *addr;
	int got_port = -1;

	fd = evhttp_bound_socket_get_fd(handle);
	memset(&ss, 0, sizeof(ss));
	if (getsockname(fd, (struct sockaddr *)&ss, &socklen)) {
		perror("getsockname() failed");
		return 1;
	}

	if (ss.ss_family == AF_INET) {
		got_port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
		inaddr = &((struct sockaddr_in*)&ss)->sin_addr;
	} else if (ss.ss_family == AF_INET6) {
		got_port = ntohs(((struct sockaddr_in6*)&ss)->sin6_port);
		inaddr = &((struct sockaddr_in6*)&ss)->sin6_addr;
	} else {
		fprintf(stderr, "Weird address family %d\n",
		    ss.ss_family);
		return 1;
	}

	addr = evutil_inet_ntop(ss.ss_family, inaddr, addrbuf,
	    sizeof(addrbuf));
	if (addr) {
		printf("Listening on %s:%d\n", addr, got_port);
	} else {
		fprintf(stderr, "evutil_inet_ntop failed\n");
		return 1;
	}

	return 0;
}

static void do_term(int sig, short events, void *arg)
{
	struct event_base *base = (struct event_base *)arg;
	event_base_loopbreak(base);
	fprintf(stderr, "Got %i, Terminating\n", sig);
}

int LibeventContext::InitLibevent()
{

    struct evhttp_bound_socket *handle = NULL;
    int ret = 0;

    if (verbose)
		event_enable_debug_logging(EVENT_DBG_ALL);

    struct event_config *cfg = event_config_new();
	base = event_base_new_with_config(cfg);
	if (!base) {
		cout << "Couldn't create an event_base: exiting\n";
		return -1;
	}
    event_config_free(cfg);

    http = evhttp_new(base);
	if (!http) {
		cout << "couldn't create evhttp. Exiting.\n";
		return -2;
	}

    dnsbase = evdns_base_new(base, EVDNS_BASE_INITIALIZE_NAMESERVERS);
    if (!http) {
		cout << "couldn't create dnsbase. Exiting.\n";
		return -3;
	}

	evhttp_set_allowed_methods(http, 
		EVHTTP_REQ_PUT|
		EVHTTP_REQ_DELETE|
		EVHTTP_REQ_OPTIONS|
		EVHTTP_REQ_TRACE|
		EVHTTP_REQ_PATCH|
		EVHTTP_REQ_CONNECT|
		EVHTTP_REQ_GET|
		EVHTTP_REQ_POST|
		EVHTTP_REQ_HEAD);

	handle = evhttp_bind_socket_with_handle(http, ip.c_str(), port);
	if (!handle) {
		cout << "couldn't bind to port:" << port << ". Exiting.\n" ;
		return -4;
	}
	
	if (display_listen_sock(handle)) {
		cout << "display_listen_sock error\n" ;
		return -5;
	}

	struct event *term = evsignal_new(base, SIGINT, do_term, base);
	if (!term) {
        cout << "evsignal_new error\n" ;
		return -6;
    }
		
	if (event_add(term, NULL)) {
        cout << "evsignal_new error\n" ;
		return -6;
    }

    RegisterHttpHandler();
    return ret;
}


// ./http_proxy 9.135.8.82 18023 -v
int LibeventContext::ParseOpts(int argc, char **argv)
{
    if (argc < 3) {
        cout << "cmd line error!" << endl;
        return -1;
    }

    ip = argv[1];
    port = atoi(argv[2]);

    if (argc == 4)
        verbose = 1;

    cout << "ip:" << ip << " port:" << port << " verbose:" << verbose << endl;
	return 0;
}

static void
readcb(struct bufferevent *bev, void *ctx)
{
	struct bufferevent *partner = (struct bufferevent *)ctx;
	struct evbuffer *src, *dst;
	size_t len;

	src = bufferevent_get_input(bev);
	len = evbuffer_get_length(src);	
	//printf("readcb len:%ld\n", len);
	if (!partner) {
		evbuffer_drain(src, len);
		return;
	}
	dst = bufferevent_get_output(partner);
	evbuffer_add_buffer(dst, src);
}

static void
close_on_finished_writecb(struct bufferevent *bev, void *ctx)
{
	struct evbuffer *b = bufferevent_get_output(bev);
	printf("close_on_finished_writecb\n");
	if (evbuffer_get_length(b) == 0) {
		bufferevent_free(bev);
	}
}


static void
eventcb(struct bufferevent *bev, short what, void *ctx)
{
	struct bufferevent *partner = (struct bufferevent *)ctx;
	printf("eventcb, what:%d ", what);
	if ((what & BEV_EVENT_READING) == BEV_EVENT_READING) {
		printf(" BEV_EVENT_READING");
	}
	if ((what & BEV_EVENT_WRITING) == BEV_EVENT_WRITING) {
		printf(" BEV_EVENT_WRITING");
	}
	if ((what & BEV_EVENT_EOF) == BEV_EVENT_EOF) {
		printf(" BEV_EVENT_EOF");
	}
	if ((what & BEV_EVENT_ERROR) == BEV_EVENT_ERROR) {
		printf(" BEV_EVENT_ERROR");
	}
	if ((what & BEV_EVENT_TIMEOUT) == BEV_EVENT_TIMEOUT) {
		printf(" BEV_EVENT_TIMEOUT");
	}
	if ((what & BEV_EVENT_CONNECTED) == BEV_EVENT_CONNECTED) {
		printf(" BEV_EVENT_CONNECTED");
	}
	printf("\n");

	if (what & (BEV_EVENT_EOF|BEV_EVENT_ERROR)) {
		if (what & BEV_EVENT_ERROR) {
			if (errno)
				perror("connection error");
		}

		if (partner) {
			/* Flush all pending data */
			readcb(bev, ctx);

			if (evbuffer_get_length(
				    bufferevent_get_output(partner))) {
				/* We still have to flush data from the other
				 * side, but when that's done, close the other
				 * side. */
				bufferevent_setcb(partner,
				    NULL, close_on_finished_writecb,
				    eventcb, NULL);
				bufferevent_disable(partner, EV_READ);
			} else {
				/* We have nothing left to say to the other
				 * side; close it. */
				bufferevent_free(partner);
			}
		}
		bufferevent_free(bev);
	}
}

void get_addr(int result, char type, int count, int ttl,
			  void *addrs, void *orig, char *buf, int buf_size) {
    
    struct evhttp_request *req = (struct evhttp_request *)orig;

    if (!count) {
		printf("%s: No answer (%d)\n", evhttp_request_get_host(req), result);
		return;
	}

	if (type == DNS_IPv4_A) {
		inet_ntop(AF_INET, &((ev_uint32_t*)addrs)[0], buf, buf_size);
		printf("DNS_IPv4_A %s: %s ttl:%d\n", evhttp_request_get_host(req), buf, ttl);
	} else if (type == DNS_PTR) {
		snprintf(buf, buf_size, "%s", ((char**)addrs)[0]);
		printf("DNS_PTR %s: %s ttl:%d\n", evhttp_request_get_host(req), ((char**)addrs)[0], ttl);
	}
}

static void
https_dns_callback(int result, char type, int count, int ttl,
			  void *addrs, void *orig) {

	struct evhttp_request *req = (struct evhttp_request *)orig;
	static struct sockaddr_storage connect_to_addr;
	static int connect_to_addrlen = sizeof(connect_to_addr);

    char buf[32] = {0};
    get_addr(result, type, count, ttl, addrs, orig, buf, sizeof(buf));
    int port = evhttp_uri_get_port(evhttp_request_get_evhttp_uri(req));
    if (port == -1)
        port = 443;
    string addr = buf + string(":") + to_string(port);

	// 建立连接
	struct bufferevent *b_out = bufferevent_socket_new(LibeventCtx.GetEventBase(), -1,
		BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);

	if (evutil_parse_sockaddr_port(addr.c_str(),
		(struct sockaddr*)&connect_to_addr, &connect_to_addrlen)<0)
		printf("evutil_parse_sockaddr_port error\n");

	if (bufferevent_socket_connect(b_out,
		(struct sockaddr*)&connect_to_addr, connect_to_addrlen)<0) {
		perror("bufferevent_socket_connect");
		bufferevent_free(b_out);
		return;
	}

	struct evhttp_connection *evcon = evhttp_request_get_connection(req);
	bufferevent_setcb(b_out, readcb, NULL, eventcb, evhttp_connection_get_bufferevent(evcon));
	bufferevent_enable(b_out, EV_READ|EV_WRITE);

	// CONNECT请求回包
	evhttp_send_reply(req, 200, "Connection Established", NULL);

	// 修改此连接的读写回调函数
	bufferevent_setcb(evhttp_connection_get_bufferevent(evcon),readcb,NULL,eventcb,b_out);
}

void http_header_copy(struct evhttp_request *from_req, struct evhttp_request *to_req)
{
    struct evkeyval *header;
    struct evkeyvalq *headers = evhttp_request_get_input_headers(from_req);
    struct evkeyvalq *output_headers = evhttp_request_get_output_headers(to_req);

    for (header = headers->tqh_first; header; header = header->next.tqe_next) {
        string str_key = string(header->key);
        printf("%s: %s\n", header->key, header->value);
		evhttp_add_header(output_headers, header->key, header->value);
	}
    evhttp_add_header(output_headers, "Proxy-Connection", "keep-alive");
    printf("\r\n");
}

static void
http_request_done(struct evhttp_request *req, void *ctx)
{
    struct evhttp_request *old_req = (struct evhttp_request *)ctx;
    if (req == NULL) {
        printf("http_request_done null error\n");
        evhttp_request_free(old_req);
        return;
    }

	printf("Response line: %s %d %s\n",
		evhttp_request_get_host(req),
	    evhttp_request_get_response_code(req),
	    evhttp_request_get_response_code_line(req));

	http_header_copy(req, old_req);

	evhttp_send_reply(old_req, 
		evhttp_request_get_response_code(req), 
		evhttp_request_get_response_code_line(req), 
		evhttp_request_get_input_buffer(req));

}

static void
http_dns_callback(int result, char type, int count, int ttl,
			  void *addrs, void *orig) {

	struct evhttp_request *req = (struct evhttp_request *)orig;
	static struct sockaddr_storage connect_to_addr;
	static int connect_to_addrlen = sizeof(connect_to_addr);

	char buf[32] = {0};
    get_addr(result, type, count, ttl, addrs, orig, buf, sizeof(buf));
    int port = evhttp_uri_get_port(evhttp_request_get_evhttp_uri(req));
    if (port == -1)
        port = 80;

	// 新连接
    struct bufferevent *new_be = bufferevent_socket_new(LibeventCtx.GetEventBase(), -1, BEV_OPT_CLOSE_ON_FREE);
    struct evhttp_connection *new_conn = evhttp_connection_base_bufferevent_new(LibeventCtx.GetEventBase(), NULL, new_be,
        buf, port);
    if (new_conn == NULL) {
        fprintf(stderr, "evhttp_connection_base_bufferevent_new() failed\n");
    }

    struct evhttp_request *new_req = evhttp_request_new(http_request_done, req);
    if (new_req == NULL) {
        fprintf(stderr, "evhttp_request_new() failed\n");
    }

    LibeventCtx.mapConn[req] = new_req;
    
    // 复制请求头
    http_header_copy(req, new_req);
    // 复制请求体
    evbuffer_add_buffer(evhttp_request_get_output_buffer(new_req), evhttp_request_get_input_buffer(req));

    if (evhttp_make_request(new_conn, new_req, evhttp_request_get_command(req), evhttp_request_get_uri(req)) != 0) {
        fprintf(stderr, "evhttp_make_request() failed\n");
	}
}

static void proxy_request_cb(struct evhttp_request *req, void *arg)
{
	const char *cmdtype;
	switch (evhttp_request_get_command(req)) {
	case EVHTTP_REQ_GET: cmdtype = "GET"; break;
	case EVHTTP_REQ_POST: cmdtype = "POST"; break;
	case EVHTTP_REQ_HEAD: cmdtype = "HEAD"; break;
	case EVHTTP_REQ_PUT: cmdtype = "PUT"; break;
	case EVHTTP_REQ_DELETE: cmdtype = "DELETE"; break;
	case EVHTTP_REQ_OPTIONS: cmdtype = "OPTIONS"; break;
	case EVHTTP_REQ_TRACE: cmdtype = "TRACE"; break;
	case EVHTTP_REQ_CONNECT: cmdtype = "CONNECT"; break;
	case EVHTTP_REQ_PATCH: cmdtype = "PATCH"; break;
	default: cmdtype = "unknown"; break;
	}

	printf("Received a %s request for %s host:%s port:%d \n",
	    cmdtype, evhttp_request_get_uri(req), evhttp_request_get_host(req), evhttp_uri_get_port(evhttp_request_get_evhttp_uri(req)));

	
    // 处理 https 请求
	if (evhttp_request_get_command(req) == EVHTTP_REQ_CONNECT) {
		// 异步域名解析
		evdns_base_resolve_ipv4(LibeventCtx.GetEvdnsBase(), evhttp_request_get_host(req), 0, https_dns_callback, req);
		return;
	}

    // 处理 http 请求
    auto iter = LibeventCtx.mapConn.find(req);
    if (iter != LibeventCtx.mapConn.end()) {
        printf("!!!!!!!connection reuse!!!!!!!!!\n");
    }

    // 异步域名解析
    evdns_base_resolve_ipv4(LibeventCtx.GetEvdnsBase(), evhttp_request_get_host(req), 0, http_dns_callback, req);
    
}

void LibeventContext::RegisterHttpHandler()
{
    // 设置默认处理函数
    evhttp_set_gencb(http, proxy_request_cb, NULL);

    cout << "RegisterHttpHandler" << endl;
}

int main(int argc, char **argv)
{
    int ret = 0;
    ret = LibeventCtx.ParseOpts(argc, argv);
    if (ret != 0) {
        cout << "ParseOpts error!" << endl;
        return ret;
    }

    ret = LibeventCtx.InitLibevent();
    if (ret != 0) {
        cout << "InitLibevent error!" << endl;
        return ret;
    }

    event_base_dispatch(LibeventCtx.GetEventBase());
    return ret;
}