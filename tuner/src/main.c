/* Tuner - internet radio, Apple Podcasts, BBC radio and IPTV audio for Playdate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "common.h"

PlaydateAPI* pd;

#define RB_BASE "https://de1.api.radio-browser.info/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=30&bitrateMax=192"
#define APPLE_CHARTS "https://rss.marketingtools.apple.com/api/v2/us/podcasts/top/25/podcasts.json"
#define RING_BYTES (160 * 1024)
#define PREBUFFER (48 * 1024)
#define MAX_ITEMS 40
#define ENC_MAX (URL_MAX * 3)

typedef enum { K_NONE, K_MENU, K_RADIO_Q, K_RADIO_SEARCH, K_POD_CHARTS, K_POD_SEARCH, K_POD_ID, K_POD_FEED, K_RELAY_LIST, K_RELAY_SEARCH, K_PLAY } Kind;
typedef struct { char label[80]; char arg[1100]; Kind kind; } Item;
typedef struct { char title[40]; Item* items; int n, sel, top; } List;
typedef enum { M_LIST, M_LOADING, M_MSG, M_TEXT, M_PLAY } Mode;
typedef void (*Parser)(Req* r);

static LCDFont* font;
static List* stack[8];
static int depth;
static Mode mode = M_LOADING;
static bool netAllowed, accessAsked;
static char relayHost[96];        /* "host:port" from Data/settings.json, empty if unset */
static char msg[200];

/* loading */
static Req* pending;
static Parser pendingParser;
static char pendingTitle[40];

/* text entry */
static char textBuf[28];
static int textLen;
static int charIdx;
static Kind textKind;
static const char* CHARS = " abcdefghijklmnopqrstuvwxyz0123456789";

/* playback */
static FilePlayer* player;
static Req* stream;
static bool started;
static uint32_t startedAt;
static char nowTitle[80];

static float crankAcc;

/* ---------- helpers ---------- */

static void* xalloc(size_t n) { return pd->system->realloc(NULL, n); }
static void xfree(void* p) { pd->system->realloc(p, 0); }

static void urlenc(const char* s, char* out, size_t sz)
{
	static const char* hex = "0123456789ABCDEF";
	size_t o = 0;
	for (; *s && o + 4 < sz; s++) {
		unsigned char c = (unsigned char)*s;
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.' || c == '~')
			out[o++] = (char)c;
		else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
	}
	out[o] = 0;
}

static void show_msg(const char* fmt, const char* a)
{
	snprintf(msg, sizeof msg, fmt, a ? a : "");
	mode = M_MSG;
}

static List* list_new(const char* title)
{
	List* l = xalloc(sizeof(List));
	memset(l, 0, sizeof *l);
	l->items = xalloc(sizeof(Item) * MAX_ITEMS);
	snprintf(l->title, sizeof l->title, "%s", title);
	return l;
}

static Item* list_add(List* l, const char* label, Kind k, const char* arg)
{
	if (l->n >= MAX_ITEMS) return NULL;
	Item* it = &l->items[l->n++];
	copy_ascii(it->label, sizeof it->label, label);
	snprintf(it->arg, sizeof it->arg, "%s", arg ? arg : "");
	it->kind = k;
	return it;
}

static void list_free(List* l) { xfree(l->items); xfree(l); }

static void push(List* l)
{
	if (depth < 8) stack[depth++] = l; else list_free(l);
	mode = M_LIST;
}

static void pop(void)
{
	if (depth > 1) list_free(stack[--depth]);
	mode = M_LIST;
}

static const char* relay_url(char* out, size_t sz, const char* path)
{
	snprintf(out, sz, "http://%s%s", relayHost, path);
	return out;
}

/* "relay:/t.mp3?u=<encoded url>" - the relay fetches (following any redirects) and transcodes to a small MP3 */
static void via_relay(char* out, size_t sz, const char* url)
{
	static char enc[ENC_MAX];
	urlenc(url, enc, sizeof enc);
	snprintf(out, sz, "relay:/t.mp3?u=%s", enc);
}

/* ---------- settings ---------- */

static void load_settings(void)
{
	SDFile* f = pd->file->open("settings.json", kFileReadData);
	if (!f) {
		f = pd->file->open("settings.json", kFileWrite);
		if (f) {
			const char* t = "{\n  \"relay\": \"\"\n}\n";
			pd->file->write(f, t, (unsigned)strlen(t));
			pd->file->close(f);
		}
		return;
	}
	char buf[512];
	int n = pd->file->read(f, buf, sizeof buf - 1);
	pd->file->close(f);
	if (n <= 0) return;
	buf[n] = 0;
	Span s = { buf, (size_t)n };
	json_get_str(s, "relay", relayHost, sizeof relayHost);
}

/* ---------- menus ---------- */

static void build_root(void)
{
	List* l = list_new("Tuner");
	list_add(l, "Radio", K_MENU, "radio");
	list_add(l, "Podcasts (Apple)", K_MENU, "pod");
	list_add(l, "BBC Radio", K_MENU, "bbc");
	list_add(l, "IPTV channels (audio)", K_RELAY_LIST, "/iptv/channels");
	list_add(l, "TuneIn radio", K_MENU, "tunein");
	push(l);
}

static void open_menu(const char* id)
{
	List* l;
	if (!strcmp(id, "radio")) {
		l = list_new("Radio");
		list_add(l, "Top stations", K_RADIO_Q, "");
		list_add(l, "Search...", K_RADIO_SEARCH, "");
		const char* tags[][2] = { {"Jazz","jazz"}, {"Classical","classical"}, {"News","news"}, {"Rock","rock"}, {"Pop","pop"},
			{"Electronic","electronic"}, {"Chillout","chillout"}, {"Talk","talk"}, {"Country","country"}, {"Hip hop","hiphop"} };
		for (int i = 0; i < 10; i++) { char a[64]; snprintf(a, sizeof a, "&tag=%s", tags[i][1]); list_add(l, tags[i][0], K_RADIO_Q, a); }
		list_add(l, "UK", K_RADIO_Q, "&countrycode=GB");
		list_add(l, "USA", K_RADIO_Q, "&countrycode=US");
	} else if (!strcmp(id, "pod")) {
		l = list_new("Apple Podcasts");
		list_add(l, "Top charts", K_POD_CHARTS, "");
		list_add(l, "Search...", K_POD_SEARCH, "");
	} else if (!strcmp(id, "bbc")) {
		l = list_new("BBC Radio");
		const char* st[][2] = { {"Radio 1","bbc_radio_one"}, {"Radio 2","bbc_radio_two"}, {"Radio 3","bbc_radio_three"},
			{"Radio 4","bbc_radio_fourfm"}, {"Radio 4 Extra","bbc_radio_four_extra"}, {"Radio 5 Live","bbc_radio_five_live"},
			{"6 Music","bbc_6music"}, {"World Service","bbc_world_service"}, {"Asian Network","bbc_asian_network"},
			{"Radio Scotland","bbc_radio_scotland_fm"}, {"Radio Wales","bbc_radio_wales_fm"}, {"Radio Ulster","bbc_radio_ulster"} };
		for (int i = 0; i < 12; i++) {
			char a[96], lab[64];
			snprintf(a, sizeof a, "relay:/bbc/%s.mp3", st[i][1]);
			snprintf(lab, sizeof lab, "BBC %s", st[i][0]);
			list_add(l, lab, K_PLAY, a);
		}
	} else {
		l = list_new("TuneIn");
		list_add(l, "Trending", K_RELAY_LIST, "/tunein/browse");
		list_add(l, "Search...", K_RELAY_SEARCH, "");
	}
	push(l);
}

/* ---------- async fetch + parsers ---------- */

static void on_fetched(Req* r) { (void)r; /* handled in update() */ }

static void start_fetch(const char* url, size_t max, Parser p, const char* title)
{
	if (pending) { net_free(pending); pending = NULL; }
	snprintf(pendingTitle, sizeof pendingTitle, "%s", title);
	pendingParser = p;
	pending = net_get(url, max, on_fetched, NULL);
	mode = M_LOADING;
	if (!pending) show_msg("Out of memory", NULL);
}

static void parse_radio(Req* r)
{
	const char* arr = json_array_start(r->buf, NULL);
	List* l = list_new(pendingTitle);
	const char* cur = arr; Span o;
	while (arr && l->n < MAX_ITEMS && json_next_object(&cur, r->buf + r->len, &o)) {
		char name[96], url[400], codec[16], lab[96];
		json_get_str(o, "name", name, sizeof name);
		json_get_str(o, "url_resolved", url, sizeof url);
		json_get_str(o, "codec", codec, sizeof codec);
		long br = json_get_int(o, "bitrate", 0);
		if (!name[0] || !url[0]) continue;
		bool mp3 = strcasecmp(codec, "MP3") == 0;
		char arg[1100];
		if (mp3) snprintf(arg, sizeof arg, "%s", url);
		else if (relayHost[0]) via_relay(arg, sizeof arg, url);
		else continue;
		snprintf(lab, sizeof lab, "%s%s", name, mp3 ? "" : " *");
		if (br > 0 && br < 100000 && strlen(lab) < 70) { char t[24]; snprintf(t, sizeof t, " %ldk", br); strcat(lab, t); }
		list_add(l, lab, K_PLAY, arg);
	}
	if (!l->n) { list_free(l); show_msg("No stations found.", NULL); return; }
	push(l);
}

static void parse_charts(Req* r)
{
	const char* arr = json_array_start(r->buf, "results");
	List* l = list_new(pendingTitle);
	const char* cur = arr; Span o;
	while (arr && l->n < MAX_ITEMS && json_next_object(&cur, r->buf + r->len, &o)) {
		char name[96], id[24];
		json_get_str(o, "name", name, sizeof name);
		json_get_str(o, "id", id, sizeof id);
		if (name[0] && id[0]) list_add(l, name, K_POD_ID, id);
	}
	if (!l->n) { list_free(l); show_msg("Couldn't read the charts.", NULL); return; }
	push(l);
}

static void parse_search(Req* r)
{
	const char* arr = json_array_start(r->buf, "results");
	List* l = list_new(pendingTitle);
	const char* cur = arr; Span o;
	while (arr && l->n < MAX_ITEMS && json_next_object(&cur, r->buf + r->len, &o)) {
		char name[96], feed[520];
		json_get_str(o, "collectionName", name, sizeof name);
		json_get_str(o, "feedUrl", feed, sizeof feed);
		if (name[0] && feed[0]) list_add(l, name, K_POD_FEED, feed);
	}
	if (!l->n) { list_free(l); show_msg("No podcasts found.", NULL); return; }
	push(l);
}

static void parse_episodes(Req* r)
{
	List* l = list_new(pendingTitle);
	const char* cur = r->buf; Span item;
	int skipped = 0;
	while (l->n < MAX_ITEMS && rss_next_item(&cur, &item)) {
		static char url[1000]; char title[96], arg[1100]; bool mp3;
		if (!rss_enclosure(item, url, sizeof url, &mp3)) continue;
		if (!mp3 && !relayHost[0]) { skipped++; continue; }
		if (!rss_tag_text(item, "title", title, sizeof title)) snprintf(title, sizeof title, "Episode");
		if (mp3) snprintf(arg, sizeof arg, "%s", url); else via_relay(arg, sizeof arg, url);
		list_add(l, title, K_PLAY, arg);
	}
	if (!l->n) {
		list_free(l);
		show_msg(skipped ? "This show only has AAC/M4A episodes, which the Playdate can't play." : "No episodes found.", NULL);
		return;
	}
	push(l);
}

static void parse_lookup(Req* r)
{
	const char* arr = json_array_start(r->buf, "results");
	const char* cur = arr; Span o; char feed[560] = "";
	if (arr && json_next_object(&cur, r->buf + r->len, &o)) json_get_str(o, "feedUrl", feed, sizeof feed);
	if (!feed[0]) { show_msg("No feed URL for this podcast.", NULL); return; }
	char title[40]; snprintf(title, sizeof title, "%s", pendingTitle);
	start_fetch(feed, 320 * 1024, parse_episodes, title);
}

static void parse_relay_list(Req* r)   /* [{"name": "...", "path": "/t.mp3?..."}] */
{
	const char* arr = json_array_start(r->buf, NULL);
	List* l = list_new(pendingTitle);
	const char* cur = arr; Span o;
	while (arr && l->n < MAX_ITEMS && json_next_object(&cur, r->buf + r->len, &o)) {
		char name[96], path[480], a[500];
		json_get_str(o, "name", name, sizeof name);
		json_get_str(o, "path", path, sizeof path);
		if (!name[0] || !path[0]) continue;
		snprintf(a, sizeof a, "relay:%s", path);
		list_add(l, name, K_PLAY, a);
	}
	if (!l->n) { list_free(l); show_msg("Nothing found.", NULL); return; }
	push(l);
}

/* ---------- playback ---------- */

static int data_source(uint8_t* data, int bytes, void* ud)
{
	Req* s = ud;
	return s ? ring_read(s, data, bytes) : 0;
}

static void stop_playback(void)
{
	if (player) { pd->sound->fileplayer->stop(player); pd->sound->fileplayer->freePlayer(player); player = NULL; }
	if (stream) { net_free(stream); stream = NULL; }
	started = false;
}

static char playTitle[80], playArg[1100];
static bool triedRelay;

static void start_playback(const char* title, const char* arg)
{
	static char url[URL_MAX];
	stop_playback();
	snprintf(playTitle, sizeof playTitle, "%s", title);
	snprintf(playArg, sizeof playArg, "%s", arg);
	if (!strncmp(arg, "relay:", 6)) {
		if (!relayHost[0]) { show_msg("Needs the relay: set \"relay\" in Data/settings.json (see README).", NULL); return; }
		relay_url(url, sizeof url, arg + 6);
	} else snprintf(url, sizeof url, "%s", arg);
	copy_ascii(nowTitle, sizeof nowTitle, title);
	stream = net_stream(url, RING_BYTES);
	if (!stream) { show_msg("Out of memory", NULL); return; }
	player = pd->sound->fileplayer->newPlayer();
	pd->sound->fileplayer->setStopOnUnderrun(player, 0);
	pd->sound->fileplayer->setMP3StreamSource(player, data_source, stream, 2.0f);
	mode = M_PLAY;
}

/* Some hosts redirect in ways the Playdate's HTTP client can't follow (very long Location headers, for one).
   If a direct stream fails and a relay is configured, retry it through the relay. */
static void maybe_fall_back(void)
{
	if (mode != M_PLAY || !stream || stream->status >= 0 || triedRelay || !relayHost[0] || !strncmp(playArg, "relay:", 6)) return;
	triedRelay = true;
	static char arg[1100];
	via_relay(arg, sizeof arg, playArg);
	char t[80]; snprintf(t, sizeof t, "%s", playTitle);
	start_playback(t, arg);
}

/* ---------- actions ---------- */

static void activate(Item* it)
{
	static char url[URL_MAX];
	switch (it->kind) {
		case K_MENU: open_menu(it->arg); break;
		case K_RADIO_Q:
			snprintf(url, sizeof url, "%s%s%s", RB_BASE, relayHost[0] ? "" : "&codec=MP3", it->arg);
			start_fetch(url, 160 * 1024, parse_radio, it->label); break;
		case K_RADIO_SEARCH: case K_POD_SEARCH: case K_RELAY_SEARCH:
			textKind = it->kind; textLen = 0; charIdx = 1; mode = M_TEXT; break;
		case K_POD_CHARTS: start_fetch(APPLE_CHARTS, 96 * 1024, parse_charts, "Top podcasts"); break;
		case K_POD_ID:
			snprintf(url, sizeof url, "https://itunes.apple.com/lookup?id=%s", it->arg);
			start_fetch(url, 64 * 1024, parse_lookup, it->label); break;
		case K_POD_FEED: start_fetch(it->arg, 320 * 1024, parse_episodes, it->label); break;
		case K_RELAY_LIST:
			if (!relayHost[0]) { show_msg("Needs the relay: set \"relay\" in Data/settings.json (see README).", NULL); break; }
			relay_url(url, sizeof url, it->arg);
			start_fetch(url, 64 * 1024, parse_relay_list, it->label); break;
		case K_PLAY: triedRelay = false; start_playback(it->label, it->arg); break;
		default: break;
	}
}

static void submit_text(void)
{
	static char url[800]; char q[64], enc[200];
	if (textLen == 0) { mode = M_LIST; return; }
	textBuf[textLen] = 0;
	urlenc(textBuf, enc, sizeof enc);
	snprintf(q, sizeof q, "%s", textBuf);
	if (textKind == K_RADIO_SEARCH) {
		snprintf(url, sizeof url, "%s%s&name=%s", RB_BASE, relayHost[0] ? "" : "&codec=MP3", enc);
		start_fetch(url, 160 * 1024, parse_radio, q);
	} else if (textKind == K_POD_SEARCH) {
		snprintf(url, sizeof url, "https://itunes.apple.com/search?media=podcast&limit=25&term=%s", enc);
		start_fetch(url, 128 * 1024, parse_search, q);
	} else {
		if (!relayHost[0]) { show_msg("Needs the relay: set \"relay\" in Data/settings.json (see README).", NULL); return; }
		char path[300]; snprintf(path, sizeof path, "/tunein/search?q=%s", enc);
		relay_url(url, sizeof url, path);
		start_fetch(url, 64 * 1024, parse_relay_list, q);
	}
}

/* ---------- drawing ---------- */

static void text(const char* s, int x, int y, LCDBitmapDrawMode m)
{
	pd->graphics->setDrawMode(m);
	pd->graphics->drawText(s, strlen(s), kUTF8Encoding, x, y);
}

static void draw_header(const char* title)
{
	pd->graphics->fillRect(0, 0, 400, 22, kColorBlack);
	text(title, 6, 3, kDrawModeFillWhite);
}

static void draw_footer(const char* hint)
{
	pd->graphics->drawLine(0, 220, 400, 220, 1, kColorBlack);
	text(hint, 6, 223, kDrawModeFillBlack);
}

static void wrap_text(const char* s, int x, int y, int cols, int lineH)
{
	char line[64];
	while (*s) {
		int n = (int)strlen(s) < cols ? (int)strlen(s) : cols;
		if ((int)strlen(s) > cols) { int k = n; while (k > 0 && s[k] != ' ') k--; if (k > 0) n = k; }
		snprintf(line, sizeof line, "%.*s", n, s);
		text(line, x, y, kDrawModeFillBlack);
		s += n; while (*s == ' ') s++;
		y += lineH;
	}
}

static void draw_list(List* l)
{
	draw_header(l->title);
	for (int i = 0; i < 9; i++) {
		int idx = l->top + i;
		if (idx >= l->n) break;
		int y = 24 + i * 21;
		char lab[96]; snprintf(lab, sizeof lab, "%.58s", l->items[idx].label);
		if (idx == l->sel) { pd->graphics->fillRect(0, y, 400, 21, kColorBlack); text(lab, 8, y + 3, kDrawModeFillWhite); }
		else text(lab, 8, y + 3, kDrawModeFillBlack);
	}
	if (l->n > 9) {                      /* scrollbar */
		int h = 190 * 9 / l->n; int y = 24 + (190 - h) * l->top / (l->n - 9);
		pd->graphics->fillRect(396, y, 4, h, kColorBlack);
	}
	draw_footer(depth > 1 ? "A select   B back   crank scrolls" : "A select   crank scrolls");
}

static void draw_play(void)
{
	draw_header("Now playing");
	wrap_text(nowTitle, 10, 34, 44, 20);
	char st[96];
	if (!stream) snprintf(st, sizeof st, "Stopped");
	else if (stream->status < 0) snprintf(st, sizeof st, "Error: %s%s", stream->error, relayHost[0] ? "" : " (a relay may help, see README)");
	else if (stream->total == 0) snprintf(st, sizeof st, "Connecting...");
	else if (!started) snprintf(st, sizeof st, "Buffering %u%%", (unsigned)(ring_fill(stream) * 100 / PREBUFFER));
	else {
		int t = (int)((pd->system->getCurrentTimeMilliseconds() - startedAt) / 1000);
		snprintf(st, sizeof st, "Playing %d:%02d  (%u KB received)", t / 60, t % 60, (unsigned)stream->total / 1024);
	}
	text(st, 10, 120, kDrawModeFillBlack);
	if (stream) {
		pd->graphics->drawRect(10, 150, 380, 14, kColorBlack);
		int w = (int)((uint64_t)ring_fill(stream) * 376 / stream->rsize);
		pd->graphics->fillRect(12, 152, w, 10, kColorBlack);
		text("buffer", 10, 168, kDrawModeFillBlack);
	}
	draw_footer("B stop & back");
}

static void draw_text_entry(void)
{
	draw_header(textKind == K_POD_SEARCH ? "Search podcasts" : textKind == K_RELAY_SEARCH ? "Search TuneIn" : "Search stations");
	char shown[40];
	snprintf(shown, sizeof shown, "%.*s[%c]", textLen, textBuf, CHARS[charIdx] == ' ' ? '_' : CHARS[charIdx]);
	text(shown, 20, 80, kDrawModeFillBlack);
	draw_footer("Up/Dn letter  R add  L del  A go  B back");
}

/* ---------- main loop ---------- */

static void on_access(bool allowed, void* ud) { (void)ud; netAllowed = allowed; if (!allowed) show_msg("Network access was denied.", NULL); }

static int update(void* ud)
{
	(void)ud;
	PDButtons cur, pushed, released;
	pd->system->getButtonState(&cur, &pushed, &released);
	pd->graphics->clear(kColorWhite);
	pd->graphics->setFont(font);

	if (!accessAsked) {
		accessAsked = true;
		enum accessReply a = pd->network->http->requestAccess(NULL, 0, true, "to stream radio and podcasts", on_access, NULL);
		if (a == kAccessAllow) netAllowed = true;
		else if (a == kAccessDeny) show_msg("Network access was denied.", NULL);
	}
	if (netAllowed && depth == 0) build_root();

	net_update();
	maybe_fall_back();

	/* finished list fetch */
	if (pending && pending->finished) {
		Req* r = pending; pending = NULL;
		Parser p = pendingParser;
		if (r->status < 0 || r->status >= 400) { char m[700]; snprintf(m, sizeof m, "%s [%.80s]", r->error[0] ? r->error : "request failed", r->url); show_msg("Couldn't load: %s", m); }
		else if (r->len == 0) show_msg("The server sent nothing.", NULL);
		else p(r);
		net_free(r);
	}

	/* streaming playback control */
	if (stream && !started && stream->status >= 0 && ring_fill(stream) >= PREBUFFER) {
		started = true; startedAt = pd->system->getCurrentTimeMilliseconds();
		pd->sound->fileplayer->play(player, 1);
		pd->system->logToConsole("playback started, ring=%u", (unsigned)ring_fill(stream));
	}

	switch (mode) {
		case M_LOADING:
			if (!netAllowed) { draw_header("Tuner"); text("Waiting for network permission...", 10, 100, kDrawModeFillBlack); }
			else {
				draw_header(pendingTitle[0] ? pendingTitle : "Tuner");
				char dots[24]; snprintf(dots, sizeof dots, "Loading%.*s", (int)((pd->system->getCurrentTimeMilliseconds() / 400) % 4), "...");
				text(dots, 10, 100, kDrawModeFillBlack);
				draw_footer("B cancel");
				if (pushed & kButtonB) { if (pending) { net_free(pending); pending = NULL; } mode = M_LIST; }
			}
			break;
		case M_MSG:
			draw_header("Tuner");
			wrap_text(msg, 10, 60, 44, 20);
			draw_footer("A / B back");
			if (pushed & (kButtonA | kButtonB)) mode = depth ? M_LIST : M_LOADING;
			break;
		case M_TEXT: {
			draw_text_entry();
			int n = (int)strlen(CHARS);
			if (pushed & kButtonUp) charIdx = (charIdx + 1) % n;
			if (pushed & kButtonDown) charIdx = (charIdx + n - 1) % n;
			float c = pd->system->getCrankChange(); crankAcc += c;
			while (crankAcc > 20) { charIdx = (charIdx + 1) % n; crankAcc -= 20; }
			while (crankAcc < -20) { charIdx = (charIdx + n - 1) % n; crankAcc += 20; }
			if ((pushed & kButtonRight) && textLen < (int)sizeof textBuf - 2) textBuf[textLen++] = CHARS[charIdx];
			if ((pushed & kButtonLeft) && textLen > 0) textLen--;
			if (pushed & kButtonA) { if (textLen < (int)sizeof textBuf - 2 && CHARS[charIdx] != ' ') textBuf[textLen++] = CHARS[charIdx]; submit_text(); }
			if (pushed & kButtonB) mode = M_LIST;
			break;
		}
		case M_PLAY:
			draw_play();
			if (pushed & kButtonB) { stop_playback(); mode = M_LIST; }
			break;
		case M_LIST: {
			if (!depth) break;
			List* l = stack[depth - 1];
			float c = pd->system->getCrankChange(); crankAcc += c;
			while (crankAcc > 25) { if (l->sel < l->n - 1) l->sel++; crankAcc -= 25; }
			while (crankAcc < -25) { if (l->sel > 0) l->sel--; crankAcc += 25; }
			if ((pushed & kButtonDown) && l->sel < l->n - 1) l->sel++;
			if ((pushed & kButtonUp) && l->sel > 0) l->sel--;
			if (l->sel < l->top) l->top = l->sel;
			if (l->sel > l->top + 8) l->top = l->sel - 8;
			draw_list(l);
			if ((pushed & kButtonA) && l->n) activate(&l->items[l->sel]);
			if ((pushed & kButtonB) && depth > 1) pop();
			break;
		}
	}
	return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg)
{
	(void)arg;
	if (event == kEventInit) {
		pd = playdate;
		const char* err;
		font = pd->graphics->loadFont("/System/Fonts/Asheville-Sans-14-Light.pft", &err);
		load_settings();
		pd->network->setEnabled(true, NULL);
		pd->system->setUpdateCallback(update, NULL);
	} else if (event == kEventTerminate || event == kEventLock) {
		if (event == kEventTerminate) stop_playback();
	}
	return 0;
}
