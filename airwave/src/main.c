/* Airwave - internet radio, Apple Podcasts, BBC, TuneIn, TV (dithered video) and an AI chat for Playdate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include "common.h"

PlaydateAPI* pd;

#define RB_BASE "https://de1.api.radio-browser.info/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=30&bitrateMax=192"
#define APPLE_CHARTS "https://rss.marketingtools.apple.com/api/v2/us/podcasts/top/25/podcasts.json"
#define POLLINATIONS "https://text.pollinations.ai/openai"
#define RING_BYTES (160 * 1024)
#define PREBUFFER (48 * 1024)
#define MAX_ITEMS 40
#define VIDEO_RING (192 * 1024)
#define VIDEO_AUDIO_RING (48 * 1024)
#define VIDEO_AUDIO_START (9 * 1024)   /* ~1.5 s of the relay's 48 kbps audio */
#define AV_DELAY_MS 1500               /* video is shown this long after it arrives, to line up with the audio prebuffer */
#define VQ_MAX 96
#define FRAME_BYTES (LCD_ROWSIZE * 240)
#define ENC_MAX (URL_MAX * 3)

typedef enum { K_NONE, K_MENU, K_RADIO_Q, K_SEARCH, K_SEARCH_ALL, K_SEARCH_IN, K_POD_CHARTS, K_POD_ID, K_POD_FEED, K_RELAY_LIST, K_PLAY, K_PLAY_VIDEO } Kind;
typedef struct { char label[80]; char arg[1100]; Kind kind; } Item;
typedef struct { char title[40]; Item* items; int n, sel, top; } List;
typedef enum { M_SPLASH, M_HOME, M_LIST, M_LOADING, M_MSG, M_TEXT, M_PLAY, M_VIDEO, M_CHAT } Mode;
typedef void (*Parser)(Req* r);

static LCDFont *fBody, *fBold, *fBig, *fTitle;
static List* stack[8];
static int depth;
static Mode mode = M_SPLASH;
static bool netAllowed, accessAsked;
static char relayToken[48];       /* optional "relay_token", sent as ?k= (the relay's --token) */
static char relayHost[128];       /* "host:port" or a full http(s):// URL (e.g. a Cloudflare tunnel) from Data/settings.json */
static char msg[400];
static float crankAcc;
static int homeSel;

/* loading */
static Req* pending;
static Parser pendingParser;
static char pendingTitle[40];

/* text entry (on-screen keyboard) */
static char textBuf[128];
static int textLen, textMax;
static char textSource[16];       /* "radio" "pod" "tunein" "iptv" "yt" "ia" "all" "chat" */
static bool kbWant, kbActive, kbVisible;   /* system keyboard (Lua): wanted / in use / currently on screen */
static int kbCaps;
static int viewW = 400;                    /* width left of the keyboard while it slides in; the whole UI lays out in this */
static char searchQuery[28];

/* playback */
static FilePlayer* player;
static Req* stream;
static bool started;
static uint32_t startedAt;
static char nowTitle[80];

/* ---------- helpers ---------- */

/* Opt-in breadcrumbs: set "debug": true in settings.json and the app appends what it is doing to Data/<bundle>/airwave.log, so a
   hardware crash can be located (the last lines show the last thing that happened). */
static bool debugOn;
static void dbg(const char* fmt, ...)
{
	if (!debugOn) return;
	static char line[200];
	va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
	SDFile* f = pd->file->open("airwave.log", kFileAppend);
	if (!f) return;
	pd->file->write(f, line, (unsigned)strlen(line)); pd->file->write(f, "\n", 1);
	pd->file->close(f);
}


static void* xalloc(size_t n) { return pd->system->realloc(NULL, n); }
static void xfree(void* p) { if (p) pd->system->realloc(p, 0); }
static uint32_t now_ms(void) { return pd->system->getCurrentTimeMilliseconds(); }

static void T(LCDFont* f, const char* s, int x, int y, LCDBitmapDrawMode m)
{
	if (f) pd->graphics->setFont(f);
	pd->graphics->setDrawMode(m);
	pd->graphics->drawText(s, strlen(s), kUTF8Encoding, x, y);
}
static int TW(LCDFont* f, const char* s) { return f ? pd->graphics->getTextWidth(f, s, strlen(s), kUTF8Encoding, 0) : (int)strlen(s) * 7; }
static void Tc(LCDFont* f, const char* s, int cx, int y, LCDBitmapDrawMode m) { T(f, s, cx - TW(f, s) / 2, y, m); }

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

static List deadList;                                   /* stand-in when memory runs out */

static List* list_new(const char* title)
{
	List* l = xalloc(sizeof(List));
	if (!l) { dbg("list_new: out of memory"); memset(&deadList, 0, sizeof deadList); return &deadList; }   /* empty, never NULL */
	memset(l, 0, sizeof *l);
	l->items = xalloc(sizeof(Item) * MAX_ITEMS);
	if (!l->items) dbg("list_new: out of memory (items)");
	snprintf(l->title, sizeof l->title, "%s", title);
	return l;
}

static Item* list_add(List* l, const char* label, Kind k, const char* arg)
{
	if (!l->items || l->n >= MAX_ITEMS) return NULL;
	Item* it = &l->items[l->n++];
	copy_ascii(it->label, sizeof it->label, label);
	snprintf(it->arg, sizeof it->arg, "%s", arg ? arg : "");
	it->kind = k;
	return it;
}

static void list_free(List* l) { if (l == &deadList) return; xfree(l->items); xfree(l); }

static void push(List* l)
{
	if (depth < 8) stack[depth++] = l; else list_free(l);
	mode = M_LIST;
}

static void pop(void)
{
	if (depth > 0) list_free(stack[--depth]);
	mode = depth ? M_LIST : M_HOME;
}

static const char* relay_url(char* out, size_t sz, const char* path)
{
	if (!strncmp(relayHost, "http", 4)) snprintf(out, sz, "%s%s", relayHost, path);   /* full URL, e.g. https://x.trycloudflare.com */
	else snprintf(out, sz, "http://%s%s", relayHost, path);
	if (relayToken[0]) {
		size_t n = strlen(out);
		snprintf(out + n, sz - n, "%ck=%s", strchr(path, '?') ? '&' : '?', relayToken);
	}
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
			const char* t = "{\n  \"relay\": \"\",\n  \"relay_token\": \"\"\n}\n";
			pd->file->write(f, t, (unsigned)strlen(t));
			pd->file->close(f);
		}
		return;
	}
	static char buf[512];
	int n = pd->file->read(f, buf, sizeof buf - 1);
	pd->file->close(f);
	if (n <= 0) return;
	buf[n] = 0;
	Span s = { buf, (size_t)n };
	json_get_str(s, "relay", relayHost, sizeof relayHost);
	json_get_str(s, "relay_token", relayToken, sizeof relayToken);
	char dbgv[16]; json_get_str(s, "debug", dbgv, sizeof dbgv);
	debugOn = strstr(buf, "\"debug\": true") || strstr(buf, "\"debug\":true");
}

/* ---------- menus ---------- */

static void open_menu(const char* id)
{
	List* l;
	if (!strcmp(id, "radio")) {
		l = list_new("Radio");
		list_add(l, "Top stations", K_RADIO_Q, "");
		list_add(l, "Search...", K_SEARCH, "radio");
		const char* tags[][2] = { {"Jazz","jazz"}, {"Classical","classical"}, {"News","news"}, {"Rock","rock"}, {"Pop","pop"},
			{"Electronic","electronic"}, {"Chillout","chillout"}, {"Talk","talk"}, {"Country","country"}, {"Hip hop","hiphop"} };
		for (int i = 0; i < 10; i++) { char a[64]; snprintf(a, sizeof a, "&tag=%s", tags[i][1]); list_add(l, tags[i][0], K_RADIO_Q, a); }
		list_add(l, "UK", K_RADIO_Q, "&countrycode=GB");
		list_add(l, "USA", K_RADIO_Q, "&countrycode=US");
	} else if (!strcmp(id, "pod")) {
		l = list_new("Apple Podcasts");
		list_add(l, "Top charts", K_POD_CHARTS, "");
		list_add(l, "Search...", K_SEARCH, "pod");
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
	} else if (!strcmp(id, "tv")) {
		l = list_new("TV & Shows");
		list_add(l, "Live TV channels (Pluto TV)", K_RELAY_LIST, "/pluto/categories");
		list_add(l, "Documentary channels (YouTube)", K_RELAY_LIST, "/tv/shows");
		list_add(l, "YouTube Shorts", K_MENU, "shorts");
		list_add(l, "Free live news", K_RELAY_LIST, "/tv/free");
		list_add(l, "My IPTV channels", K_MENU, "iptv");
		list_add(l, "Internet Archive (classic TV...)", K_MENU, "archive");
		list_add(l, "Search YouTube...", K_SEARCH, "yt");
		list_add(l, "Search live channels...", K_SEARCH, "pluto");
	} else if (!strcmp(id, "shorts")) {
		l = list_new("YouTube Shorts");
		list_add(l, "Funny", K_RELAY_LIST, "/yt/shorts?q=funny");
		list_add(l, "Funny animals", K_RELAY_LIST, "/yt/shorts?q=funnyanimals");
		list_add(l, "Cats", K_RELAY_LIST, "/yt/shorts?q=cats");
		list_add(l, "Fails", K_RELAY_LIST, "/yt/shorts?q=fails");
		list_add(l, "Wildlife", K_RELAY_LIST, "/yt/shorts?q=wildlife");
		list_add(l, "Nat Geo", K_RELAY_LIST, "/yt/shorts?q=%40NatGeo");
		list_add(l, "Search shorts...", K_SEARCH, "shorts");
	} else if (!strcmp(id, "iptv")) {
		l = list_new("My IPTV");
		list_add(l, "Search channels...", K_SEARCH, "iptv");
		list_add(l, "Groups", K_RELAY_LIST, "/iptv/groups");
		list_add(l, "All channels", K_RELAY_LIST, "/iptv/channels");
	} else if (!strcmp(id, "archive")) {
		l = list_new("Internet Archive");
		list_add(l, "Search...", K_SEARCH, "ia");
		list_add(l, "Classic TV shows", K_RELAY_LIST, "/ia/search?q=collection%3Aclassic_tv");
		list_add(l, "Classic cartoons", K_RELAY_LIST, "/ia/search?q=collection%3Aclassic_cartoons");
		list_add(l, "NASA", K_RELAY_LIST, "/ia/search?q=collection%3Anasa");
		list_add(l, "Vintage films (Prelinger)", K_RELAY_LIST, "/ia/search?q=collection%3Aprelinger");
		list_add(l, "Feature films", K_RELAY_LIST, "/ia/search?q=collection%3Afeature_films");
	} else {
		l = list_new("TuneIn");
		list_add(l, "Trending", K_RELAY_LIST, "/tunein/browse");
		list_add(l, "Search...", K_SEARCH, "tunein");
	}
	push(l);
}

/* ---------- async fetch + parsers ---------- */

static void on_fetched(Req* r) { (void)r; /* handled in update() */ }

static void start_fetch(const char* url, size_t max, Parser p, const char* title)
{
	dbg("fetch %.100s", url);
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
		static char name[96], url[400], codec[16], lab[96], arg[1100];
		json_get_str(o, "name", name, sizeof name);
		json_get_str(o, "url_resolved", url, sizeof url);
		json_get_str(o, "codec", codec, sizeof codec);
		long br = json_get_int(o, "bitrate", 0);
		if (!name[0] || !url[0]) continue;
		bool mp3 = strcasecmp(codec, "MP3") == 0;
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
		static char name[96], id[24];
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
		static char name[96], feed[520];
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
		static char url[1000], title[96], arg[1100]; bool mp3;
		if (!rss_enclosure(item, url, sizeof url, &mp3)) continue;
		if (!mp3 && !relayHost[0]) { skipped++; continue; }
		if (!rss_tag_text(item, "title", title, sizeof title)) snprintf(title, sizeof title, "Episode");
		if (mp3) snprintf(arg, sizeof arg, "%s", url); else via_relay(arg, sizeof arg, url);
		list_add(l, title, K_PLAY, arg);
	}
	if (!l->n) {
		list_free(l);
		show_msg(skipped ? "This show only has AAC/M4A episodes, which the Playdate can't play (a relay can convert them)." : "No episodes found.", NULL);
		return;
	}
	push(l);
}

static void parse_lookup(Req* r)
{
	const char* arr = json_array_start(r->buf, "results");
	const char* cur = arr; Span o; static char feed[560]; feed[0] = 0;
	if (arr && json_next_object(&cur, r->buf + r->len, &o)) json_get_str(o, "feedUrl", feed, sizeof feed);
	if (!feed[0]) { show_msg("No feed URL for this podcast.", NULL); return; }
	char title[40]; snprintf(title, sizeof title, "%s", pendingTitle);
	start_fetch(feed, 320 * 1024, parse_episodes, title);
}

/* relay list: [{"name", "path", "video"?, "kind"?}] - "kind":"list" opens another list, "video" marks a TV item */
static void parse_relay_list(Req* r)
{
	const char* arr = json_array_start(r->buf, NULL);
	List* l = list_new(pendingTitle);
	const char* cur = arr; Span o;
	while (arr && l->n < MAX_ITEMS && json_next_object(&cur, r->buf + r->len, &o)) {
		static char name[120], path[480], video[200], kind[16], a[500];
		json_get_str(o, "name", name, sizeof name);
		json_get_str(o, "path", path, sizeof path);
		json_get_str(o, "video", video, sizeof video);
		json_get_str(o, "kind", kind, sizeof kind);
		if (!name[0] || !path[0]) continue;
		if (!strcmp(kind, "list")) { list_add(l, name, K_RELAY_LIST, path); continue; }
		if (video[0]) { snprintf(a, sizeof a, "relay:%s", video); list_add(l, name, K_PLAY_VIDEO, a); }
		else { snprintf(a, sizeof a, "relay:%s", path); list_add(l, name, K_PLAY, a); }
	}
	if (!l->n) { list_free(l); show_msg("Nothing found.", NULL); return; }
	push(l);
}

/* ---------- playback ---------- */

static Ring audioRing;                 /* video mode: demuxed MP3 for the audio thread */
static bool videoActive;
typedef struct { uint32_t pts; bool key; uint32_t len; uint8_t* data; } VFrame;
static VFrame vq[VQ_MAX];
static int vqHead, vqCount;
static uint8_t frameBuf[FRAME_BYTES];  /* the dithered picture, row stride LCD_ROWSIZE */
static bool vHaveT0, vGotFrame, videoFellBack;
static uint32_t vT0;

static int data_source(uint8_t* data, int bytes, void* ud)
{
	Ring* g = ud;
	return g ? ring_read(g, data, bytes) : 0;
}

static void video_reset(void)
{
	for (int i = 0; i < vqCount; i++) xfree(vq[(vqHead + i) % VQ_MAX].data);
	vqHead = vqCount = 0;
	vHaveT0 = vGotFrame = false;
	memset(frameBuf, 0, sizeof frameBuf);
	ring_free(&audioRing);
	videoActive = false;
}

static void stop_playback(void)
{
	if (player) { pd->sound->fileplayer->stop(player); pd->sound->fileplayer->freePlayer(player); player = NULL; }
	if (stream) { net_free(stream); stream = NULL; }
	video_reset();
	started = false;
}

static char playTitle[80], playArg[1100];
static List* playList;                 /* the list the current item came from: Up/Down in the player = next/previous */
static uint32_t overlayUntil;          /* channel-name banner; armed when the first picture appears */
static bool overlayDrawn, overlayArmed;
static bool triedRelay;

static void start_playback(const char* title, const char* arg, bool video)
{
	static char url[URL_MAX];
	stop_playback();
	snprintf(playTitle, sizeof playTitle, "%s", title);
	snprintf(playArg, sizeof playArg, "%s", arg);
	if (!strncmp(arg, "relay:", 6)) {
		if (!relayHost[0]) { show_msg("This needs the relay. Put its address in Data/settings.json (see the README).", NULL); return; }
		relay_url(url, sizeof url, arg + 6);
	} else snprintf(url, sizeof url, "%s", arg);
	copy_ascii(nowTitle, sizeof nowTitle, title);
	dbg("play %s %.100s", video ? "video" : "audio", url);
	stream = net_stream(url, video ? VIDEO_RING : RING_BYTES);
	if (!stream) { show_msg("Out of memory", NULL); return; }
	Ring* src = &stream->ring;
	if (video) {
		if (!ring_init(&audioRing, VIDEO_AUDIO_RING)) { stop_playback(); show_msg("Out of memory", NULL); return; }
		videoActive = true;
		src = &audioRing;
	}
	player = pd->sound->fileplayer->newPlayer();
	if (!player) { stop_playback(); show_msg("Couldn't start the audio player.", NULL); return; }
	pd->sound->fileplayer->setStopOnUnderrun(player, 0);
	pd->sound->fileplayer->setMP3StreamSource(player, data_source, src, 2.0f);
	mode = video ? M_VIDEO : M_PLAY;
	overlayUntil = 0; overlayDrawn = false; overlayArmed = false;
}

/* "/iptv/video/7" -> "/iptv/audio/7" (every relay source has both) */
static void audio_arg(const char* videoArg, char* out, size_t sz)
{
	const char* v = strstr(videoArg, "/video/");
	if (!v) { snprintf(out, sz, "%s", videoArg); return; }
	snprintf(out, sz, "%.*s/audio/%s", (int)(v - videoArg), videoArg, v + 7);
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
	start_playback(t, arg, false);
}

/* ---- video: demux the relay's packet stream, then show frames on schedule ---- */

static void video_demux(uint32_t now)
{
	static uint8_t tmp[1024];
	for (int guard = 0; guard < 64; guard++) {
		uint8_t h[3];
		if (ring_peek(&stream->ring, 0, h, 3) < 3) break;
		uint32_t len = h[1] | ((uint32_t)h[2] << 8);
		if (ring_fill(&stream->ring) < 3 + len) break;
		if (!vHaveT0) { vHaveT0 = true; vT0 = now; }
		if (h[0] == 1) {                                        /* MP3 audio */
			if (ring_space(&audioRing) < len) break;
			ring_read(&stream->ring, NULL, 3);
			while (len) { uint32_t n = len < sizeof tmp ? len : sizeof tmp; ring_read(&stream->ring, tmp, (int)n); ring_put(&audioRing, tmp, n); len -= n; }
		} else if (h[0] == 2 && len >= 5) {                     /* video frame: pts:u32 flags:u8 packbits */
			if (vqCount >= VQ_MAX) break;
			uint8_t pre[5];
			ring_read(&stream->ring, NULL, 3);
			ring_read(&stream->ring, pre, 5);
			VFrame* f = &vq[(vqHead + vqCount) % VQ_MAX];
			f->pts = pre[0] | (pre[1] << 8) | (pre[2] << 16) | ((uint32_t)pre[3] << 24);
			f->key = pre[4] & 1;
			f->len = len - 5;
			f->data = xalloc(f->len ? f->len : 1);
			ring_read(&stream->ring, f->data, (int)f->len);
			vqCount++;
		} else ring_read(&stream->ring, NULL, (int)(3 + len));   /* unknown packet: skip */
	}
}

static void apply_frame(const VFrame* f)
{
	size_t o = 0, i = 0;
	if (f->key) memset(frameBuf, 0, sizeof frameBuf);
	while (i < f->len && o < FRAME_BYTES) {                      /* PackBits, XORed onto the previous picture */
		uint8_t c = f->data[i++];
		if (c < 128) {
			size_t n = (size_t)c + 1;
			if (i + n > f->len || o + n > FRAME_BYTES) return;
			for (size_t k = 0; k < n; k++) frameBuf[o++] ^= f->data[i++];
		} else if (c > 128) {
			size_t n = 257 - (size_t)c;
			if (i >= f->len || o + n > FRAME_BYTES) return;
			uint8_t v = f->data[i++];
			if (v) for (size_t k = 0; k < n; k++) frameBuf[o + k] ^= v;
			o += n;
		}
	}
}

static void video_show(uint32_t now)
{
	bool changed = false;
	while (vqCount) {
		VFrame* f = &vq[vqHead];
		if (now < vT0 + f->pts + AV_DELAY_MS) break;            /* not due yet */
		apply_frame(f);                                          /* late frames are still applied so deltas stay correct */
		xfree(f->data);
		vqHead = (vqHead + 1) % VQ_MAX; vqCount--;
		changed = vGotFrame = true;
	}
	if (vGotFrame && !overlayArmed) { overlayArmed = true; overlayUntil = now + 3500; }
	bool overlay = vGotFrame && now < overlayUntil;
	if (changed || overlay || overlayDrawn) {
		uint8_t* fb = pd->graphics->getFrame();
		if (!fb) return;
		memcpy(fb, frameBuf, FRAME_BYTES);
		if (overlay) {                                           /* channel name for a few seconds after tuning in */
			pd->graphics->fillRect(0, 0, 400, 22, kColorBlack);
			T(fBold, nowTitle, 6, 3, kDrawModeFillWhite);
			if (playList) T(fBody, "Up/Down: next", 394 - TW(fBody, "Up/Down: next"), 3, kDrawModeFillWhite);
		}
		overlayDrawn = overlay;
		pd->graphics->markUpdatedRows(0, (changed || !overlay) ? 239 : 24);
	}
}

/* ---------- AI chat (pollinations.ai, no key) ---------- */

#define CHAT_MAX 10
#define CHAT_LINES 300
static char chatText[CHAT_MAX][900];
static bool chatUser[CHAT_MAX];
static int chatN;
static Req* chatReq;
static char chatLines[CHAT_LINES][80];
static uint8_t chatStyle[CHAT_LINES];     /* 0 = assistant, 1 = you, 2 = notice */
static int chatLineN, chatScroll;
static bool chatDirty = true;
static int chatWrapW = 372, chatWrapBuilt = 372;

static void chat_add(bool user, const char* text)
{
	if (chatN == CHAT_MAX) {                                     /* drop the oldest */
		memmove(chatText[0], chatText[1], sizeof chatText[0] * (CHAT_MAX - 1));
		memmove(&chatUser[0], &chatUser[1], CHAT_MAX - 1);
		chatN--;
	}
	snprintf(chatText[chatN], sizeof chatText[0], "%s", text);
	chatUser[chatN++] = user;
	chatDirty = true;
}

static void chat_add_line(const char* s, size_t n, uint8_t style)
{
	if (chatLineN >= CHAT_LINES) { memmove(chatLines[0], chatLines[1], sizeof chatLines[0] * (CHAT_LINES - 1)); memmove(chatStyle, chatStyle + 1, CHAT_LINES - 1); chatLineN--; }
	snprintf(chatLines[chatLineN], sizeof chatLines[0], "%.*s", (int)n, s);
	chatStyle[chatLineN++] = style;
}

/* greedy word wrap by pixel width */
static void chat_wrap(const char* text, uint8_t style, const char* prefix)
{
	const int maxw = chatWrapW;
	static char line[80]; line[0] = 0;
	size_t pl = strlen(prefix);
	bool first = true;
	snprintf(line, sizeof line, "%s", prefix);
	const char* p = text;
	while (*p) {
		if (*p == '\n') { chat_add_line(line, strlen(line), style); snprintf(line, sizeof line, "%s", first ? "" : ""); line[0] = 0; first = false; p++; continue; }
		const char* e = p; while (*e && *e != ' ' && *e != '\n') e++;
		static char word[80]; size_t wl = (size_t)(e - p); if (wl > 60) wl = 60;
		snprintf(word, sizeof word, "%.*s", (int)wl, p);
		static char trial[160]; snprintf(trial, sizeof trial, "%s%s%s", line, (line[0] && strcmp(line, prefix)) ? " " : "", word);
		if (TW(fBody, trial) > maxw && line[0] && strcmp(line, prefix)) { chat_add_line(line, strlen(line), style); snprintf(line, sizeof line, "%s", word); first = false; }
		else snprintf(line, sizeof line, "%.78s", trial);
		p = e; while (*p == ' ') p++;
	}
	if (line[0] && strcmp(line, prefix)) chat_add_line(line, strlen(line), style);
	(void)pl;
}

static void chat_rebuild(void)
{
	chatLineN = 0;
	for (int i = 0; i < chatN; i++) {
		chat_wrap(chatText[i], chatUser[i] ? 1 : 0, chatUser[i] ? "> " : "");
		chat_add_line("", 0, 2);
	}
	chatDirty = false;
}

static void json_escape(const char* in, char* out, size_t sz)
{
	size_t o = 0;
	for (; *in && o + 7 < sz; in++) {
		unsigned char c = (unsigned char)*in;
		if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
		else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
		else if (c < 32) out[o++] = ' ';
		else out[o++] = (char)c;
	}
	out[o] = 0;
}

static void parse_chat_reply(Req* r)
{
	static char reply[900];
	reply[0] = 0;
	const char* arr = json_array_start(r->buf, "choices");
	const char* cur = arr; Span o, m;
	if (arr && json_next_object(&cur, r->buf + r->len, &o) && json_get_obj(o, "message", &m)) json_get_text(m, "content", reply, sizeof reply);
	static char clean[900]; size_t k = 0;
	for (size_t i = 0; reply[i] && k + 1 < sizeof clean; i++) if (reply[i] != '*' && reply[i] != '#' && reply[i] != '`') clean[k++] = reply[i];
	clean[k] = 0;
	/* drop emoji and symbols the Latin-only font can't draw (copy_ascii would turn each into '?') */
	static char kept[900]; size_t q = 0;
	for (size_t i = 0; clean[i] && q + 4 < sizeof kept;) {
		unsigned char c = (unsigned char)clean[i];
		if (c >= 0xF0) i += 4;
		else if (c >= 0xE0 && !(c == 0xE2 && (unsigned char)clean[i + 1] == 0x80)) i += 3;
		else kept[q++] = clean[i++];
	}
	kept[q] = 0;
	static char shown[900]; copy_ascii(shown, sizeof shown, kept);
	chat_add(false, shown[0] ? shown : "(no answer - try again)");
}

static void chat_send(const char* text)
{
	static char body[9000], esc[1000];
	chat_add(true, text);
	size_t o = (size_t)snprintf(body, sizeof body,
		"{\"model\":\"openai\",\"max_tokens\":350,\"messages\":[{\"role\":\"system\",\"content\":\"You are Airwave, a friendly assistant living on a Playdate console. "
		"Answer in plain text, no markdown, usually under 70 words.\"}");
	int start = chatN > 8 ? chatN - 8 : 0;
	for (int i = start; i < chatN && o + 1100 < sizeof body; i++) {
		json_escape(chatText[i], esc, sizeof esc);
		o += (size_t)snprintf(body + o, sizeof body - o, ",{\"role\":\"%s\",\"content\":\"%s\"}", chatUser[i] ? "user" : "assistant", esc);
	}
	snprintf(body + o, sizeof body - o, "]}");
	if (chatReq) net_free(chatReq);
	chatReq = net_post(POLLINATIONS, body, 24 * 1024, on_fetched, NULL);
	if (!chatReq) chat_add(false, "(out of memory)");
	mode = M_CHAT;
}

static void chat_poll(void)
{
	if (!chatReq || !chatReq->finished) return;
	Req* r = chatReq; chatReq = NULL;
	if (r->status == 429) chat_add(false, "(Pollinations is busy - wait a few seconds and try again)");
	else if (r->status < 0 || r->status >= 400 || r->len == 0) {
		static char m[120]; snprintf(m, sizeof m, "(couldn't reach the AI: %s)", r->error[0] ? r->error : "no answer");
		chat_add(false, m);
	} else parse_chat_reply(r);
	net_free(r);
	chatScroll = 1 << 20;                                        /* jump to the newest line */
}

static void open_chat(void)
{
	if (!chatN) chat_add(false, "Hi! I'm Airwave's assistant, powered by pollinations.ai. Press A to type a question.");
	mode = M_CHAT;
	chatScroll = 1 << 20;
}

/* ---------- actions ---------- */

static void need_relay(void) { show_msg("This needs the relay. Put its address in Data/settings.json (see the README).", NULL); }

static void open_text(const char* source, int max)
{
	snprintf(textSource, sizeof textSource, "%s", source);
	textLen = 0; textBuf[0] = 0; textMax = max;
	kbWant = kbActive = true;                                    /* Lua shows the system keyboard on its next frame */
	kbCaps = !strcmp(source, "chat");                            /* sentences for chat, plain for searches */
	if (strcmp(source, "chat")) mode = M_TEXT;                   /* chat compose happens inside the chat screen */
}

/* source: "radio" | "pod" | "tunein" | "iptv" | "yt" | "ia" */
static void run_search(const char* source, const char* query)
{
	static char url[900], enc[200], path[300];
	urlenc(query, enc, sizeof enc);
	if (!strcmp(source, "radio")) {
		snprintf(url, sizeof url, "%s%s&name=%s", RB_BASE, relayHost[0] ? "" : "&codec=MP3", enc);
		start_fetch(url, 160 * 1024, parse_radio, query);
	} else if (!strcmp(source, "pod")) {
		snprintf(url, sizeof url, "https://itunes.apple.com/search?media=podcast&limit=25&term=%s", enc);
		start_fetch(url, 128 * 1024, parse_search, query);
	} else {
		if (!relayHost[0]) { need_relay(); return; }
		const char* route = !strcmp(source, "tunein") ? "/tunein/search?q=%s" : !strcmp(source, "yt") ? "/yt/search?q=%s" :
			!strcmp(source, "ia") ? "/ia/search?q=%s" : !strcmp(source, "pluto") ? "/pluto/channels?q=%s" :
			!strcmp(source, "shorts") ? "/yt/shorts?q=%s" : "/iptv/channels?q=%s";
		snprintf(path, sizeof path, route, enc);
		relay_url(url, sizeof url, path);
		start_fetch(url, 64 * 1024, parse_relay_list, query);
	}
}

static void activate(Item* it, bool altAction)
{
	static char url[URL_MAX];
	switch (it->kind) {
		case K_MENU: open_menu(it->arg); break;
		case K_RADIO_Q:
			snprintf(url, sizeof url, "%s%s%s", RB_BASE, relayHost[0] ? "" : "&codec=MP3", it->arg);
			start_fetch(url, 160 * 1024, parse_radio, it->label); break;
		case K_SEARCH: open_text(it->arg, 27); break;
		case K_SEARCH_ALL: open_text("all", 27); break;
		case K_SEARCH_IN: run_search(it->arg, searchQuery); break;
		case K_POD_CHARTS: start_fetch(APPLE_CHARTS, 96 * 1024, parse_charts, "Top podcasts"); break;
		case K_POD_ID:
			snprintf(url, sizeof url, "https://itunes.apple.com/lookup?id=%s", it->arg);
			start_fetch(url, 64 * 1024, parse_lookup, it->label); break;
		case K_POD_FEED: start_fetch(it->arg, 320 * 1024, parse_episodes, it->label); break;
		case K_RELAY_LIST: {
			if (!relayHost[0]) { need_relay(); break; }
			char title[40];
			if (strstr(it->arg, "offset=") || strstr(it->arg, "page=")) { snprintf(title, sizeof title, "%s", stack[depth - 1]->title); if (depth > 1) pop(); }   /* "More..." replaces the page */
			else snprintf(title, sizeof title, "%s", it->label);
			relay_url(url, sizeof url, it->arg);
			start_fetch(url, 96 * 1024, parse_relay_list, title); break;
		}
		case K_PLAY: triedRelay = false; playList = depth ? stack[depth - 1] : NULL; start_playback(it->label, it->arg, false); break;
		case K_PLAY_VIDEO:
			triedRelay = true; videoFellBack = altAction;                 /* Right button = audio only */
			playList = depth ? stack[depth - 1] : NULL;
			if (altAction) { static char a[1100]; audio_arg(it->arg, a, sizeof a); start_playback(it->label, a, false); }
			else start_playback(it->label, it->arg, true);
			break;
		default: break;
	}
}

/* channel surfing: Up/Down in the player moves through the list the item came from */
static void surf(int dir)
{
	List* l = playList;
	if (!l) return;
	bool stillThere = false;
	for (int i = 0; i < depth; i++) if (stack[i] == l) stillThere = true;
	if (!stillThere) return;
	Kind want = l->items[l->sel].kind;
	for (int i = l->sel + dir; i >= 0 && i < l->n; i += dir) {
		if (l->items[i].kind != want) continue;
		l->sel = i;
		if (l->sel < l->top) l->top = l->sel;
		if (l->sel > l->top + 7) l->top = l->sel - 7;
		activate(&l->items[i], false);
		return;
	}
}

static void submit_text(void)
{
	if (textLen == 0) { mode = depth ? M_LIST : M_HOME; return; }
	textBuf[textLen] = 0;
	if (!strcmp(textSource, "chat")) { char t[128]; snprintf(t, sizeof t, "%s", textBuf); chat_send(t); return; }
	snprintf(searchQuery, sizeof searchQuery, "%s", textBuf);
	if (strcmp(textSource, "all")) { run_search(textSource, searchQuery); return; }
	char title[40]; snprintf(title, sizeof title, "Search: %.28s", searchQuery);          /* search everywhere: pick where */
	List* l = list_new(title);
	list_add(l, "Radio stations", K_SEARCH_IN, "radio");
	list_add(l, "Podcasts", K_SEARCH_IN, "pod");
	list_add(l, "Live TV channels", K_SEARCH_IN, "pluto");
	list_add(l, "YouTube videos", K_SEARCH_IN, "yt");
	list_add(l, "YouTube Shorts", K_SEARCH_IN, "shorts");
	list_add(l, "Internet Archive", K_SEARCH_IN, "ia");
	list_add(l, "TuneIn", K_SEARCH_IN, "tunein");
	list_add(l, "My IPTV channels", K_SEARCH_IN, "iptv");
	push(l);
}

/* ---------- drawing ---------- */

static void header(const char* title, const char* right)
{
	pd->graphics->fillRect(0, 0, viewW, 24, kColorBlack);
	int rw = (right && right[0]) ? TW(fBody, right) + 12 : 0;
	char t[80]; snprintf(t, sizeof t, "%s", title);
	if (TW(fBold, t) > viewW - 16 - rw) {                                                    /* shrink with the available width */
		while (strlen(t) > 4 && TW(fBold, t) > viewW - 16 - rw - 14) t[strlen(t) - 1] = 0;
		strcat(t, "..");
	}
	T(fBold, t, 8, 4, kDrawModeFillWhite);
	if (rw) T(fBody, right, viewW - 8 - TW(fBody, right), 4, kDrawModeFillWhite);
}

static void footer(const char* hint)
{
	pd->graphics->fillRect(0, 218, viewW, 22, kColorWhite);
	pd->graphics->drawLine(0, 217, viewW, 217, 1, kColorBlack);
	char t[100]; snprintf(t, sizeof t, "%s", hint);
	while (strlen(t) > 3 && TW(fBody, t) > viewW - 12) t[strlen(t) - 1] = 0;
	T(fBody, t, 8, 222, kDrawModeFillBlack);
}

/* little icons, drawn in `c` (black on white, or white on a selected black tile) */
typedef enum { I_SEARCH, I_RADIO, I_POD, I_BBC, I_TV, I_TUNEIN, I_CHAT, I_SETUP } Icon;

static void draw_icon(Icon ic, int x, int y, LCDColor c)         /* ~44x44 box with top-left at x,y */
{
	const struct playdate_graphics* G = pd->graphics;
	switch (ic) {
		case I_SEARCH:
			G->drawEllipse(x + 4, y + 2, 26, 26, 4, 0, 0, c);
			G->drawLine(x + 26, y + 26, x + 40, y + 40, 5, c);
			break;
		case I_RADIO:
			G->drawRect(x + 2, y + 12, 40, 28, c); G->drawRect(x + 3, y + 13, 38, 26, c);
			G->drawLine(x + 8, y + 12, x + 32, y + 2, 2, c);
			G->drawEllipse(x + 8, y + 18, 16, 16, 3, 0, 0, c);
			for (int i = 0; i < 3; i++) G->fillRect(x + 29, y + 19 + i * 6, 9, 3, c);
			break;
		case I_POD:
			G->fillEllipse(x + 15, y + 2, 14, 22, 0, 0, c);
			G->drawEllipse(x + 8, y + 12, 28, 22, 3, 90, 270, c);
			G->drawLine(x + 22, y + 34, x + 22, y + 41, 3, c);
			G->drawLine(x + 13, y + 42, x + 31, y + 42, 3, c);
			break;
		case I_BBC:
			for (int i = 0; i < 3; i++) { G->fillRect(x + 1 + i * 15, y + 12, 13, 18, c); }
			G->drawLine(x + 1, y + 36, x + 43, y + 36, 2, c);
			break;
		case I_TV:
			G->drawRect(x + 2, y + 10, 40, 28, c); G->drawRect(x + 3, y + 11, 38, 26, c);
			G->fillRect(x + 8, y + 16, 22, 16, c);
			G->drawLine(x + 16, y + 10, x + 10, y + 1, 2, c); G->drawLine(x + 28, y + 10, x + 34, y + 1, 2, c);
			G->drawLine(x + 12, y + 41, x + 32, y + 41, 3, c);
			break;
		case I_TUNEIN:
			G->fillEllipse(x + 18, y + 18, 8, 8, 0, 0, c);
			for (int i = 1; i <= 3; i++) { int r = 5 + i * 5; G->drawEllipse(x + 22 - r, y + 22 - r, r * 2, r * 2, 3, 300, 420, c); G->drawEllipse(x + 22 - r, y + 22 - r, r * 2, r * 2, 3, 120, 240, c); }
			break;
		case I_CHAT:
			G->drawRect(x + 1, y + 4, 42, 28, c); G->drawRect(x + 2, y + 5, 40, 26, c);
			G->fillTriangle(x + 8, y + 31, x + 8, y + 42, x + 20, y + 31, c);
			for (int i = 0; i < 3; i++) G->fillEllipse(x + 11 + i * 10, y + 14, 6, 6, 0, 0, c);
			break;
		case I_SETUP:
			G->drawEllipse(x + 8, y + 8, 28, 28, 5, 0, 0, c);
			for (int i = 0; i < 8; i++) {
				static const int dx[8] = {0, 14, 20, 14, 0, -14, -20, -14}, dy[8] = {-20, -14, 0, 14, 20, 14, 0, -14};
				G->fillRect(x + 20 + dx[i], y + 20 + dy[i], 4, 4, c);
			}
			break;
	}
}

static void home_tile(int i, int x, int y, int w, int h, bool sel)
{
	static const char* names[8] = { "Search", "Radio", "Podcasts", "BBC", "TV", "TuneIn", "AI Chat", "Setup" };
	static const Icon icons[8] = { I_SEARCH, I_RADIO, I_POD, I_BBC, I_TV, I_TUNEIN, I_CHAT, I_SETUP };
	LCDColor ink = sel ? kColorWhite : kColorBlack;
	if (sel) pd->graphics->fillRect(x, y, w, h, kColorBlack);
	else { pd->graphics->drawRect(x, y, w, h, kColorBlack); pd->graphics->drawRect(x + 1, y + 1, w - 2, h - 2, kColorBlack); }
	draw_icon(icons[i], x + (w - 44) / 2, y + 10, ink);
	Tc(fBold, names[i], x + w / 2, y + h - 22, sel ? kDrawModeFillWhite : kDrawModeFillBlack);
}

static void draw_home(void)
{
	header("Airwave", relayHost[0] ? "relay on" : "no relay");
	for (int i = 0; i < 8; i++) home_tile(i, 8 + (i % 4) * 97, 32 + (i / 4) * 92, 91, 86, i == homeSel);
	footer("A open   D-pad / crank to move");
}

static void row_icon(Kind k, int x, int y, LCDColor c)           /* ~10x10 */
{
	const struct playdate_graphics* G = pd->graphics;
	switch (k) {
		case K_MENU: case K_RADIO_Q: case K_POD_CHARTS: G->fillTriangle(x + 2, y, x + 2, y + 10, x + 9, y + 5, c); break;
		case K_PLAY: G->fillRect(x, y + 2, 3, 6, c); G->fillTriangle(x + 3, y + 1, x + 3, y + 9, x + 10, y + 5, c); break;
		case K_PLAY_VIDEO: G->drawRect(x, y + 1, 11, 8, c); G->fillTriangle(x + 4, y + 3, x + 4, y + 7, x + 8, y + 5, c); break;
		case K_SEARCH: case K_SEARCH_ALL: case K_SEARCH_IN: G->drawEllipse(x, y, 7, 7, 2, 0, 0, c); G->drawLine(x + 6, y + 6, x + 10, y + 10, 2, c); break;
		default: G->fillRect(x, y + 1, 10, 2, c); G->fillRect(x, y + 4, 10, 2, c); G->fillRect(x, y + 7, 10, 2, c); break;
	}
}

static void draw_list(List* l)
{
	char page[24] = "";
	if (l->n > 8) snprintf(page, sizeof page, "%d / %d", l->sel + 1, l->n);
	header(l->title, page);
	for (int i = 0; i < 8; i++) {
		int idx = l->top + i;
		if (idx >= l->n) break;
		Item* it = &l->items[idx];
		int y = 27 + i * 24;
		bool sel = idx == l->sel;
		char lab[96]; snprintf(lab, sizeof lab, "%.60s", it->label);
		if (TW(fBody, lab) > 350) {                                                          /* fit the row, with an ellipsis */
			while (TW(fBody, lab) > 336 && strlen(lab) > 4) lab[strlen(lab) - 1] = 0;
			while (strlen(lab) > 1 && (lab[strlen(lab) - 1] == ' ' || lab[strlen(lab) - 1] == '|' || lab[strlen(lab) - 1] == '-')) lab[strlen(lab) - 1] = 0;
			strcat(lab, "...");
		}
		if (sel) pd->graphics->fillRect(4, y, 384, 22, kColorBlack);
		else pd->graphics->drawLine(8, y + 23, 384, y + 23, 1, kColorBlack);
		LCDColor ink = sel ? kColorWhite : kColorBlack;
		row_icon(it->kind, 11, y + 6, ink);
		T(fBody, lab, 28, y + 3, sel ? kDrawModeFillWhite : kDrawModeFillBlack);
	}
	if (l->n > 8) {                                                                          /* scrollbar */
		int h = 190 * 8 / l->n; if (h < 12) h = 12;
		int y = 26 + (190 - h) * l->top / (l->n - 8);
		pd->graphics->drawLine(396, 26, 396, 216, 1, kColorBlack);
		pd->graphics->fillRect(393, y, 6, h, kColorBlack);
	}
	bool v = l->n && l->items[l->sel].kind == K_PLAY_VIDEO;
	footer(v ? "A watch    Right audio only    B back" : depth > 1 || true ? "A select    B back    crank scrolls" : "");
}

static void draw_play(void)
{
	header("Now playing", started ? "live" : "");
	/* title */
	{
		const char* s = nowTitle; int y = 32, lines = 0;
		char line[80];
		while (*s && lines < 3) {
			int n = (int)strlen(s); if (n > 60) n = 60;
			while (n > 1 && TW(fBig, (snprintf(line, sizeof line, "%.*s", n, s), line)) > 384) n--;
			if (s[n] && n > 4) { int k = n; while (k > 1 && s[k] != ' ') k--; if (k > 4) n = k; }
			snprintf(line, sizeof line, "%.*s", n, s);
			T(fBig, line, 8, y, kDrawModeFillBlack);
			y += 26; lines++; s += n; while (*s == ' ') s++;
		}
	}
	/* equaliser */
	uint32_t t = now_ms();
	for (int i = 0; i < 20; i++) {
		int h = 4;
		if (started) { uint32_t v = (t / 90 + (uint32_t)i * 37) * 2654435761u; h = 6 + (int)((v >> 24) % 52); }
		pd->graphics->fillRect(14 + i * 19, 168 - h, 14, h, kColorBlack);
	}
	/* status + buffer */
	char st[96];
	if (!stream) snprintf(st, sizeof st, "Stopped");
	else if (stream->status < 0) snprintf(st, sizeof st, "%s", !strcmp(stream->error, "HTTP 502") ? "The relay couldn't open it (YouTube?)" : stream->error);
	else if (stream->total == 0) snprintf(st, sizeof st, "Connecting...");
	else if (!started) snprintf(st, sizeof st, "Buffering %u%%", (unsigned)(ring_fill(&stream->ring) * 100 / PREBUFFER));
	else { int s = (int)((t - startedAt) / 1000); snprintf(st, sizeof st, "%d:%02d", s / 60, s % 60); }
	T(fBold, st, 8, 176, kDrawModeFillBlack);
	if (stream) {
		pd->graphics->drawRect(8, 198, 384, 10, kColorBlack);
		int w = (int)((uint64_t)ring_fill(&stream->ring) * 380 / stream->ring.size);
		pd->graphics->fillRect(10, 200, w, 6, kColorBlack);
		char kb[32]; snprintf(kb, sizeof kb, "%u KB", (unsigned)stream->total / 1024);
		T(fBody, kb, 392 - TW(fBody, kb), 176, kDrawModeFillBlack);
	}
	footer(playList ? "Up/Down next    B stop & back" : "B stop & back");
}

static const char* search_title(void)
{
	static const char* titles[][2] = { {"radio","Search radio"}, {"pod","Search podcasts"}, {"tunein","Search TuneIn"}, {"iptv","Search my IPTV"},
		{"yt","Search YouTube"}, {"ia","Search Internet Archive"}, {"all","Search everything"}, {"chat","Message the AI"},
		{"pluto","Search live channels"}, {"shorts","Search Shorts (one word / #tag)"} };
	for (int i = 0; i < 10; i++) if (!strcmp(titles[i][0], textSource)) return titles[i][1];
	return "Search";
}

/* what you are typing, wrapped to `w` pixels; returns the number of lines drawn */
static int draw_typed(int x, int y, int w, int maxLines)
{
	static char shown[160]; snprintf(shown, sizeof shown, "%.*s", textLen, textBuf);
	int n = (int)strlen(shown), pos = 0, line = 0, endx = x;
	while ((pos < n || line == 0) && line < maxLines) {
		int cut = n - pos;
		static char seg[100];
		while (cut > 0 && TW(fBody, (snprintf(seg, sizeof seg, "%.*s", cut > 90 ? 90 : cut, shown + pos), seg)) > w) cut--;
		if (cut == 0 && pos < n) cut = 1;
		if (pos + cut < n && cut > 4) { int k = cut; while (k > 1 && shown[pos + k] != ' ') k--; if (k > 3) cut = k; }
		snprintf(seg, sizeof seg, "%.*s", cut, shown + pos);
		T(fBody, seg, x, y + line * 18, kDrawModeFillBlack);
		endx = x + TW(fBody, seg);
		pos += cut; while (shown[pos] == ' ') pos++;
		line++;
		if (pos >= n) break;
	}
	if ((now_ms() / 450) & 1) pd->graphics->fillRect(endx + 2, y + (line > 0 ? line - 1 : 0) * 18 + 1, 2, 15, kColorBlack);
	return line;
}

static void draw_text_screen(void)
{
	char cnt[16]; snprintf(cnt, sizeof cnt, "%d/%d", textLen, textMax);
	header(search_title(), cnt);
	int w = viewW - 24;
	pd->graphics->drawRect(8, 34, viewW - 16, 74, kColorBlack);
	pd->graphics->drawRect(9, 35, viewW - 18, 72, kColorBlack);
	draw_typed(16, 42, w - 8, 3);
	T(fBody, kbVisible ? "OK to search, B deletes" : "Opening keyboard...", 12, 120, kDrawModeFillBlack);
	footer(!strcmp(textSource, "all") ? "then choose where to search" : "type, then OK");
}

static void draw_chat(void)
{
	bool typing = kbActive && !strcmp(textSource, "chat");
	char cnt[16]; snprintf(cnt, sizeof cnt, "%d/%d", textLen, textMax);
	header("AI chat - pollinations.ai", typing ? cnt : chatReq ? "thinking..." : "");
	/* the transcript re-wraps to whatever width the keyboard leaves */
	chatWrapW = viewW - 28;
	if (chatWrapW != chatWrapBuilt && (abs(chatWrapW - chatWrapBuilt) >= 6 || viewW == 400)) { chatDirty = true; chatWrapBuilt = chatWrapW; }
	if (chatDirty) chat_rebuild();
	int composeH = typing ? 44 : 0;
	int rows = (216 - 28 - composeH) / 18, maxScroll = chatLineN > rows ? chatLineN - rows : 0;
	if (typing) chatScroll = maxScroll;                                  /* keep the newest lines in view while typing */
	if (chatScroll > maxScroll) chatScroll = maxScroll;
	if (chatScroll < 0) chatScroll = 0;
	for (int i = 0; i < rows; i++) {
		int idx = chatScroll + i;
		if (idx >= chatLineN) break;
		T(chatStyle[idx] == 1 ? fBold : fBody, chatLines[idx], 8, 28 + i * 18, kDrawModeFillBlack);
	}
	if (chatReq) {
		char d[24]; snprintf(d, sizeof d, "thinking%.*s", (int)((now_ms() / 350) % 4), "...");
		T(fBody, d, 8, 28 + (chatLineN - chatScroll < rows ? chatLineN - chatScroll : rows - 1) * 18, kDrawModeFillBlack);
	}
	if (maxScroll && !typing) {
		int h = 180 * rows / chatLineN; if (h < 10) h = 10;
		pd->graphics->fillRect(viewW - 4, 28 + (180 - h) * chatScroll / maxScroll, 4, h, kColorBlack);
	}
	if (typing) {                                                        /* input box rides at the bottom of the free area */
		pd->graphics->drawRect(6, 216 - composeH + 2, viewW - 12, composeH - 4, kColorBlack);
		draw_typed(12, 216 - composeH + 5, viewW - 28, 2);
	}
	footer(typing ? "OK sends, B deletes" : "A write    crank / up-down scroll    B back");
}

/* ---------- splash animation and page transitions ---------- */

#ifndef TRANS_MS
#define TRANS_MS 230
#endif
static LCDBitmap* oldBmp;              /* the screen we are leaving, slid out while the new one slides in */
static struct { bool active; int dir; uint32_t t0; } trans;
static uint32_t splashT0, fadeInT0;

/* ordered-dither fade: cover the screen with `black`/white at coverage `level` 0..16 (a 4x4 Bayer mask, so it dissolves) */
static void fade_overlay(int x, int y, int w, int h, bool black, int level)
{
	static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
	if (level <= 0) return;
	static uint8_t pat[16];                       /* 8 rows of colour then 8 rows of mask */
	for (int r = 0; r < 8; r++) {
		uint8_t mask = 0;
		for (int c = 0; c < 8; c++) if (bayer[r & 3][c & 3] < level) mask |= (uint8_t)(0x80 >> c);
		pat[r] = black ? 0x00 : 0xFF; pat[8 + r] = mask;
	}
	pd->graphics->fillRect(x, y, w, h, (LCDColor)(uintptr_t)pat);
}

static float ease_out(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return 1 - (1 - t) * (1 - t) * (1 - t); }

static int ui_level(void)
{
	switch (mode) {
		case M_SPLASH: return -1;
		case M_HOME: return 0;
		case M_LIST: case M_LOADING: return depth;
		default: return depth + 1;
	}
}

static bool trans_ok(Mode m) { return m != M_VIDEO && m != M_SPLASH && m != M_TEXT; }

static void trans_start(int dir)
{
	if (!oldBmp) return;
	uint8_t *data, *mask; int w, h, rb;
	uint8_t* fb = pd->graphics->getFrame();
	pd->graphics->getBitmapData(oldBmp, &w, &h, &rb, &mask, &data);
	if (!fb || !data || h < 240 || rb < 50) return;
	for (int y = 0; y < 240; y++) memcpy(data + y * rb, fb + y * LCD_ROWSIZE, rb < LCD_ROWSIZE ? (size_t)rb : (size_t)LCD_ROWSIZE);
	trans.active = true; trans.dir = dir; trans.t0 = now_ms();
}

static void draw_splash(uint32_t t)
{
	pd->graphics->clear(kColorBlack);
	const LCDColor W = kColorWhite;
	const int cx = 52, cy = 120;
	float dotT = ease_out(t / 450.0f);
	int dr = (int)(13 * dotT);
	if (dr > 0) pd->graphics->fillEllipse(cx - dr, cy - dr, dr * 2, dr * 2, 0, 0, W);
	for (int i = 0; i < 6; i++) {                         /* radio waves leave the dot one after another */
		float p = ease_out(((int)t - 300 - i * 150) / 650.0f);
		if (p <= 0) continue;
		int r = (int)(16 + p * (22 + i * 17));
		int lw = 6 - i / 2;
		pd->graphics->drawEllipse(cx - r, cy - r, r * 2, r * 2, lw < 2 ? 2 : lw, 30, 150, W);
	}
	/* wordmark and tagline dissolve in (the black overlay thins out) */
	const int tx = 176, ty = 78;
	T(fTitle ? fTitle : fBig, "Airwave", tx, ty, kDrawModeFillWhite);
	T(fBody, "radio  podcasts  TV  chat", tx + 2, ty + 54, kDrawModeFillWhite);
	pd->graphics->fillRect(tx + 2, ty + 42, 190, 2, W);
	int tl = 16 - (int)(16 * (t - 900) / 700.0f), gl = 16 - (int)(16 * (t - 1500) / 600.0f);
	if (t < 900) tl = 16;
	if (t < 1500) gl = 16;
	fade_overlay(tx - 4, ty - 6, 230, 50, true, tl);
	fade_overlay(tx - 4, ty + 40, 230, 36, true, gl);
	if (t > 2500) fade_overlay(0, 0, 400, 240, false, (int)(16 * (t - 2500) / 380.0f));   /* dissolve to white, then the home screen fades in */
}

/* ---------- main loop ---------- */

static void on_access(bool allowed, void* ud) { (void)ud; netAllowed = allowed; if (!allowed) show_msg("Network access was denied.", NULL); }

static void show_setup(void)
{
	const char* wifi = pd->network->getStatus() == kWifiConnected ? "connected" : "not connected";
	snprintf(msg, sizeof msg, "Wi-Fi: %s\nRelay: %s%s\nTo set it: edit Data/airwave/settings.json - \"relay\": \"192.168.1.20:8787\" (and \"relay_token\"). See the README.",
		wifi, relayHost[0] ? relayHost : "not set", relayToken[0] ? " (token set)" : "");
	mode = M_MSG;
}

static void list_input(List* l, PDButtons pushed)
{
	float c = pd->system->getCrankChange(); crankAcc += c;
	while (crankAcc > 25) { if (l->sel < l->n - 1) l->sel++; crankAcc -= 25; }
	while (crankAcc < -25) { if (l->sel > 0) l->sel--; crankAcc += 25; }
	if ((pushed & kButtonDown) && l->sel < l->n - 1) l->sel++;
	if ((pushed & kButtonUp) && l->sel > 0) l->sel--;
	if (l->sel < l->top) l->top = l->sel;
	if (l->sel > l->top + 7) l->top = l->sel - 7;
}

/* One frame of the app. `left` = x of the system keyboard's left edge (400 when hidden); while it slides in the whole UI
   re-lays itself out in the free width, so it follows the keyboard. */
static void app_update(int left, bool keyboardOn)
{
	static bool first = true;
	if (first) { first = false; dbg("first frame"); }
	PDButtons cur, pushed, released;
	pd->system->getButtonState(&cur, &pushed, &released);
	kbVisible = keyboardOn;
	viewW = left < 160 ? 160 : left > 400 ? 400 : left;
	if (keyboardOn) pushed = 0;                                   /* the keyboard owns the buttons (they still reach us as raw state) */
	if (mode == M_SPLASH) {
		uint32_t now = now_ms();
		if (!splashT0) splashT0 = now;
		uint32_t t = now - splashT0;
		if ((pushed & (kButtonA | kButtonB)) && t > 700 && t < 2500) { splashT0 = now - 2500; t = 2500; }     /* skip */
		draw_splash(t);
		if (t >= 2880) { mode = netAllowed ? M_HOME : M_LOADING; fadeInT0 = now; }
		return;
	}
	if (trans.active) pushed = 0;                                 /* no input while sliding */
	if (mode != M_VIDEO) pd->graphics->clear(kColorWhite);
	if (viewW < 400) pd->graphics->setClipRect(0, 0, viewW, 240); else pd->graphics->clearClipRect();
	Mode drawnMode = mode; int drawnLevel = ui_level();
	int slideX = 0;
	if (trans.active) {                                            /* the new screen is drawn shifted; the old bitmap fills the gap */
		float t = (float)(now_ms() - trans.t0) / TRANS_MS;
		if (t >= 1.0f) trans.active = false;
		else { int m = (int)(400 * (1.0f - ease_out(t))); slideX = trans.dir > 0 ? m : -m; pd->graphics->setDrawOffset(slideX, 0); }
	}

	if (!accessAsked) {
		accessAsked = true;
		enum accessReply a = pd->network->http->requestAccess(NULL, 0, true, "to stream radio and TV, and chat", on_access, NULL);
		if (a == kAccessAllow) netAllowed = true;
		else if (a == kAccessDeny) show_msg("Network access was denied.", NULL);
	}
	if (netAllowed && mode == M_LOADING && !pending && !depth) mode = M_HOME;

	net_update();
	maybe_fall_back();
	chat_poll();

	/* finished list fetch */
	if (pending && pending->finished) {
		Req* r = pending; pending = NULL;
		Parser p = pendingParser;
		if (r->status < 0 || r->status >= 400) { static char m[200]; snprintf(m, sizeof m, "%s%s", r->error[0] ? r->error : "request failed", r->status == 502 ? " - the relay couldn't open that source" : ""); show_msg("Couldn't load: %s", m); }
		else if (r->len == 0) show_msg("The server sent nothing.", NULL);
		else p(r);
		net_free(r);
	}

	/* streaming playback control */
	if (mode == M_VIDEO && stream) { uint32_t now = now_ms(); video_demux(now); video_show(now); }
	if (stream && player && !started && stream->status >= 0 && ring_fill(videoActive ? &audioRing : &stream->ring) >= (videoActive ? VIDEO_AUDIO_START : PREBUFFER)) {
		started = true; startedAt = now_ms();
		pd->sound->fileplayer->play(player, 1);
	}

	switch (mode) {
		case M_HOME: {
			if ((pushed & kButtonRight) && homeSel % 4 < 3) homeSel++;
			if ((pushed & kButtonLeft) && homeSel % 4 > 0) homeSel--;
			if ((pushed & kButtonDown) && homeSel < 4) homeSel += 4;
			if ((pushed & kButtonUp) && homeSel >= 4) homeSel -= 4;
			float c = pd->system->getCrankChange(); crankAcc += c;
			while (crankAcc > 30) { homeSel = (homeSel + 1) % 8; crankAcc -= 30; }
			while (crankAcc < -30) { homeSel = (homeSel + 7) % 8; crankAcc += 30; }
			draw_home();
			if (pushed & kButtonA) {
				static const char* ids[8] = { "", "radio", "pod", "bbc", "tv", "tunein", "", "" };
				if (homeSel == 0) open_text("all", 27);
				else if (homeSel == 6) open_chat();
				else if (homeSel == 7) show_setup();
				else open_menu(ids[homeSel]);
			}
			break;
		}
		case M_LOADING:
			if (!netAllowed) { header("Airwave", ""); T(fBody, "Waiting for network permission...", 10, 100, kDrawModeFillBlack); }
			else {
				header(pendingTitle[0] ? pendingTitle : "Airwave", "");
				char dots[24]; snprintf(dots, sizeof dots, "Loading%.*s", (int)((now_ms() / 400) % 4), "...");
				T(fBig, dots, 10, 90, kDrawModeFillBlack);
				footer("B cancel");
				if (pushed & kButtonB) { if (pending) { net_free(pending); pending = NULL; } mode = depth ? M_LIST : M_HOME; }
			}
			break;
		case M_MSG: {
			header("Airwave", "");
			int y = 36; const char* s = msg;
			while (*s) {                                                                    /* wrap by width, honour \n */
				static char line[100]; int n = 0;
				while (s[n] && s[n] != '\n' && n < 90 && TW(fBody, (snprintf(line, sizeof line, "%.*s", n + 1, s), line)) < 380) n++;
				if (s[n] && s[n] != '\n') { int k = n; while (k > 1 && s[k] != ' ') k--; if (k > 4) n = k; }
				snprintf(line, sizeof line, "%.*s", n, s);
				T(fBody, line, 10, y, kDrawModeFillBlack); y += 20;
				s += n; if (*s == '\n' || *s == ' ') s++;
			}
			footer("A / B back");
			if (pushed & (kButtonA | kButtonB)) mode = depth ? M_LIST : M_HOME;
			break;
		}
		case M_TEXT:
			draw_text_screen();
			if (!kbActive && !kbWant) mode = depth ? M_LIST : M_HOME;     /* keyboard was cancelled */
			break;
		case M_CHAT: {
			draw_chat();
			float c = pd->system->getCrankChange(); crankAcc += c;
			while (crankAcc > 20) { chatScroll++; crankAcc -= 20; }
			while (crankAcc < -20) { chatScroll--; crankAcc += 20; }
			if (pushed & kButtonDown) chatScroll += 3;
			if (pushed & kButtonUp) chatScroll -= 3;
			if ((pushed & kButtonA) && !chatReq && !kbActive) open_text("chat", 110);
			if ((pushed & kButtonB) && !kbActive) mode = depth ? M_LIST : M_HOME;
			break;
		}
		case M_VIDEO: {
			bool dead = stream && stream->status < 0 && vqCount == 0;
			if (dead && !vGotFrame && !videoFellBack && strstr(playArg, "/video/")) {      /* no picture at all (audio-only channel?): fall back to audio */
				videoFellBack = true;
				static char a[1100]; audio_arg(playArg, a, sizeof a);
				char t[80]; snprintf(t, sizeof t, "%s", playTitle);
				start_playback(t, a, false);
				break;
			}
			if (!vGotFrame || dead) {
				pd->graphics->clear(kColorBlack);
				T(fBold, dead ? "Can't play this one" : "Loading video...", 12, 100, kDrawModeFillWhite);
				if (dead) { T(fBody, !strcmp(stream->error, "HTTP 502") ? "The relay couldn't open it (YouTube needs yt-dlp, see README)" : stream->error[0] ? stream->error : "stream ended", 12, 124, kDrawModeFillWhite); T(fBody, nowTitle, 12, 146, kDrawModeFillWhite); }
			}
			if (pushed & kButtonDown) surf(1);
			if (pushed & kButtonUp) surf(-1);
			if (pushed & kButtonB) { stop_playback(); mode = depth ? M_LIST : M_HOME; }
			break;
		}
		case M_PLAY:
			draw_play();
			if (pushed & kButtonDown) surf(1);
			if (pushed & kButtonUp) surf(-1);
			if (pushed & kButtonB) { stop_playback(); mode = depth ? M_LIST : M_HOME; }
			break;
		case M_LIST: {
			if (!depth) { mode = M_HOME; break; }
			List* l = stack[depth - 1];
			list_input(l, pushed);
			draw_list(l);
			if ((pushed & kButtonA) && l->n) activate(&l->items[l->sel], false);
			if ((pushed & kButtonRight) && l->n && l->items[l->sel].kind == K_PLAY_VIDEO) activate(&l->items[l->sel], true);
			if (pushed & kButtonB) pop();
			break;
		}
	}
	if (viewW < 400) pd->graphics->clearClipRect();
	if (slideX) {                                                  /* finish the slide: put the old page back beside the new one */
		pd->graphics->setDrawOffset(0, 0);
		pd->graphics->clearClipRect();
		int ox = trans.dir > 0 ? slideX - 400 : 400 + slideX;
		pd->graphics->setDrawMode(kDrawModeCopy);                  /* text drawing leaves a fill mode set; a bitmap needs plain copy */
		pd->graphics->drawBitmap(oldBmp, ox, 0, kBitmapUnflipped);
		int ex = trans.dir > 0 ? slideX : 400 + slideX;
		pd->graphics->drawLine(ex, 0, ex, 240, 2, kColorBlack);
	}
	/* a level change (list opened/closed, player opened...) starts a slide: the frame just drawn is the page being left */
	int lvlNow = ui_level();
	if (lvlNow != drawnLevel && !trans.active && !kbVisible && trans_ok(drawnMode) && trans_ok(mode)) {
		pd->graphics->setDrawOffset(0, 0);
		trans_start(lvlNow > drawnLevel ? 1 : -1);
	}
	if (fadeInT0) {                                                /* home fades in from white after the splash */
		int lv = 16 - (int)(16 * (now_ms() - fadeInT0) / 380.0f);
		if (lv <= 0) fadeInT0 = 0; else fade_overlay(0, 0, 400, 240, false, lv);
	}
}

/* ---------- Lua bridge: main.lua drives the loop and the system keyboard ---------- */

static int l_update(lua_State* L)
{
	(void)L;
	app_update(pd->lua->getArgInt(1), pd->lua->getArgBool(2));
	return 0;
}

static int l_kbRequest(lua_State* L)                    /* -> text, caps  (or nil) */
{
	(void)L;
	if (!kbWant) { pd->lua->pushNil(); return 1; }
	kbWant = false;
	pd->lua->pushString(textBuf);
	pd->lua->pushInt(kbCaps);
	return 2;
}

static int l_kbText(lua_State* L)
{
	(void)L;
	const char* t = pd->lua->getArgString(1);
	copy_ascii(textBuf, sizeof textBuf, t ? t : "");
	textLen = (int)strlen(textBuf);
	if (textLen > textMax) textLen = textMax;                    /* longer than the field allows: ignore the rest */
	return 0;
}

static int l_kbDone(lua_State* L)                        /* keyboard closing: ok = the user chose OK */
{
	(void)L;
	bool ok = pd->lua->getArgBool(1);
	kbActive = false; kbWant = false;
	dbg("keyboard closed ok=%d len=%d", ok, textLen);
	if (ok) submit_text(); else if (mode == M_TEXT) mode = depth ? M_LIST : M_HOME;
	return 0;
}

static void wifi_done(PDNetErr err) { dbg("wifi: %d", (int)err); }

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg)
{
	(void)arg;
	if (event == kEventInit) {
		pd = playdate;
		const char* err;
		fBody = pd->graphics->loadFont("/System/Fonts/Roobert-11-Medium.pft", &err);
		fBold = pd->graphics->loadFont("/System/Fonts/Roobert-11-Bold.pft", &err);
		fBig = pd->graphics->loadFont("/System/Fonts/Roobert-20-Medium.pft", &err);
		fTitle = pd->graphics->loadFont("/System/Fonts/Roobert-24-Medium.pft", &err);
		oldBmp = pd->graphics->newBitmap(400, 240, kColorWhite);
		if (!fBody) fBody = pd->graphics->loadFont("/System/Fonts/Asheville-Sans-14-Light.pft", &err);
		if (!fBold) fBold = fBody;
		if (!fBig) fBig = fBold;
		load_settings();
		if (debugOn) { SDFile* lf = pd->file->open("airwave.log", kFileWrite); if (lf) pd->file->close(lf); }
		dbg("init: fonts %p %p %p", (void*)fBody, (void*)fBold, (void*)fBig);
		pd->network->setEnabled(true, wifi_done);       /* the device may call this unconditionally, so never pass NULL */
		/* no update callback on purpose: that makes the system start Lua, and main.lua drives the frames */
	} else if (event == kEventInitLua) {
		const char* err;
		if (!pd->lua->addFunction(l_update, "airwave.update", &err)) pd->system->logToConsole("addFunction update: %s", err);
		if (!pd->lua->addFunction(l_kbRequest, "airwave.kbRequest", &err)) pd->system->logToConsole("addFunction kbRequest: %s", err);
		if (!pd->lua->addFunction(l_kbText, "airwave.kbText", &err)) pd->system->logToConsole("addFunction kbText: %s", err);
		if (!pd->lua->addFunction(l_kbDone, "airwave.kbDone", &err)) pd->system->logToConsole("addFunction kbDone: %s", err);
	} else if (event == kEventTerminate) {
		stop_playback();
	}
	return 0;
}
