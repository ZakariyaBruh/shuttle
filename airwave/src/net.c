#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "common.h"

#define MAX_REQS 4
#define STALL_MS 25000
#define IDLE_DONE_MS 2000
#define MAX_REDIRECTS 10
#define TINY_BODY 8192   /* a redirect page, as opposed to real content */

static Req* reqs[MAX_REQS];
static const char* UA = "User-Agent: Airwave/1.0\r\nAccept: */*\r\nConnection: close\r\n";

static void* xrealloc(void* p, size_t n) { return pd->system->realloc(p, n); }

/* ---- connection callbacks (userdata = Req*) ---- */
static void on_header(HTTPConnection* c, const char* key, const char* value)
{
	Req* r = pd->network->http->getUserdata(c);
	if (r && strcasecmp(key, "location") == 0) snprintf(r->loc, sizeof r->loc, "%s", value);
}
static void on_headers(HTTPConnection* c)
{
	Req* r = pd->network->http->getUserdata(c);
	if (r) { r->status = pd->network->http->getResponseStatus(c); r->headers = true; }
}
static void on_complete(HTTPConnection* c) { Req* r = pd->network->http->getUserdata(c); if (r) r->complete = true; }
static void on_closed(HTTPConnection* c)   { Req* r = pd->network->http->getUserdata(c); if (r) r->closed = true; }

/* split "https://host:port/path" */
static bool split_url(const char* url, char* host, size_t hsz, int* port, bool* ssl, char* path, size_t psz)
{
	const char* p = url;
	if (strncmp(p, "https://", 8) == 0) { *ssl = true; p += 8; }
	else if (strncmp(p, "http://", 7) == 0) { *ssl = false; p += 7; }
	else return false;
	const char* slash = p + strcspn(p, "/?");
	size_t hl = (size_t)(slash - p);
	char hp[200];
	if (hl == 0 || hl >= sizeof hp) return false;
	memcpy(hp, p, hl); hp[hl] = 0;
	*port = 0;
	char* colon = strrchr(hp, ':');
	if (colon) { *port = atoi(colon + 1); *colon = 0; }
	snprintf(host, hsz, "%s", hp);
	if (*slash == '/') snprintf(path, psz, "%s", slash);
	else snprintf(path, psz, "/%s", slash);   /* "host?query" */
	return true;
}

static void drop_conn(Req* r)
{
	if (!r->conn) return;
	pd->network->http->close(r->conn);
	pd->network->http->setUserdata(r->conn, NULL);
	pd->network->http->release(r->conn);
	r->conn = NULL;
}

static bool connect_req(Req* r)
{
	static char path[URL_MAX];
	static char host[200]; int port; bool ssl;
	r->headers = r->complete = r->closed = false;
	r->status = 0; r->loc[0] = 0;
	r->lastProgress = pd->system->getCurrentTimeMilliseconds();
	if (!split_url(r->url, host, sizeof host, &port, &ssl, path, sizeof path)) {
		snprintf(r->error, sizeof r->error, "bad url");
		return false;
	}
	const struct playdate_http* h = pd->network->http;
	r->conn = h->newConnection(host, port, ssl);
	if (!r->conn) { snprintf(r->error, sizeof r->error, "network access not allowed"); return false; }
	h->setUserdata(r->conn, r);
	h->setConnectTimeout(r->conn, 10000);
	h->setReadTimeout(r->conn, 200);
	h->setHeaderReceivedCallback(r->conn, on_header);
	h->setHeadersReadCallback(r->conn, on_headers);
	h->setRequestCompleteCallback(r->conn, on_complete);
	h->setConnectionClosedCallback(r->conn, on_closed);
	PDNetErr e;
	if (r->postBody) {
		static const char* ph = "User-Agent: Airwave/1.0\r\nAccept: */*\r\nConnection: close\r\nContent-Type: application/json\r\n";
		e = h->post(r->conn, path, ph, strlen(ph), r->postBody, r->postLen);
	} else e = h->get(r->conn, path, UA, strlen(UA));
	if (e != NET_OK) { snprintf(r->error, sizeof r->error, "request failed (%d)", e); return false; }
	return true;
}

static Req* add_req(Req* r)
{
	for (int i = 0; i < MAX_REQS; i++)
		if (!reqs[i]) { reqs[i] = r; return r; }
	pd->system->realloc(r, 0);
	return NULL;
}

Req* net_get(const char* url, size_t maxBytes, ReqDone done, void* ud)
{
	Req* r = pd->system->realloc(NULL, sizeof(Req));
	memset(r, 0, sizeof *r);
	snprintf(r->url, sizeof r->url, "%s", url);
	r->max = maxBytes; r->done = done; r->ud = ud;
	r->cap = 4096; r->buf = xrealloc(NULL, r->cap);
	if (!add_req(r)) return NULL;
	if (!connect_req(r)) { r->status = -1; r->finished = true; }
	return r;
}

Req* net_post(const char* url, const char* jsonBody, size_t maxBytes, ReqDone done, void* ud)
{
	Req* r = pd->system->realloc(NULL, sizeof(Req));
	memset(r, 0, sizeof *r);
	snprintf(r->url, sizeof r->url, "%s", url);
	r->max = maxBytes; r->done = done; r->ud = ud;
	r->postLen = strlen(jsonBody);
	r->postBody = xrealloc(NULL, r->postLen + 1);
	memcpy(r->postBody, jsonBody, r->postLen + 1);
	r->cap = 4096; r->buf = xrealloc(NULL, r->cap);
	if (!add_req(r)) return NULL;
	if (!connect_req(r)) { r->status = -1; r->finished = true; }
	return r;
}

Req* net_stream(const char* url, size_t ringBytes)
{
	Req* r = pd->system->realloc(NULL, sizeof(Req));
	memset(r, 0, sizeof *r);
	snprintf(r->url, sizeof r->url, "%s", url);
	r->stream = true;
	if (!ring_init(&r->ring, (uint32_t)ringBytes)) { xrealloc(r, 0); return NULL; }
	if (!add_req(r)) return NULL;
	if (!connect_req(r)) { r->status = -1; r->finished = true; }
	return r;
}

void net_free(Req* r)
{
	if (!r) return;
	for (int i = 0; i < MAX_REQS; i++) if (reqs[i] == r) reqs[i] = NULL;
	drop_conn(r);
	if (r->buf) xrealloc(r->buf, 0);
	ring_free(&r->ring);
	if (r->postBody) xrealloc(r->postBody, 0);
	xrealloc(r, 0);
}

bool ring_init(Ring* g, uint32_t size)
{
	memset(g, 0, sizeof *g);
	g->buf = xrealloc(NULL, size);
	g->size = g->buf ? size : 0;
	return g->buf != NULL;
}

void ring_free(Ring* g) { if (g->buf) xrealloc(g->buf, 0); memset(g, 0, sizeof *g); }

uint32_t ring_fill(Ring* g)
{
	return __atomic_load_n(&g->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&g->tail, __ATOMIC_ACQUIRE);
}

uint32_t ring_space(Ring* g) { return g->size - ring_fill(g); }

void ring_put(Ring* g, const uint8_t* data, uint32_t n)
{
	uint32_t head = g->head;
	for (uint32_t i = 0; i < n; i++) g->buf[(head + i) % g->size] = data[i];
	__atomic_store_n(&g->head, head + n, __ATOMIC_RELEASE);
}

uint32_t ring_peek(Ring* g, uint32_t offset, uint8_t* out, uint32_t n)
{
	uint32_t avail = ring_fill(g);
	if (offset >= avail) return 0;
	if (n > avail - offset) n = avail - offset;
	uint32_t tail = g->tail + offset;
	for (uint32_t i = 0; i < n; i++) out[i] = g->buf[(tail + i) % g->size];
	return n;
}

/* consumer side: may run on the audio thread */
int ring_read(Ring* g, uint8_t* out, int n)
{
	uint32_t head = __atomic_load_n(&g->head, __ATOMIC_ACQUIRE);
	uint32_t tail = g->tail;
	uint32_t avail = head - tail;
	if ((uint32_t)n > avail) n = (int)avail;
	if (out) for (int i = 0; i < n; i++) out[i] = g->buf[(tail + i) % g->size];   /* out == NULL: the decoder is skipping bytes */
	__atomic_store_n(&g->tail, tail + n, __ATOMIC_RELEASE);
	return n;
}

static void fail(Req* r, const char* why)
{
	/* one automatic retry for list/feed fetches: Wi-Fi and CDNs flake */
	if (!r->stream && r->retries < 1 && !(r->status >= 400 && r->status < 500)) {
		r->retries++;
		drop_conn(r);
		r->len = 0; r->error[0] = 0;
		if (connect_req(r)) return;
	}
	if (!r->error[0]) snprintf(r->error, sizeof r->error, "%s", why);
	r->status = -1; r->finished = true;
}

/* The client follows some redirects itself but hands back others as a small, completed reply with a Location header. */
static bool wants_manual_redirect(Req* r)
{
	size_t got = r->stream ? r->total : r->len;
	return r->loc[0] && (r->complete || r->closed) && got < TINY_BODY && !(r->status >= 200 && r->status < 300);
}

static void follow_redirect(Req* r)
{
	static char next[URL_MAX];
	if (r->redirects++ >= MAX_REDIRECTS) { fail(r, "too many redirects"); return; }
	if (strncmp(r->loc, "http", 4) == 0) snprintf(next, sizeof next, "%s", r->loc);
	else {                                  /* relative */
		static char base[URL_MAX]; snprintf(base, sizeof base, "%s", r->url);
		char* hostEnd = strchr(base + 8, '/');
		if (hostEnd) *hostEnd = 0;
		snprintf(next, sizeof next, "%s%s%s", base, r->loc[0] == '/' ? "" : "/", r->loc);
	}
	snprintf(r->url, sizeof r->url, "%s", next);
	drop_conn(r);
	r->len = 0; r->total = 0;
	if (r->ring.buf) r->ring.tail = r->ring.head;
	if (!connect_req(r)) fail(r, r->error[0] ? r->error : "redirect failed");
}

static void pump(Req* r)
{
	const struct playdate_http* h = pd->network->http;
	size_t avail = h->getBytesAvailable(r->conn);
	int guard = 0;
	while (avail > 0 && guard++ < 32) {
		static uint8_t tmp[2048];     /* static: the device's C stack is small */
		size_t want = avail < sizeof tmp ? avail : sizeof tmp;
		if (r->stream) {
			uint32_t free_ = ring_space(&r->ring);
			if (free_ == 0) break;                 /* back-pressure: leave data in the connection */
			if (want > free_) want = free_;
		} else if (r->len + want > r->max) {
			want = r->max - r->len;
			if (want == 0) { r->complete = true; break; }   /* truncated: we only need the head of big feeds */
		}
		int n = h->read(r->conn, tmp, (unsigned)want);
		if (n <= 0) break;
		r->lastProgress = pd->system->getCurrentTimeMilliseconds();
		if (r->stream) {
			ring_put(&r->ring, tmp, (uint32_t)n);
			r->total += n;
		} else {
			if (r->len + n + 1 > r->cap) {
				while (r->len + n + 1 > r->cap) r->cap *= 2;
				r->buf = xrealloc(r->buf, r->cap);
			}
			memcpy(r->buf + r->len, tmp, n);
			r->len += n;
		}
		avail = h->getBytesAvailable(r->conn);
	}
}

void net_update(void)
{
	uint32_t now = pd->system->getCurrentTimeMilliseconds();
	for (int i = 0; i < MAX_REQS; i++) {
		Req* r = reqs[i];
		if (!r || r->finished || !r->conn) continue;
		const struct playdate_http* h = pd->network->http;

		if (!r->headers) {            /* don't rely on the headers-read callback firing for short responses */
			int st = h->getResponseStatus(r->conn);
			if (st > 0) { r->status = st; r->headers = true; }
		}
		if (wants_manual_redirect(r)) {
			follow_redirect(r);
			if (r->finished && r->done) { ReqDone d = r->done; r->done = NULL; d(r); }
			continue;
		}
		if (r->headers && r->status >= 400) { char m[40]; snprintf(m, sizeof m, "HTTP %d", r->status); fail(r, m); }
		else pump(r);

		if (r->finished) { }
		else if (r->stream) {
			if (r->closed && h->getBytesAvailable(r->conn) == 0 && ring_fill(&r->ring) == 0) fail(r, "stream ended");
		} else {
			/* Servers using chunked encoding / keep-alive may never fire the "complete" callback, so also
			   finish when Content-Length has been read, or after the body has been idle for a moment. */
			int rd = 0, tot = 0;
			h->getProgress(r->conn, &rd, &tot);
			bool drained = h->getBytesAvailable(r->conn) == 0;
			if (drained && ((tot > 0 && r->len >= (size_t)tot) || (r->len > 0 && now - r->lastProgress > IDLE_DONE_MS)))
				r->complete = true;
			if ((r->complete || r->closed) && (drained || r->len >= r->max)) {
				if (r->buf) r->buf[r->len] = 0;
				r->finished = true;
				if (r->status == 0) r->status = 200;
			}
		}
		if (!r->finished && now - r->lastProgress > STALL_MS && !(r->stream && ring_fill(&r->ring) > 0))
			fail(r, "timed out");
		if (r->finished && r->done) { ReqDone d = r->done; r->done = NULL; d(r); }
	}
}
