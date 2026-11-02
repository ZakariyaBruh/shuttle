#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pd_api.h"

extern PlaydateAPI* pd;

/* ---- net.c: async HTTP(S) GET (the Playdate client follows redirects itself) ---- */
#define URL_MAX 2600
typedef struct Req Req;
typedef void (*ReqDone)(Req* r);

struct Req {
	HTTPConnection* conn;
	char url[URL_MAX];
	char loc[URL_MAX];     /* Location header of the last reply */
	int redirects;
	/* buffered mode */
	char* buf;
	size_t len, cap, max;
	/* stream mode: single-producer (net_update) / single-consumer (audio thread) ring */
	uint8_t* ring;
	uint32_t rsize;
	volatile uint32_t head, tail;
	uint32_t total;
	bool stream;
	/* state */
	int status;       /* HTTP status if the client reported one (it follows redirects itself), -1 on failure */
	int retries;
	bool headers, complete, closed, finished;
	uint32_t lastProgress;
	char error[64];
	ReqDone done;
	void* ud;
};

Req* net_get(const char* url, size_t maxBytes, ReqDone done, void* ud);
Req* net_stream(const char* url, size_t ringBytes);
void net_update(void);                 /* call every frame */
void net_free(Req* r);                 /* cancels if still running */
uint32_t ring_fill(Req* r);
int ring_read(Req* r, uint8_t* out, int n);

/* ---- parse.c: tiny JSON / RSS scanners (no allocation) ---- */
typedef struct { const char* s; size_t n; } Span;

/* Iterate the objects of the array stored under `arrayKey` (NULL = top-level array). */
const char* json_next_object(const char** cursor, const char* end, Span* obj);
const char* json_array_start(const char* json, const char* arrayKey);
bool json_get_str(Span obj, const char* key, char* out, size_t outsz);
long json_get_int(Span obj, const char* key, long dflt);

bool rss_next_item(const char** cursor, Span* item);
bool rss_tag_text(Span item, const char* tag, char* out, size_t outsz);
bool rss_enclosure(Span item, char* url, size_t sz, bool* isMp3);

void xml_unescape(char* s);
void copy_ascii(char* dst, size_t dstsz, const char* src); /* keeps UTF-8, drops control chars */

#endif
