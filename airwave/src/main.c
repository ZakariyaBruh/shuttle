/* Airwave - radio, podcasts, TV (dithered video), an audio library and an AI chat for Playdate.
   App logic lives here; src/ui.c draws, src/net.c talks HTTP, src/parse.c reads JSON/RSS, Source/main.lua owns the frame loop + keyboard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include "ui.h"
#include "defaults.h"
#include "art_layout.h"

PlaydateAPI* pd;

#define RB_BASE "https://de1.api.radio-browser.info/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=30&bitrateMax=192"
#define APPLE_CHARTS "https://rss.marketingtools.apple.com/api/v2/us/podcasts/top/25/podcasts.json"
#define POLLINATIONS "https://text.pollinations.ai/openai"
#define TUNEIN "https://opml.radiotime.com/"
#define BBC_WS "https://stream.live.vc.bbcmedia.co.uk/bbc_world_service"
#define RING_BYTES (128 * 1024)
#define PREBUFFER (48 * 1024)
#define MAX_ITEMS 40
#define VIDEO_RING (128 * 1024)
#define VIDEO_AUDIO_RING (48 * 1024)
#define VIDEO_AUDIO_START (9 * 1024)   /* ~1.5 s of the relay's 48 kbps audio */
#define AV_DELAY_MS 1500               /* video is shown this long after it arrives, to line up with the audio prebuffer */
#define VQ_MAX 64
#define VQ_BYTES (160 * 1024)          /* cap on queued compressed frames: back-pressure instead of memory growth */
#define FRAME_BYTES (LCD_ROWSIZE * 240)
#define ENC_MAX (URL_MAX * 3)
#define VERSION "2.5"

typedef enum { M_SPLASH, M_HOME, M_LIST, M_LOADING, M_MSG, M_TEXT, M_PLAY, M_VIDEO, M_CHAT } Mode;
typedef void (*Parser)(Req* r);
static void start_playback_hook(const char* title, const char* arg);

static List* stack[8];
static int depth;
static Mode mode = M_SPLASH;
static bool netAllowed, accessAsked;
static char relayToken[48];       /* optional "relay_token", sent as ?k= (the relay's --token) */
static char relayHost[128];       /* "host:port" or a full http(s):// URL (e.g. a Cloudflare tunnel); set in Setup or settings.json */
static float crankAcc;
static int homeSel;
static float homePos;
static char lastTest[80] = "Not tested yet";

/* message cards */
static char msg[400], msgTitle[40] = "Heads up";
static IconId msgIcon = IC_INFO;
static int msgAction;             /* 1 = A opens Setup */

/* loading */
static Req* pending;
static Parser pendingParser;
static char pendingTitle[40];

/* text entry (system keyboard, see main.lua) */
static char textBuf[128];
static int textLen, textMax;
static char textSource[16];       /* "radio" "pod" "tunein" "lib" "iptv" "yt" "ia" "pluto" "shorts" "all" "chat" "relay" "token" */
static bool kbWant, kbActive, kbVisible;
static int kbCaps;
static char searchQuery[28];

/* playback */
static FilePlayer* player;
static Req* stream;
static bool started;
static uint32_t startedAt;
static char nowTitle[80], nowSub[56];

/* ---------- helpers ---------- */

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

bool relay_configured(void) { return relayHost[0] != 0; }

static void show_card(IconId ic, const char* title, const char* body, int action)
{
	snprintf(msgTitle, sizeof msgTitle, "%s", title);
	snprintf(msg, sizeof msg, "%s", body);
	msgIcon = ic; msgAction = action;
	mode = M_MSG;
}
static void show_msg(const char* fmt, const char* a)
{
	static char t[400];
	snprintf(t, sizeof t, fmt, a ? a : "");
	show_card(IC_INFO, "Heads up", t, 0);
}
static void need_relay(const char* what)
{
	static char t[400];
	snprintf(t, sizeof t, "%s needs a small helper server (the relay) to convert it for the Playdate. "
		"Radio, podcasts, TuneIn, the Library and AI Chat all work without it.", what);
	show_card(IC_LOCK, "Needs the relay", t, 1);
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

void copy_ascii(char* dst, size_t dstsz, const char* src);

static Item* list_add2(List* l, const char* label, const char* sub, Kind k, const char* arg)
{
	if (!l->items || l->n >= MAX_ITEMS) return NULL;
	Item* it = &l->items[l->n++];
	copy_ascii(it->label, sizeof it->label, label);
	copy_ascii(it->sub, sizeof it->sub, sub ? sub : "");
	snprintf(it->arg, sizeof it->arg, "%s", arg ? arg : "");
	it->kind = k;
	return it;
}
static Item* list_add(List* l, const char* label, Kind k, const char* arg) { return list_add2(l, label, "", k, arg); }

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

/* Which rows are padlocked when there is no relay (they open a friendly explanation, never an error). */
bool item_locked(const Item* it)
{
	if (relayHost[0]) return false;
	static const char* relayMenus[] = { "tv", "yt", "shorts", "iptv", "archive", "bbc", "pluto" };
	static const char* relaySearch[] = { "iptv", "yt", "ia", "pluto", "shorts" };
	switch (it->kind) {
		case K_RELAY_LIST: case K_PLAY_VIDEO: return true;
		case K_PLAY: return !strncmp(it->arg, "relay:", 6);
		case K_MENU: for (int i = 0; i < 7; i++) if (!strcmp(it->arg, relayMenus[i])) return true; return false;
		case K_SEARCH: case K_SEARCH_IN: for (int i = 0; i < 5; i++) if (!strcmp(it->arg, relaySearch[i])) return true; return false;
		default: return false;
	}
}

IconId item_icon(const Item* it)
{
	switch (it->kind) {
		case K_SEARCH: case K_SEARCH_ALL: case K_SEARCH_IN: return IC_SEARCH;
		case K_PLAY_VIDEO: return IC_VIDEO;
		case K_PLAY: case K_TUNEIN_PLAY: case K_RADIO_Q: return IC_RADIO;
		case K_POD_CHARTS: case K_POD_GENRES: case K_POD_GENRE: case K_POD_ID: case K_POD_FEED: return IC_POD;
		case K_TUNEIN_LIST: return IC_TUNEIN;
		case K_IA_Q: case K_IA_ITEM: return IC_LIB;
		case K_RELAY_EDIT: case K_TOKEN_EDIT: return IC_SETUP;
		case K_RELAY_TEST: return IC_CHECK;
		case K_INFO: return IC_INFO;
		case K_MENU:
			if (!strcmp(it->arg, "yt") || !strcmp(it->arg, "shorts")) return IC_YT;
			if (!strcmp(it->arg, "tv") || !strcmp(it->arg, "pluto") || !strcmp(it->arg, "iptv") || !strcmp(it->arg, "archive")) return IC_TV;
			if (!strcmp(it->arg, "bbc") || !strcmp(it->arg, "radio")) return IC_RADIO;
			if (!strcmp(it->arg, "podgenres")) return IC_POD;
			return IC_LIST;
		default: return IC_LIST;
	}
}

/* ---------- settings ---------- */

static void load_settings(void)
{
	snprintf(relayHost, sizeof relayHost, "%s", DEFAULT_RELAY);
	snprintf(relayToken, sizeof relayToken, "%s", DEFAULT_RELAY_TOKEN);
	SDFile* f = pd->file->open("settings.json", kFileReadData);
	if (!f) return;
	static char buf[512];
	int n = pd->file->read(f, buf, sizeof buf - 1);
	pd->file->close(f);
	if (n <= 0) return;
	buf[n] = 0;
	Span s = { buf, (size_t)n };
	json_get_str(s, "relay", relayHost, sizeof relayHost);
	json_get_str(s, "relay_token", relayToken, sizeof relayToken);
	debugOn = strstr(buf, "\"debug\": true") || strstr(buf, "\"debug\":true");
}

static void save_settings(void)
{
	SDFile* f = pd->file->open("settings.json", kFileWrite);
	if (!f) return;
	static char out[400];
	int n = snprintf(out, sizeof out, "{\n  \"relay\": \"%s\",\n  \"relay_token\": \"%s\"%s\n}\n", relayHost, relayToken, debugOn ? ",\n  \"debug\": true" : "");
	pd->file->write(f, out, (unsigned)n);
	pd->file->close(f);
}

/* ---------- menus ---------- */

static void open_setup(void);
static void refresh_setup(void);

static const struct { const char* name; const char* id; } POD_GENRES[] = {
	{ "Comedy", "1303" }, { "True Crime", "1488" }, { "News", "1489" }, { "Society & Culture", "1324" }, { "Sports", "1545" },
	{ "Technology", "1318" }, { "Business", "1321" }, { "Science", "1533" }, { "History", "1487" }, { "Health & Fitness", "1512" },
	{ "Education", "1304" }, { "Arts", "1301" }, { "Fiction", "1483" }, { "Kids & Family", "1305" }, { "Music", "1310" },
	{ "TV & Film", "1309" }, { "Leisure", "1502" }, { "Government", "1511" }, { "Religion", "1314" },
};

static void open_menu(const char* id)
{
	List* l;
	if (!strcmp(id, "radio")) {
		l = list_new("Radio");
		list_add2(l, "BBC World Service", "Live news, plays instantly", K_PLAY, BBC_WS);
		list_add2(l, "Top stations", "Most popular worldwide", K_RADIO_Q, "");
		list_add2(l, "Search stations", "By name", K_SEARCH, "radio");
		const char* tags[][3] = { {"Jazz","jazz","Smooth to bebop"}, {"Classical","classical","Orchestras and chamber"}, {"News","news","Headlines and talk"},
			{"Rock","rock","Guitars up"}, {"Pop","pop","Chart hits"}, {"Electronic","electronic","Beats and ambient"}, {"Chillout","chillout","Calm and downtempo"},
			{"Talk","talk","Conversation radio"}, {"Country","country","Nashville and beyond"}, {"Hip hop","hiphop","Rap and beats"},
			{"Reggae","reggae","Island sounds"}, {"Metal","metal","Loud and proud"} };
		for (int i = 0; i < 12; i++) { char a[64]; snprintf(a, sizeof a, "&tag=%s", tags[i][1]); list_add2(l, tags[i][0], tags[i][2], K_RADIO_Q, a); }
		const char* cc[][2] = { {"United Kingdom","GB"}, {"United States","US"}, {"Canada","CA"}, {"Australia","AU"}, {"Germany","DE"}, {"France","FR"}, {"Japan","JP"} };
		for (int i = 0; i < 7; i++) { char a[64]; snprintf(a, sizeof a, "&countrycode=%s", cc[i][1]); list_add2(l, cc[i][0], "Local stations", K_RADIO_Q, a); }
		list_add2(l, "More BBC stations", "Radio 1-6, 4 Extra...", K_MENU, "bbc");
	} else if (!strcmp(id, "tunein")) {
		l = list_new("TuneIn");
		list_add2(l, "Trending", "Popular right now", K_TUNEIN_LIST, TUNEIN "Browse.ashx?c=trending&render=json");
		list_add2(l, "Music", "By genre", K_TUNEIN_LIST, TUNEIN "Browse.ashx?c=music&render=json");
		list_add2(l, "News", "Live news radio", K_TUNEIN_LIST, TUNEIN "Browse.ashx?c=news&render=json");
		list_add2(l, "Talk", "Conversation and shows", K_TUNEIN_LIST, TUNEIN "Browse.ashx?c=talk&render=json");
		list_add2(l, "Sports", "Games and commentary", K_TUNEIN_LIST, TUNEIN "Browse.ashx?c=sports&render=json");
		list_add2(l, "By location", "Local radio near you", K_TUNEIN_LIST, TUNEIN "Browse.ashx?render=json&id=r0");
		list_add2(l, "By language", "Radio in your language", K_TUNEIN_LIST, TUNEIN "Browse.ashx?c=lang&render=json");
		list_add2(l, "Search TuneIn", "By station or topic", K_SEARCH, "tunein");
	} else if (!strcmp(id, "pod")) {
		l = list_new("Podcasts");
		list_add2(l, "Top charts", "What everyone is listening to", K_POD_CHARTS, "");
		list_add2(l, "Categories", "Comedy, true crime, news...", K_MENU, "podgenres");
		list_add2(l, "Search podcasts", "By show name", K_SEARCH, "pod");
	} else if (!strcmp(id, "podgenres")) {
		l = list_new("Categories");
		for (size_t i = 0; i < sizeof POD_GENRES / sizeof POD_GENRES[0]; i++) list_add2(l, POD_GENRES[i].name, "Top 20 shows", K_POD_GENRE, POD_GENRES[i].id);
	} else if (!strcmp(id, "library")) {
		l = list_new("Library");
		list_add2(l, "Old-time radio", "Dramas, comedy and mysteries", K_IA_Q, "collection:oldtimeradio");
		list_add2(l, "Audiobooks", "Free LibriVox readings", K_IA_Q, "collection:librivoxaudio");
		list_add2(l, "Radio programs", "Talks and broadcasts", K_IA_Q, "collection:radioprograms");
		list_add2(l, "Live concerts", "Taper-friendly bands", K_IA_Q, "collection:etree");
		list_add2(l, "Spoken word", "Lectures and readings", K_IA_Q, "collection:audio_bookspoetry");
		list_add2(l, "Search the library", "Millions of recordings", K_SEARCH, "lib");
	} else if (!strcmp(id, "bbc")) {
		l = list_new("BBC Radio");
		const char* st[][2] = { {"Radio 1","bbc_radio_one"}, {"Radio 2","bbc_radio_two"}, {"Radio 3","bbc_radio_three"},
			{"Radio 4","bbc_radio_fourfm"}, {"Radio 4 Extra","bbc_radio_four_extra"}, {"Radio 5 Live","bbc_radio_five_live"},
			{"6 Music","bbc_6music"}, {"Asian Network","bbc_asian_network"},
			{"Radio Scotland","bbc_radio_scotland_fm"}, {"Radio Wales","bbc_radio_wales_fm"}, {"Radio Ulster","bbc_radio_ulster"} };
		for (int i = 0; i < 11; i++) {
			char a[96], lab[64];
			snprintf(a, sizeof a, "relay:/bbc/%s.mp3", st[i][1]);
			snprintf(lab, sizeof lab, "BBC %s", st[i][0]);
			list_add2(l, lab, "Live (needs the relay)", K_PLAY, a);
		}
	} else if (!strcmp(id, "tv")) {
		l = list_new("TV");
		list_add2(l, "Pluto TV", "Free live channels", K_MENU, "pluto");
		list_add2(l, "Free live news", "DW, Al Jazeera, Red Bull TV", K_RELAY_LIST, "/tv/free");
		list_add2(l, "Classic TV and films", "Internet Archive", K_MENU, "archive");
		list_add2(l, "My IPTV playlist", "Your own channels", K_MENU, "iptv");
	} else if (!strcmp(id, "pluto")) {
		l = list_new("Pluto TV");
		list_add2(l, "Pluto Movies", "Movie channels, Westerns, Sci-Fi", K_RELAY_LIST, "/pluto/movies");
		list_add2(l, "News", "Live news channels", K_RELAY_LIST, "/pluto/channels?cat=News%20%2B%20Opinion");
		list_add2(l, "All categories", "Browse every genre", K_RELAY_LIST, "/pluto/categories");
		list_add2(l, "Search Pluto", "By channel name", K_SEARCH, "pluto");
	} else if (!strcmp(id, "yt")) {
		l = list_new("YouTube");
		list_add2(l, "Documentary channels", "Full episodes: Nat Geo, BBC Earth...", K_RELAY_LIST, "/tv/shows");
		list_add2(l, "Shorts", "Funny, animals, wildlife", K_MENU, "shorts");
		list_add2(l, "Nature and wildlife", "Documentaries", K_RELAY_LIST, "/yt/search?q=nature%20documentary");
		list_add2(l, "Science and space", "Explained", K_RELAY_LIST, "/yt/search?q=science%20space%20documentary");
		list_add2(l, "History", "Documentaries", K_RELAY_LIST, "/yt/search?q=history%20documentary");
		list_add2(l, "Comedy", "Stand-up and sketches", K_RELAY_LIST, "/yt/search?q=stand%20up%20comedy");
		list_add2(l, "Music", "Live sessions", K_RELAY_LIST, "/yt/search?q=live%20music%20session");
		list_add2(l, "Search YouTube", "Videos and episodes", K_SEARCH, "yt");
	} else if (!strcmp(id, "shorts")) {
		l = list_new("Shorts");
		list_add2(l, "Funny", "The good stuff", K_RELAY_LIST, "/yt/shorts?q=funny");
		list_add2(l, "Funny animals", "Pets and wildlife", K_RELAY_LIST, "/yt/shorts?q=funnyanimals");
		list_add2(l, "Cats", "Obviously", K_RELAY_LIST, "/yt/shorts?q=cats");
		list_add2(l, "Fails", "Ouch", K_RELAY_LIST, "/yt/shorts?q=fails");
		list_add2(l, "Wildlife", "Nature in 60 seconds", K_RELAY_LIST, "/yt/shorts?q=wildlife");
		list_add2(l, "Nat Geo", "From @NatGeo", K_RELAY_LIST, "/yt/shorts?q=%40NatGeo");
		list_add2(l, "Search Shorts", "A word or #tag", K_SEARCH, "shorts");
	} else if (!strcmp(id, "iptv")) {
		l = list_new("My IPTV");
		list_add2(l, "Search channels", "By name", K_SEARCH, "iptv");
		list_add2(l, "Groups", "Browse by category", K_RELAY_LIST, "/iptv/groups");
		list_add2(l, "All channels", "Everything in your playlist", K_RELAY_LIST, "/iptv/channels");
	} else if (!strcmp(id, "archive")) {
		l = list_new("Classic TV and films");
		list_add2(l, "Search", "Internet Archive video", K_SEARCH, "ia");
		list_add2(l, "Classic TV shows", "Westerns, sitcoms, serials", K_RELAY_LIST, "/ia/search?q=collection%3Aclassic_tv");
		list_add2(l, "Classic cartoons", "Golden-age animation", K_RELAY_LIST, "/ia/search?q=collection%3Aclassic_cartoons");
		list_add2(l, "NASA", "Missions and documentaries", K_RELAY_LIST, "/ia/search?q=collection%3Anasa");
		list_add2(l, "Vintage films", "Prelinger archive", K_RELAY_LIST, "/ia/search?q=collection%3Aprelinger");
		list_add2(l, "Feature films", "Public-domain movies", K_RELAY_LIST, "/ia/search?q=collection%3Afeature_films");
	} else { open_setup(); return; }
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
		static char name[96], url[400], codec[16], cc[8], arg[1100], sub[56];
		json_get_str(o, "name", name, sizeof name);
		json_get_str(o, "url_resolved", url, sizeof url);
		json_get_str(o, "codec", codec, sizeof codec);
		json_get_str(o, "countrycode", cc, sizeof cc);
		long br = json_get_int(o, "bitrate", 0);
		if (!name[0] || !url[0]) continue;
		bool mp3 = strcasecmp(codec, "MP3") == 0;
		if (mp3) snprintf(arg, sizeof arg, "%s", url);
		else if (relayHost[0]) via_relay(arg, sizeof arg, url);
		else continue;
		snprintf(sub, sizeof sub, "%s%s%s%s", cc, cc[0] ? "  " : "", br > 0 && br < 100000 ? "" : "", codec);
		if (br > 0 && br < 100000) snprintf(sub, sizeof sub, "%s%s%ldk %s%s", cc, cc[0] ? "  -  " : "", br, codec, mp3 ? "" : " via relay");
		list_add2(l, name, sub, K_PLAY, arg);
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
		static char name[96], id[24], artist[56];
		json_get_str(o, "name", name, sizeof name);
		json_get_str(o, "id", id, sizeof id);
		json_get_str(o, "artistName", artist, sizeof artist);
		if (name[0] && id[0]) list_add2(l, name, artist, K_POD_ID, id);
	}
	if (!l->n) { list_free(l); show_msg("Couldn't read the charts.", NULL); return; }
	push(l);
}

static void parse_genre_chart(Req* r)                      /* iTunes RSS chart: feed.entry[] with im:name / im:artist / id.attributes */
{
	const char* arr = json_array_start(r->buf, "entry");
	List* l = list_new(pendingTitle);
	const char* cur = arr; Span o;
	while (arr && l->n < MAX_ITEMS && json_next_object(&cur, r->buf + r->len, &o)) {
		static char name[96], artist[56], id[24];
		Span nm, ar, io, at;
		name[0] = artist[0] = id[0] = 0;
		if (json_get_obj(o, "im:name", &nm)) json_get_str(nm, "label", name, sizeof name);
		if (json_get_obj(o, "im:artist", &ar)) json_get_str(ar, "label", artist, sizeof artist);
		if (json_get_obj(o, "id", &io) && json_get_obj(io, "attributes", &at)) json_get_str(at, "im:id", id, sizeof id);
		if (name[0] && id[0]) list_add2(l, name, artist, K_POD_ID, id);
	}
	if (!l->n) { list_free(l); show_msg("Couldn't read that chart.", NULL); return; }
	push(l);
}

static void parse_search(Req* r)
{
	const char* arr = json_array_start(r->buf, "results");
	List* l = list_new(pendingTitle);
	const char* cur = arr; Span o;
	while (arr && l->n < MAX_ITEMS && json_next_object(&cur, r->buf + r->len, &o)) {
		static char name[96], feed[520], artist[56];
		json_get_str(o, "collectionName", name, sizeof name);
		json_get_str(o, "feedUrl", feed, sizeof feed);
		json_get_str(o, "artistName", artist, sizeof artist);
		if (name[0] && feed[0]) list_add2(l, name, artist, K_POD_FEED, feed);
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
		static char url[1000], title[96], arg[1100], date[40]; bool mp3;
		if (!rss_enclosure(item, url, sizeof url, &mp3)) continue;
		if (!mp3 && !relayHost[0]) { skipped++; continue; }
		if (!rss_tag_text(item, "title", title, sizeof title)) snprintf(title, sizeof title, "Episode");
		if (!rss_tag_text(item, "pubDate", date, sizeof date)) date[0] = 0;
		if (strlen(date) > 16) date[16] = 0;
		if (mp3) snprintf(arg, sizeof arg, "%s", url); else via_relay(arg, sizeof arg, url);
		list_add2(l, title, date, K_PLAY, arg);
	}
	if (!l->n) {
		list_free(l);
		show_msg(skipped ? "This show only has AAC/M4A episodes, which the Playdate can't play (the relay can convert them)." : "No episodes found.", NULL);
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

/* relay list: [{"name", "sub"?, "path", "video"?, "kind"?}] - "kind":"list" opens another list, "video" marks a TV item */
static void parse_relay_list(Req* r)
{
	const char* arr = json_array_start(r->buf, NULL);
	List* l = list_new(pendingTitle);
	const char* cur = arr; Span o;
	while (arr && l->n < MAX_ITEMS && json_next_object(&cur, r->buf + r->len, &o)) {
		static char name[120], sub[56], path[480], video[200], kind[16], a[500];
		json_get_str(o, "name", name, sizeof name);
		json_get_str(o, "sub", sub, sizeof sub);
		json_get_str(o, "path", path, sizeof path);
		json_get_str(o, "video", video, sizeof video);
		json_get_str(o, "kind", kind, sizeof kind);
		if (!name[0] || !path[0]) continue;
		if (!strcmp(kind, "list")) { list_add2(l, name, sub, K_RELAY_LIST, path); continue; }
		if (video[0]) { snprintf(a, sizeof a, "relay:%s", video); list_add2(l, name, sub, K_PLAY_VIDEO, a); }
		else { snprintf(a, sizeof a, "relay:%s", path); list_add2(l, name, sub, K_PLAY, a); }
	}
	if (!l->n) { list_free(l); show_msg("Nothing found.", NULL); return; }
	push(l);
}

/* ---- TuneIn, straight from its public directory (no relay) ---- */

static void add_tunein_obj(List* l, Span o)
{
	static char type[16], text[96], subtext[56], id[24], url[300], formats[40];
	json_get_str(o, "type", type, sizeof type);
	json_get_str(o, "text", text, sizeof text);
	if (!text[0]) return;
	if (!strcmp(type, "link")) {
		json_get_str(o, "URL", url, sizeof url);
		if (!url[0] || l->n >= MAX_ITEMS) return;
		static char full[360]; snprintf(full, sizeof full, "%s%srender=json", url, strchr(url, '?') ? "&" : "?");
		list_add2(l, text, "", K_TUNEIN_LIST, full);
	} else if (!strcmp(type, "audio")) {
		json_get_str(o, "guide_id", id, sizeof id);
		json_get_str(o, "subtext", subtext, sizeof subtext);
		json_get_str(o, "formats", formats, sizeof formats);
		if (id[0] != 's' || l->n >= MAX_ITEMS) return;                   /* stations only */
		if (!relayHost[0] && !strstr(formats, "mp3")) return;           /* AAC/HLS stations need the relay */
		list_add2(l, text, subtext, K_TUNEIN_PLAY, id);
	}
}

static void parse_tunein(Req* r)
{
	const char* arr = json_array_start(r->buf, "body");
	List* l = list_new(pendingTitle);
	const char* cur = arr; Span o;
	while (arr && l->n < MAX_ITEMS && json_next_object(&cur, r->buf + r->len, &o)) {
		Span kids;
		if (json_get_arr(o, "children", &kids)) {
			const char* kc = kids.s; Span k;
			while (l->n < MAX_ITEMS && json_next_object(&kc, kids.s + kids.n, &k)) add_tunein_obj(l, k);
		} else add_tunein_obj(l, o);
	}
	if (!l->n) { list_free(l); show_msg("Nothing playable here. (Some stations need the relay.)", NULL); return; }
	push(l);
}

static char tuneTitle[80];

static void parse_tunein_tune(Req* r)
{
	const char* arr = json_array_start(r->buf, "body");
	const char* cur = arr; Span o;
	while (arr && json_next_object(&cur, r->buf + r->len, &o)) {
		static char url[700], media[16], arg[1100];
		json_get_str(o, "url", url, sizeof url);
		json_get_str(o, "media_type", media, sizeof media);
		if (!url[0]) continue;
		if (!strcmp(media, "mp3") || strstr(url, ".mp3")) { start_playback_hook(tuneTitle, url); return; }
		if (relayHost[0]) { via_relay(arg, sizeof arg, url); start_playback_hook(tuneTitle, arg); return; }
		need_relay("This station (it streams AAC)");
		return;
	}
	show_msg("TuneIn didn't return a stream for this station.", NULL);
}

/* ---- Internet Archive audio, straight from archive.org (no relay) ---- */

static char iaId[80], iaTitle[40];

static void parse_ia_list(Req* r)
{
	const char* arr = json_array_start(r->buf, "docs");
	List* l = list_new(pendingTitle);
	const char* cur = arr; Span o;
	while (arr && l->n < MAX_ITEMS && json_next_object(&cur, r->buf + r->len, &o)) {
		static char id[80], title[96], creator[56];
		json_get_str(o, "identifier", id, sizeof id);
		json_get_str(o, "title", title, sizeof title);
		json_get_str(o, "creator", creator, sizeof creator);
		if (id[0]) list_add2(l, title[0] ? title : id, creator, K_IA_ITEM, id);
	}
	if (!l->n) { list_free(l); show_msg("Nothing found in the library.", NULL); return; }
	push(l);
}

static void fmt_length(const char* len, char* out, size_t sz)
{
	out[0] = 0;
	if (!len[0]) return;
	if (strchr(len, ':')) { snprintf(out, sz, "%s", len); return; }
	int s = atoi(len);
	if (s <= 0) return;
	if (s >= 3600) snprintf(out, sz, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60); else snprintf(out, sz, "%d:%02d", s / 60, s % 60);
}

static void parse_ia_files(Req* r)
{
	const char* arr = json_array_start(r->buf, "files");
	static char fmtWant[24];
	fmtWant[0] = 0;
	/* an item usually holds several MP3 derivatives of the same audio: keep one family ("VBR MP3" if present) */
	for (int pass = 0; pass < 2 && arr; pass++) {
		const char* cur = arr; Span o;
		while (json_next_object(&cur, r->buf + r->len, &o)) {
			static char f[24];
			json_get_str(o, "format", f, sizeof f);
			if (!strstr(f, "MP3")) continue;
			if (pass == 0 && !strcmp(f, "VBR MP3")) { snprintf(fmtWant, sizeof fmtWant, "%s", f); break; }
			if (pass == 1 && !fmtWant[0]) { snprintf(fmtWant, sizeof fmtWant, "%s", f); break; }
		}
		if (fmtWant[0]) break;
	}
	List* l = list_new(pendingTitle);
	if (arr && fmtWant[0]) {
		const char* cur = arr; Span o;
		while (l->n < MAX_ITEMS && json_next_object(&cur, r->buf + r->len, &o)) {
			static char f[24], name[160], title[96], length[16], sub[24], enc[480], url[560];
			json_get_str(o, "format", f, sizeof f);
			if (strcmp(f, fmtWant)) continue;
			json_get_str(o, "name", name, sizeof name);
			json_get_str(o, "title", title, sizeof title);
			json_get_str(o, "length", length, sizeof length);
			if (!name[0]) continue;
			fmt_length(length, sub, sizeof sub);
			urlenc(name, enc, sizeof enc);
			snprintf(url, sizeof url, "https://archive.org/download/%s/%s", iaId, enc);
			if (!title[0]) { snprintf(title, sizeof title, "%s", name); char* dot = strrchr(title, '.'); if (dot) *dot = 0; }
			list_add2(l, title, sub, K_PLAY, url);
		}
	}
	if (!l->n) { list_free(l); show_msg("No playable audio in this item.", NULL); return; }
	if (l->n == 1) { static char t[80], a[1100]; snprintf(t, sizeof t, "%s", l->items[0].label); snprintf(a, sizeof a, "%s", l->items[0].arg); list_free(l); start_playback_hook(t, a); return; }
	push(l);
}

static void parse_health(Req* r)
{
	Span o = { r->buf, r->len };
	const char* p = strchr(r->buf, '{'); if (p) { o.s = p; o.n = r->len - (size_t)(p - r->buf); }
	static char ok[8], video[8], yt[8];
	json_get_str(o, "video", video, sizeof video);
	json_get_str(o, "yt", yt, sizeof yt);
	bool vid = !strcmp(video, "yes"), ytok = !strcmp(yt, "yes");
	(void)ok;
	static char chatv[8]; json_get_str(o, "chat", chatv, sizeof chatv);
	snprintf(lastTest, sizeof lastTest, "Connected%s%s%s", vid ? " - video ready" : " - audio only", ytok ? ", YouTube on" : "", !strcmp(chatv, "yes") ? ", AI chat on" : "");
	refresh_setup();
	show_card(IC_CHECK, "Relay connected", lastTest, 0);
}

/* ---------- playback ---------- */

static Ring audioRing;                 /* video mode: demuxed MP3 for the audio thread */
static bool videoActive;
typedef struct { uint32_t pts; bool key; uint32_t len; uint8_t* data; } VFrame;
static VFrame vq[VQ_MAX];
static int vqHead, vqCount;
static uint32_t vqBytes;
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
	vqHead = vqCount = 0; vqBytes = 0;
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

static char playTitle[80], playArg[1100], playSub[56];
static List* playList;                 /* the list the current item came from: Up/Down in the player = next/previous */
static uint32_t overlayUntil;          /* channel-name banner; armed when the first picture appears */
static bool overlayDrawn, overlayArmed;
static bool triedRelay;

static bool playOnDemand;              /* ends by itself (episode, track) vs. live (reconnect when it drops) */
static int resumeCount;

static void start_playback(const char* title, const char* arg, bool video)
{
	static char url[URL_MAX];
	stop_playback();
	snprintf(playTitle, sizeof playTitle, "%s", title);
	snprintf(playArg, sizeof playArg, "%s", arg);
	if (!strncmp(arg, "relay:", 6)) {
		if (!relayHost[0]) { need_relay("This stream"); return; }
		relay_url(url, sizeof url, arg + 6);
	} else snprintf(url, sizeof url, "%s", arg);
	copy_ascii(nowTitle, sizeof nowTitle, title);
	copy_ascii(nowSub, sizeof nowSub, playSub);
	playOnDemand = strstr(url, ".mp3") || strstr(url, "archive.org") || strstr(url, "/ia/") || strstr(url, "/yt/");
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

/* The "next system": starting a stream is queued, never immediate. The previous stream is torn down at once, rapid presses (surfing,
   Up/Down, Next) overwrite each other so only the LAST one starts, and it starts after a short gap so the old connection has really
   closed. That keeps us at one stream + one fetch at a time instead of piling up connections and buffers. */
static struct { bool pending; char title[80], arg[1100], sub[56]; bool video; uint32_t at; } playReq;

static void request_play(const char* title, const char* arg, bool video)
{
	stop_playback();
	if (pending) { net_free(pending); pending = NULL; }                 /* a list fetch in flight would hold a connection too */
	snprintf(playReq.title, sizeof playReq.title, "%s", title);
	snprintf(playReq.arg, sizeof playReq.arg, "%s", arg);
	snprintf(playReq.sub, sizeof playReq.sub, "%s", playSub);
	playReq.video = video; playReq.pending = true; playReq.at = now_ms() + 320;
	copy_ascii(nowTitle, sizeof nowTitle, title);
	copy_ascii(nowSub, sizeof nowSub, playSub);
	started = false;
	mode = video ? M_VIDEO : M_PLAY;
}

static void play_queue_tick(uint32_t now)
{
	if (!playReq.pending || now < playReq.at) return;
	playReq.pending = false;
	snprintf(playSub, sizeof playSub, "%s", playReq.sub);
	start_playback(playReq.title, playReq.arg, playReq.video);
}

static void start_playback_hook(const char* title, const char* arg)       /* TuneIn / Library: direct streams */
{
	triedRelay = false; resumeCount = 0;
	request_play(title, arg, false);
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
			if (vqCount >= VQ_MAX || vqBytes > VQ_BYTES) break;                 /* back-pressure: stop reading, the relay waits */
			uint8_t pre[5];
			ring_read(&stream->ring, NULL, 3);
			ring_read(&stream->ring, pre, 5);
			VFrame* f = &vq[(vqHead + vqCount) % VQ_MAX];
			f->pts = pre[0] | (pre[1] << 8) | (pre[2] << 16) | ((uint32_t)pre[3] << 24);
			f->key = pre[4] & 1;
			f->len = len - 5;
			f->data = xalloc(f->len ? f->len : 1);
			if (!f->data) { ring_read(&stream->ring, NULL, (int)f->len); continue; }
			ring_read(&stream->ring, f->data, (int)f->len);
			vqBytes += f->len;
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
		xfree(f->data); vqBytes = vqBytes > f->len ? vqBytes - f->len : 0;
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
			Tm(fBold, nowTitle, 8, 11, kDrawModeFillWhite);
			if (playList) Tmr(fSmall, "Up/Down: next", 392, 11, kDrawModeFillWhite);
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
static int16_t chatLineW[CHAT_LINES];
static int chatLineN;
static int chatMsgFirst[CHAT_MAX], chatMsgCount[CHAT_MAX], chatMsgW[CHAT_MAX];
static int chatScroll;                    /* pixels */
static bool chatDirty = true, chatStick = true;
static int chatWrapW = 300, chatWrapBuilt = 300;

static void chat_add(bool user, const char* text)
{
	if (chatN == CHAT_MAX) {                                     /* drop the oldest */
		memmove(chatText[0], chatText[1], sizeof chatText[0] * (CHAT_MAX - 1));
		memmove(&chatUser[0], &chatUser[1], CHAT_MAX - 1);
		chatN--;
	}
	snprintf(chatText[chatN], sizeof chatText[0], "%s", text);
	chatUser[chatN++] = user;
	chatDirty = true; chatStick = true;
}

static void chat_add_line(const char* s, size_t n)
{
	if (chatLineN >= CHAT_LINES) return;
	snprintf(chatLines[chatLineN], sizeof chatLines[0], "%.*s", (int)n, s);
	chatLineW[chatLineN] = (int16_t)TW(fBody, chatLines[chatLineN]);
	chatLineN++;
}

/* greedy word wrap by pixel width */
static void chat_wrap(const char* text, int maxw)
{
	static char line[80], trial[160], word[80];
	line[0] = 0;
	const char* p = text;
	while (*p) {
		if (*p == '\n') { chat_add_line(line, strlen(line)); line[0] = 0; p++; continue; }
		const char* e = p; while (*e && *e != ' ' && *e != '\n') e++;
		size_t wl = (size_t)(e - p); if (wl > 60) wl = 60;
		snprintf(word, sizeof word, "%.*s", (int)wl, p);
		snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word);
		if (TW(fBody, trial) > maxw && line[0]) { chat_add_line(line, strlen(line)); snprintf(line, sizeof line, "%s", word); }
		else snprintf(line, sizeof line, "%.78s", trial);
		p = e; while (*p == ' ') p++;
	}
	if (line[0]) chat_add_line(line, strlen(line));
}

static void chat_rebuild(void)
{
	chatLineN = 0;
	for (int i = 0; i < chatN; i++) {
		chatMsgFirst[i] = chatLineN;
		chat_wrap(chatText[i], chatWrapW);
		chatMsgCount[i] = chatLineN - chatMsgFirst[i];
		chatMsgW[i] = 0;
		for (int k = chatMsgFirst[i]; k < chatLineN; k++) if (chatLineW[k] > chatMsgW[i]) chatMsgW[i] = chatLineW[k];
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

static char chatBody[9000];
static bool chatViaRelay;

static void chat_send(const char* text)
{
	static char* body = chatBody;
	static char esc[1000];
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
	static char curl[500];
	chatViaRelay = relayHost[0] != 0;                                    /* the relay forwards to the Vercel AI Gateway (key stays on the server) */
	chatReq = net_post(chatViaRelay ? relay_url(curl, sizeof curl, "/chat") : POLLINATIONS, body, 24 * 1024, on_fetched, NULL);
	if (!chatReq) chat_add(false, "(out of memory)");
	mode = M_CHAT;
}

static void chat_poll(void)
{
	if (!chatReq || !chatReq->finished) return;
	Req* r = chatReq; chatReq = NULL;
	if (chatViaRelay && (r->status < 0 || r->status >= 400 || r->len == 0)) {      /* relay chat unavailable: fall back to pollinations.ai */
		chatViaRelay = false;
		net_free(r);
		chatReq = net_post(POLLINATIONS, chatBody, 24 * 1024, on_fetched, NULL);
		return;
	}
	if (r->status == 429) chat_add(false, "(The AI is busy - wait a few seconds and try again)");
	else if (r->status < 0 || r->status >= 400 || r->len == 0) {
		static char m[120]; snprintf(m, sizeof m, "(couldn't reach the AI: %s)", r->error[0] ? r->error : "no answer");
		chat_add(false, m);
	} else parse_chat_reply(r);
	net_free(r);
	chatStick = true;
}

static void open_chat(void)
{
	if (!chatN) chat_add(false, "Hi! I'm Airwave's assistant, powered by pollinations.ai. Press A to type a question.");
	mode = M_CHAT;
	chatStick = true;
}


/* ---------- actions ---------- */

static void open_text(const char* source, int max)
{
	snprintf(textSource, sizeof textSource, "%s", source);
	textLen = 0; textBuf[0] = 0; textMax = max;
	kbWant = kbActive = true;                                    /* Lua shows the system keyboard on its next frame */
	kbCaps = !strcmp(source, "chat");                            /* sentences for chat, plain for the rest */
	if (strcmp(source, "chat")) mode = M_TEXT;                   /* chat compose happens inside the chat screen */
}

static void ia_query(const char* q, const char* title)
{
	static char url[900], raw[300], enc[420];
	snprintf(raw, sizeof raw, "(%s) AND mediatype:audio", q);
	urlenc(raw, enc, sizeof enc);
	snprintf(url, sizeof url, "https://archive.org/advancedsearch.php?q=%s&fl%%5B%%5D=identifier&fl%%5B%%5D=title&fl%%5B%%5D=creator&rows=40&sort%%5B%%5D=downloads+desc&output=json", enc);
	start_fetch(url, 96 * 1024, parse_ia_list, title);
}

/* source: radio | pod | tunein | lib (direct) ... iptv | yt | ia | pluto | shorts (relay) */
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
	} else if (!strcmp(source, "tunein")) {
		snprintf(url, sizeof url, "%sSearch.ashx?query=%s&render=json", TUNEIN, enc);
		start_fetch(url, 96 * 1024, parse_tunein, query);
	} else if (!strcmp(source, "lib")) {
		ia_query(query, query);
	} else {
		if (!relayHost[0]) { need_relay("Searching here"); return; }
		const char* route = !strcmp(source, "yt") ? "/yt/search?q=%s" : !strcmp(source, "ia") ? "/ia/search?q=%s" :
			!strcmp(source, "pluto") ? "/pluto/channels?q=%s" : !strcmp(source, "shorts") ? "/yt/shorts?q=%s" : "/iptv/channels?q=%s";
		snprintf(path, sizeof path, route, enc);
		relay_url(url, sizeof url, path);
		start_fetch(url, 64 * 1024, parse_relay_list, query);
	}
}

static void run_relay_test(void)
{
	static char url[400];
	if (!relayHost[0]) { need_relay("Testing"); return; }
	snprintf(lastTest, sizeof lastTest, "Testing...");
	relay_url(url, sizeof url, "/health");
	start_fetch(url, 4096, parse_health, "Testing the relay");
}

static void build_setup(List* l)
{
	static char wifi[60];
	snprintf(wifi, sizeof wifi, "%s", pd->network->getStatus() == kWifiConnected ? "Connected" : "Not connected");
	list_add2(l, "Relay address", relayHost[0] ? relayHost : "Not set - press A to enter", K_RELAY_EDIT, "");
	list_add2(l, "Test the relay", lastTest, K_RELAY_TEST, "");
	list_add2(l, "Relay token", relayToken[0] ? "Set" : "None (optional)", K_TOKEN_EDIT, "");
	list_add2(l, "Wi-Fi", wifi, K_INFO, "Wi-Fi is set up in the Playdate's own Settings. Airwave connects to it automatically and turns it on when needed.");
	list_add2(l, "What needs the relay?", "TV, Shorts, BBC stations", K_INFO,
		"TV and video, YouTube Shorts and most BBC stations are converted by a small helper server (the relay). "
		"Everything else works on its own: radio, podcasts, TuneIn, the Library, BBC World Service and AI Chat.");
	list_add2(l, "About Airwave", "Version " VERSION, K_INFO, "Airwave for Playdate. Radio, podcasts, TV and AI chat in C, with the system keyboard. "
		"Chat by pollinations.ai, stations by radio-browser and TuneIn, audio from the Internet Archive.");
}

static void open_setup(void) { List* l = list_new("Setup"); build_setup(l); push(l); }

static void refresh_setup(void)                                 /* redraw the Setup page after a change, keeping the cursor */
{
	if (!depth || strcmp(stack[depth - 1]->title, "Setup")) return;
	List* old = stack[depth - 1];
	List* l = list_new("Setup");
	build_setup(l);
	l->sel = old->sel; l->selY = old->selY; l->scrollY = old->scrollY; l->primed = old->primed;
	stack[depth - 1] = l;
	list_free(old);
}

static void home_open(int idx)
{
	switch (idx) {
		case 0: open_menu("radio"); break;
		case 1: open_menu("tunein"); break;
		case 2: open_menu("pod"); break;
		case 3: open_menu("library"); break;
		case 4: if (!relayHost[0]) need_relay("TV"); else open_menu("tv"); break;
		case 5: if (!relayHost[0]) need_relay("YouTube"); else open_menu("yt"); break;
		case 6: if (!chatN) chat_add(false, "Hi! I'm Airwave's assistant. Press A to type a question."); mode = M_CHAT; chatStick = true; break;
		case 7: open_text("all", 27); break;
		default: open_setup(); break;
	}
}

static void activate(Item* it, bool altAction)
{
	static char url[URL_MAX];
	if (item_locked(it)) { need_relay(it->kind == K_PLAY_VIDEO ? "TV and video" : it->label); return; }
	switch (it->kind) {
		case K_MENU: open_menu(it->arg); break;
		case K_RADIO_Q:
			snprintf(url, sizeof url, "%s%s%s", RB_BASE, relayHost[0] ? "" : "&codec=MP3", it->arg);
			start_fetch(url, 160 * 1024, parse_radio, it->label); break;
		case K_SEARCH: open_text(it->arg, 27); break;
		case K_SEARCH_ALL: open_text("all", 27); break;
		case K_SEARCH_IN: run_search(it->arg, searchQuery); break;
		case K_POD_CHARTS: start_fetch(APPLE_CHARTS, 96 * 1024, parse_charts, "Top podcasts"); break;
		case K_POD_GENRE:
			snprintf(url, sizeof url, "https://itunes.apple.com/us/rss/toppodcasts/limit=20/genre=%s/json", it->arg);
			start_fetch(url, 256 * 1024, parse_genre_chart, it->label); break;
		case K_POD_ID:
			snprintf(url, sizeof url, "https://itunes.apple.com/lookup?id=%s", it->arg);
			start_fetch(url, 64 * 1024, parse_lookup, it->label); break;
		case K_POD_FEED: start_fetch(it->arg, 320 * 1024, parse_episodes, it->label); break;
		case K_RELAY_LIST: {
			char title[40];
			if (strstr(it->arg, "offset=") || strstr(it->arg, "page=")) { snprintf(title, sizeof title, "%s", stack[depth - 1]->title); if (depth > 1) pop(); }   /* "More..." replaces the page */
			else snprintf(title, sizeof title, "%s", it->label);
			relay_url(url, sizeof url, it->arg);
			start_fetch(url, 96 * 1024, parse_relay_list, title); break;
		}
		case K_PLAY: playList = depth ? stack[depth - 1] : NULL; snprintf(playSub, sizeof playSub, "%s", it->sub); triedRelay = false; resumeCount = 0; request_play(it->label, it->arg, false); break;
		case K_PLAY_VIDEO:
			triedRelay = true; videoFellBack = altAction;                 /* Right button = audio only */
			playList = depth ? stack[depth - 1] : NULL; snprintf(playSub, sizeof playSub, "%s", it->sub);
			resumeCount = 0;
			if (altAction) { static char a[1100]; audio_arg(it->arg, a, sizeof a); request_play(it->label, a, false); }
			else request_play(it->label, it->arg, true);
			break;
		case K_TUNEIN_LIST: start_fetch(it->arg, 96 * 1024, parse_tunein, it->label); break;
		case K_TUNEIN_PLAY:
			snprintf(tuneTitle, sizeof tuneTitle, "%s", it->label); playList = depth ? stack[depth - 1] : NULL; snprintf(playSub, sizeof playSub, "%s", it->sub);
			snprintf(url, sizeof url, "%sTune.ashx?id=%s&render=json", TUNEIN, it->arg);
			start_fetch(url, 8192, parse_tunein_tune, "Tuning in"); break;
		case K_IA_Q: ia_query(it->arg, it->label); break;
		case K_IA_ITEM:
			snprintf(iaId, sizeof iaId, "%s", it->arg); snprintf(iaTitle, sizeof iaTitle, "%s", it->label);
			snprintf(url, sizeof url, "https://archive.org/metadata/%s", it->arg);
			start_fetch(url, 192 * 1024, parse_ia_files, it->label); break;
		case K_RELAY_EDIT: open_text("relay", 100); break;
		case K_TOKEN_EDIT: open_text("token", 40); break;
		case K_RELAY_TEST: run_relay_test(); break;
		case K_INFO: show_card(IC_INFO, it->label, it->arg, 0); break;
		default: break;
	}
}

/* next/previous item (same kind) in the list the current stream came from; -1 if none */
static int next_index(int dir)
{
	List* l = playList;
	if (!l) return -1;
	bool stillThere = false;
	for (int i = 0; i < depth; i++) if (stack[i] == l) stillThere = true;
	if (!stillThere || !l->n) return -1;
	Kind want = l->items[l->sel].kind;
	for (int i = l->sel + dir; i >= 0 && i < l->n; i += dir) if (l->items[i].kind == want) return i;
	return -1;
}

/* channel surfing / skip: Up/Down or Left/Right in the player */
static void surf(int dir)
{
	int i = next_index(dir);
	if (i < 0) return;
	playList->sel = i;
	activate(&playList->items[i], false);
}

static void submit_text(void)
{
	if (textLen == 0 && strcmp(textSource, "relay") && strcmp(textSource, "token")) { mode = depth ? M_LIST : M_HOME; return; }
	textBuf[textLen] = 0;
	if (!strcmp(textSource, "chat")) { char t[128]; snprintf(t, sizeof t, "%s", textBuf); chat_send(t); return; }
	if (!strcmp(textSource, "relay")) {
		static char t[128]; int k = 0;
		for (int i = 0; textBuf[i] && k < 120; i++) if (textBuf[i] != ' ') t[k++] = textBuf[i];
		t[k] = 0;
		while (k > 0 && t[k - 1] == '/') t[--k] = 0;
		if (k && strncmp(t, "http", 4) && !strchr(t, ':')) { snprintf(t + k, sizeof t - k, ":8787"); }     /* bare IP: default port */
		snprintf(relayHost, sizeof relayHost, "%s", t);
		save_settings();
		snprintf(lastTest, sizeof lastTest, "%s", relayHost[0] ? "Not tested yet" : "No relay set");
		mode = M_LIST; refresh_setup();
		if (relayHost[0]) run_relay_test();
		return;
	}
	if (!strcmp(textSource, "token")) {
		snprintf(relayToken, sizeof relayToken, "%s", textBuf);
		save_settings(); mode = M_LIST; refresh_setup();
		if (relayHost[0]) run_relay_test();
		return;
	}
	snprintf(searchQuery, sizeof searchQuery, "%s", textBuf);
	if (strcmp(textSource, "all")) { run_search(textSource, searchQuery); return; }
	char title[40]; snprintf(title, sizeof title, "Search: %.28s", searchQuery);          /* search everywhere: pick where */
	List* l = list_new(title);
	list_add2(l, "Radio stations", "Worldwide directory", K_SEARCH_IN, "radio");
	list_add2(l, "Podcasts", "Apple podcasts", K_SEARCH_IN, "pod");
	list_add2(l, "TuneIn", "Stations and shows", K_SEARCH_IN, "tunein");
	list_add2(l, "Library", "Audiobooks and old radio", K_SEARCH_IN, "lib");
	if (relayHost[0]) {
		list_add2(l, "Live TV channels", "Pluto TV", K_SEARCH_IN, "pluto");
		list_add2(l, "YouTube", "Videos and episodes", K_SEARCH_IN, "yt");
		list_add2(l, "YouTube Shorts", "Short clips", K_SEARCH_IN, "shorts");
		list_add2(l, "Classic TV and films", "Internet Archive", K_SEARCH_IN, "ia");
		list_add2(l, "My IPTV channels", "Your playlist", K_SEARCH_IN, "iptv");
	}
	push(l);
}

/* ---------- screens: player, typing, chat ---------- */

static void draw_play(void)
{
	ui_header("Now playing", "");
	ui_art(14, 42, 114, nowTitle, started);
	int x = 142, w = viewW - x - 14;
	if (w > 40) {
		int cx = x;
		cx += ui_chip(cx, 42, started ? "PLAYING" : "TUNING", started) + 6;
		if (playList) ui_chip(cx, 42, playOnDemand ? "QUEUE" : "CHANNELS", false);
		int lines = ui_wrap(nowTitle, x, 80, w, 2, fBig, 25, kDrawModeFillBlack);
		if (nowSub[0]) Tm(fSmall, nowSub, x, 80 + (lines - 1) * 25 + 22, kDrawModeFillBlack);
		ui_eq(x, 142, w > 130 ? 130 : w, 24, started);
	}
	char st[96]; uint32_t t = now_ms();
	if (playReq.pending) snprintf(st, sizeof st, "Tuning...");
	else if (!stream) snprintf(st, sizeof st, "Stopped");
	else if (stream->status < 0) snprintf(st, sizeof st, "%s", !strcmp(stream->error, "stream ended") ? "Finished" : !strcmp(stream->error, "HTTP 502") ? "The relay couldn't open it" : stream->error);
	else if (stream->total == 0) snprintf(st, sizeof st, "Connecting...");
	else if (!started) snprintf(st, sizeof st, "Buffering %u%%", (unsigned)(ring_fill(&stream->ring) * 100 / PREBUFFER));
	else { int s = (int)((t - startedAt) / 1000); snprintf(st, sizeof st, "Playing  %d:%02d", s / 60, s % 60); }
	Tm(fBold, st, 14, 173, kDrawModeFillBlack);
	if (debugOn && stream) { static char d[200]; snprintf(d, sizeof d, "st%d hdr%d c%d cl%d r%d tot%u e:%s %.60s", stream->status, stream->headers, stream->complete, stream->closed, stream->redirects, (unsigned)stream->total, stream->error, stream->url + 8); Tm(fSmall, d, 4, 120, kDrawModeFillBlack); }
	if (stream) {
		static char kb[32]; snprintf(kb, sizeof kb, "%u KB", (unsigned)stream->total / 1024);
		Tmr(fSmall, kb, viewW - 14, 173, kDrawModeFillBlack);
		pd->graphics->drawRoundRect(14, 203, viewW - 28, 10, 5, 2, kColorBlack);
		int fw = (int)((uint64_t)ring_fill(&stream->ring) * (viewW - 36) / stream->ring.size);
		if (fw > 2) pd->graphics->fillRoundRect(18, 206, fw, 4, 2, kColorBlack);
	}
	int ni = next_index(1);
	if (ni >= 0) {                                                       /* the "next system": what plays after this */
		static char un[96]; snprintf(un, sizeof un, "Up next: %.60s", playList->items[ni].label);
		while (strlen(un) > 12 && TW(fSmall, un) > viewW - 28) un[strlen(un) - 1] = 0;
		Tm(fSmall, un, 14, 190, kDrawModeFillBlack);
	}
	ui_hints(playList ? "B Stop|LR Previous or next" : "B Stop");
}

static const char* search_title(void)
{
	static const char* titles[][2] = { {"radio","Search radio"}, {"pod","Search podcasts"}, {"tunein","Search TuneIn"}, {"lib","Search the library"},
		{"iptv","Search my IPTV"}, {"yt","Search YouTube"}, {"ia","Search classic TV"}, {"all","Search everything"}, {"chat","Message the AI"},
		{"pluto","Search live channels"}, {"shorts","Search Shorts"}, {"relay","Relay address"}, {"token","Relay token"} };
	for (int i = 0; i < 13; i++) if (!strcmp(titles[i][0], textSource)) return titles[i][1];
	return "Search";
}

/* what you are typing, wrapped to `w` pixels; cy = centre of the first line; returns the number of lines drawn */
static int draw_typed(int x, int cy, int w, int maxLines, int lineH)
{
	static char shown[160], seg[100];
	snprintf(shown, sizeof shown, "%.*s", textLen, textBuf);
	int n = (int)strlen(shown), pos = 0, line = 0, endx = x;
	while ((pos < n || line == 0) && line < maxLines) {
		int cut = n - pos;
		while (cut > 0 && TW(fBody, (snprintf(seg, sizeof seg, "%.*s", cut > 90 ? 90 : cut, shown + pos), seg)) > w) cut--;
		if (cut == 0 && pos < n) cut = 1;
		if (pos + cut < n && cut > 4) { int k = cut; while (k > 1 && shown[pos + k] != ' ') k--; if (k > 3) cut = k; }
		snprintf(seg, sizeof seg, "%.*s", cut, shown + pos);
		Tm(fBody, seg, x, cy + line * lineH, kDrawModeFillBlack);
		endx = x + TW(fBody, seg);
		pos += cut; while (shown[pos] == ' ') pos++;
		line++;
		if (pos >= n) break;
	}
	if ((now_ms() / 450) & 1) pd->graphics->fillRect(endx + 2, cy + (line > 0 ? line - 1 : 0) * lineH - 8, 2, 16, kColorBlack);
	return line;
}

static void draw_text_screen(void)
{
	char cnt[16]; snprintf(cnt, sizeof cnt, "%d/%d", textLen, textMax);
	ui_header(search_title(), cnt);
	pd->graphics->drawRoundRect(10, 46, viewW - 20, 88, 12, 3, kColorBlack);
	if (textLen == 0) ui_wrap(kbVisible ? "Type with the keyboard..." : "Opening keyboard...", 26, 68, viewW - 56, 2, fBody, 22, kDrawModeFillBlack);
	else draw_typed(26, 68, viewW - 56, 3, 24);
	const char* tip = !strcmp(textSource, "relay") ? "Example: 192.168.1.20:8787 or https://name.trycloudflare.com" :
		!strcmp(textSource, "token") ? "The same secret you started the relay with." :
		!strcmp(textSource, "all") ? "Next you choose where to search." : "OK searches, B deletes.";
	ui_wrap(tip, 14, 154, viewW - 28, 3, fSmall, 16, kDrawModeFillBlack);
}

static void draw_chat(void)
{
	bool typing = kbActive && !strcmp(textSource, "chat");
	char cnt[16]; snprintf(cnt, sizeof cnt, "%d/%d", textLen, textMax);
	ui_header("AI chat", typing ? cnt : "");
	int maxText = viewW - 56 - 24;                                       /* the transcript re-wraps to whatever width the keyboard leaves */
	chatWrapW = maxText;
	if (chatWrapW != chatWrapBuilt && (abs(chatWrapW - chatWrapBuilt) >= 6 || viewW == 400)) { chatDirty = true; chatWrapBuilt = chatWrapW; }
	if (chatDirty) chat_rebuild();
	int top = 40, bottom = typing ? 160 : 214, areaH = bottom - top;
	int total = 0;
	for (int i = 0; i < chatN; i++) total += chatMsgCount[i] * 17 + 16 + 8;
	if (chatReq) total += 38;
	int maxScroll = total > areaH ? total - areaH : 0;
	if (chatStick || typing) { chatScroll = maxScroll; chatStick = false; }
	if (chatScroll > maxScroll) chatScroll = maxScroll;
	if (chatScroll < 0) chatScroll = 0;
	pd->graphics->setClipRect(0, top, viewW, areaH);
	int y = top - chatScroll;
	for (int i = 0; i < chatN; i++) {
		int h = chatMsgCount[i] * 17 + 16, bw = chatMsgW[i] + 26;
		int x = chatUser[i] ? viewW - 12 - bw : 12;
		if (y + h > top - 2 && y < bottom + 2) {
			ui_bubble(x, y, bw, h, chatUser[i]);
			for (int k = 0; k < chatMsgCount[i]; k++)
				Tm(fBody, chatLines[chatMsgFirst[i] + k], x + 13, y + 16 + k * 17, chatUser[i] ? kDrawModeFillWhite : kDrawModeFillBlack);
		}
		y += h + 8;
	}
	if (chatReq) {
		ui_bubble(12, y, 62, 30, false);
		int ph = (int)((now_ms() / 260) % 3);
		for (int d = 0; d < 3; d++) pd->graphics->fillEllipse(24 + d * 14, y + 11 - (d == ph ? 3 : 0), 8, 8, 0, 0, kColorBlack);
	}
	pd->graphics->clearClipRect();
	if (viewW < 400) pd->graphics->setClipRect(0, 0, viewW, 240);
	if (typing) {                                                        /* input box rides above the keyboard-narrowed area */
		pd->graphics->drawRoundRect(8, 166, viewW - 16, 48, 10, 3, kColorBlack);
		if (textLen == 0) ui_wrap("Type with the keyboard...", 20, 190, viewW - 44, 2, fBody, 18, kDrawModeFillBlack);
		else draw_typed(20, 182, viewW - 44, 2, 19);
	} else ui_hints("A Write|UD Scroll|B Back");
}

/* ---------- splash animation and page transitions ---------- */

#ifndef TRANS_MS
#define TRANS_MS 230
#endif
static LCDBitmap* oldBmp;              /* the screen we are leaving, slid out while the new one slides in */
static struct { bool active; int dir; uint32_t t0; } trans;
static uint32_t splashT0, fadeInT0;

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

static LCDBitmap* markBmp[4];           /* dot + three arcs of the logo mark */
static LCDBitmap* letterBmp[WORD_N];   /* the wordmark, one bitmap per letter */

static void load_splash_art(void)
{
	const char* err;
	char path[40];
	for (int i = 0; i < 4; i++) { snprintf(path, sizeof path, "images/mark%d", i); markBmp[i] = pd->graphics->loadBitmap(path, &err); }
	for (int i = 0; i < WORD_N; i++) { snprintf(path, sizeof path, "images/letter%d", i); letterBmp[i] = pd->graphics->loadBitmap(path, &err); }
}

static float ease_back(float t)        /* overshoot, then settle */
{
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	const float c1 = 1.70158f, c3 = c1 + 1;
	return 1 + c3 * (t - 1) * (t - 1) * (t - 1) + c1 * (t - 1) * (t - 1);
}

/* Start-up animation (about 3 s): a dot pings, the signal arcs pop out one by one, the wordmark rises letter by letter, then it
   dissolves into the home screen. Uses the same artwork as the launcher card (tools/make_art.py). */
static void draw_splash(uint32_t t)
{
	const struct playdate_graphics* G = pd->graphics;
	G->clear(kColorBlack);
	G->setDrawMode(kDrawModeCopy);
	const int total = MARK_SIZE + 24 + WORD_W, mx = (400 - total) / 2, my = 120 - MARK_SIZE / 2;
	const int wx = mx + MARK_SIZE + 24, wy = 120 - WORD_H / 2;
	const int ox = mx + 12, oy = my + 36;                                   /* the dot: where the arcs radiate from */
	/* ping rings leaving the dot */
	for (int k = 0; k < 2; k++) {
		int tk = (int)t - 120 - k * 240;
		if (tk <= 0 || tk > 620) continue;
		int r = 6 + (int)(ease_out(tk / 620.0f) * 120);
		G->drawEllipse(ox - r, oy - r, r * 2, r * 2, 2, 0, 0, kColorWhite);
	}
	/* the mark: dot first, then each arc pops from the dot (the art is white on black; black is drawn transparent so the parts stack) */
	G->setDrawMode(kDrawModeBlackTransparent);
	for (int i = 0; i < 4; i++) {
		float p = ease_back(((int)t - (i == 0 ? 0 : 250 + i * 130)) / (i == 0 ? 320.0f : 300.0f));
		if (((int)t - (i == 0 ? 0 : 250 + i * 130)) <= 0 || !markBmp[i]) continue;
		float s = i == 0 ? p : 0.55f + 0.45f * p;
		G->drawScaledBitmap(markBmp[i], ox - (int)(12 * s), oy - (int)(36 * s), s, s);
	}
	/* wordmark: letters rise out of a slot, staggered */
	G->setDrawMode(kDrawModeBlackTransparent);
	G->setClipRect(wx - 4, wy - 4, WORD_W + 8, WORD_H + 8);
	for (int i = 0; i < WORD_N; i++) {
		int ti = (int)t - 900 - i * 85;
		if (ti <= 0 || !letterBmp[i]) continue;
		float p = ease_back(ti / 420.0f);
		int dy = (int)((1 - p) * 40);
		G->drawBitmap(letterBmp[i], wx + WORD_LETTERS[i].x, wy + WORD_LETTERS[i].y + dy, kBitmapUnflipped);
	}
	G->clearClipRect();
	/* underline wipe + tagline */
	float u = ease_out(((int)t - 1700) / 450.0f);
	if (u > 0) G->fillRect(wx + 2, wy + WORD_H + 8, (int)((WORD_W - 4) * u), 2, kColorWhite);
	if (t > 1750) {
		int gl = 16 - (int)(16 * ((int)t - 1750) / 500.0f);
		Tm(fBold, "radio   tv   podcasts   chat", wx + 2, wy + WORD_H + 28, kDrawModeFillWhite);
		ui_fade(wx - 2, wy + WORD_H + 14, WORD_W + 6, 30, true, gl);
	}
	if (t > 2600) ui_fade(0, 0, 400, 240, false, (int)(16 * ((int)t - 2600) / 380.0f));   /* dissolve to white; the home screen fades in */
}

/* ---------- main loop ---------- */

static void on_access(bool allowed, void* ud) { (void)ud; netAllowed = allowed; if (!allowed) show_msg("Network access was denied. You can allow it again in the Playdate's Settings.", NULL); }

static void list_input(List* l, PDButtons pushed)
{
	float c = pd->system->getCrankChange(); crankAcc += c;
	while (crankAcc > 25) { if (l->sel < l->n - 1) l->sel++; crankAcc -= 25; }
	while (crankAcc < -25) { if (l->sel > 0) l->sel--; crankAcc += 25; }
	if ((pushed & kButtonDown) && l->sel < l->n - 1) l->sel++;
	if ((pushed & kButtonUp) && l->sel > 0) l->sel--;
}

static void load_error(Req* r)
{
	static char m[240];
	const char* why = r->error[0] ? r->error : "request failed";
	if (pendingParser == parse_health) {
		snprintf(lastTest, sizeof lastTest, "Can't reach it");
		refresh_setup();
		if (r->status == 403 || !strcmp(why, "HTTP 403")) snprintf(m, sizeof m, "The relay answered, but the token doesn't match. Set the same token in Setup.");
		else snprintf(m, sizeof m, "Nothing answered at %.80s (%s). Check the address, that the relay is running, and that the Playdate is on the same Wi-Fi.", relayHost, why);
		show_card(IC_LOCK, "Can't reach the relay", m, 0);
		return;
	}
	snprintf(m, sizeof m, "%s%s", why, r->status == 502 ? " - the relay couldn't open that source" : "");
	show_card(IC_WIFI, "Couldn't load that", m, 0);
}

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
		if ((pushed & (kButtonA | kButtonB)) && t > 700 && t < 2600) { splashT0 = now - 2600; t = 2600; }     /* skip */
		draw_splash(t);
		if (t >= 2980) { mode = netAllowed ? M_HOME : M_LOADING; fadeInT0 = now; }
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
		else if (a == kAccessDeny) show_msg("Network access was denied. You can allow it again in the Playdate's Settings.", NULL);
	}
	if (netAllowed && mode == M_LOADING && !pending && !depth) mode = M_HOME;

	net_update();
	maybe_fall_back();
	chat_poll();

	/* finished list fetch */
	if (pending && pending->finished) {
		Req* r = pending; pending = NULL;
		Parser p = pendingParser;
		if (r->status < 0 || r->status >= 400) load_error(r);
		else if (r->len == 0) show_msg("The server sent nothing.", NULL);
		else p(r);
		net_free(r);
	}

	play_queue_tick(now_ms());

	/* the next system: when an on-demand item ends, play the next one in its list; when a live stream drops (server time caps,
	   Wi-Fi hiccups) reconnect it a few times */
	if (stream && stream->status < 0 && !strcmp(stream->error, "stream ended") && (mode == M_PLAY || mode == M_VIDEO) && vqCount == 0 && !playReq.pending && (started || vGotFrame) && now_ms() - startedAt > 4000) {
		if (!playOnDemand && resumeCount < 4) { resumeCount++; char t[80], a[1100]; snprintf(t, sizeof t, "%s", playTitle); snprintf(a, sizeof a, "%s", playArg); request_play(t, a, mode == M_VIDEO); }
		else if (playOnDemand && next_index(1) >= 0) surf(1);
	}

	/* streaming playback control */
	if (mode == M_VIDEO && stream) { uint32_t now = now_ms(); video_demux(now); video_show(now); }
	if (stream && player && !started && stream->status >= 0 && ring_fill(videoActive ? &audioRing : &stream->ring) >= (videoActive ? VIDEO_AUDIO_START : PREBUFFER)) {
		started = true; startedAt = now_ms();
		pd->sound->fileplayer->play(player, 1);
	}

	switch (mode) {
		case M_SPLASH: break;
		case M_HOME: {
			int n = ui_home_count();
			if ((pushed & kButtonRight) && homeSel < n - 1) homeSel++;
			if ((pushed & kButtonLeft) && homeSel > 0) homeSel--;
			float c = pd->system->getCrankChange(); crankAcc += c;
			while (crankAcc > 32) { if (homeSel < n - 1) homeSel++; crankAcc -= 32; }
			while (crankAcc < -32) { if (homeSel > 0) homeSel--; crankAcc += 32; }
			homePos += (homeSel - homePos) * 0.32f;
			if (homePos - homeSel < 0.01f && homePos - homeSel > -0.01f) homePos = (float)homeSel;
			ui_home(homeSel, homePos);
			if (pushed & kButtonA) home_open(homeSel);
			break;
		}
		case M_LOADING:
			if (!netAllowed) { ui_header("Airwave", ""); ui_spinner(200, 100, 26); Tmc(fBold, "Waiting for network permission...", 200, 148, kDrawModeFillBlack); }
			else {
				ui_header(pendingTitle[0] ? pendingTitle : "Airwave", "");
				ui_spinner(200, 98, 26);
				Tmc(fBold, "Loading", 200, 144, kDrawModeFillBlack);
				ui_hints("B Cancel");
				if (pushed & kButtonB) { if (pending) { net_free(pending); pending = NULL; } mode = depth ? M_LIST : M_HOME; }
			}
			break;
		case M_MSG:
			ui_header("Airwave", "");
			ui_card(msgIcon, msgTitle, msg);
			ui_hints(msgAction ? "A Set up the relay|B Back" : "A OK|B Back");
			if (pushed & kButtonA) { Mode back = depth ? M_LIST : M_HOME; int act = msgAction; mode = back; msgAction = 0; if (act == 1) open_setup(); }
			else if (pushed & kButtonB) { mode = depth ? M_LIST : M_HOME; msgAction = 0; }
			break;
		case M_TEXT:
			draw_text_screen();
			if (!kbActive && !kbWant) mode = depth ? M_LIST : M_HOME;     /* keyboard was cancelled */
			break;
		case M_CHAT: {
			draw_chat();
			float c = pd->system->getCrankChange();
			chatScroll += (int)(c * 1.6f);
			if (pushed & kButtonDown) chatScroll += 36;
			if (pushed & kButtonUp) chatScroll -= 36;
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
				pd->graphics->setDrawMode(kDrawModeFillWhite);
				if (!dead) { ui_spinner(200, 104, 22); Tmc(fBold, playReq.pending ? "Tuning in..." : "Loading video...", 200, 148, kDrawModeFillWhite); Tmc(fSmall, nowTitle, 200, 170, kDrawModeFillWhite); }
				else {
					Tmc(fBig, "Can't play this one", 200, 90, kDrawModeFillWhite);
					Tmc(fBody, !strcmp(stream->error, "HTTP 502") ? "The relay couldn't open it (YouTube needs yt-dlp)" : stream->error[0] ? stream->error : "stream ended", 200, 124, kDrawModeFillWhite);
					Tmc(fSmall, nowTitle, 200, 148, kDrawModeFillWhite);
				}
			}
			if (pushed & (kButtonDown | kButtonRight)) surf(1);
			if (pushed & (kButtonUp | kButtonLeft)) surf(-1);
			if (pushed & kButtonB) { stop_playback(); playReq.pending = false; mode = depth ? M_LIST : M_HOME; }
			break;
		}
		case M_PLAY:
			draw_play();
			if (pushed & (kButtonDown | kButtonRight)) surf(1);
			if (pushed & (kButtonUp | kButtonLeft)) surf(-1);
			if (pushed & kButtonB) { stop_playback(); playReq.pending = false; mode = depth ? M_LIST : M_HOME; }
			break;
		case M_LIST: {
			if (!depth) { mode = M_HOME; break; }
			List* l = stack[depth - 1];
			list_input(l, pushed);
			char cnt[16] = ""; if (l->n > 5) snprintf(cnt, sizeof cnt, "%d / %d", l->sel + 1, l->n);
			ui_header(l->title, cnt);
			ui_list(l);
			bool v = l->n && l->items[l->sel].kind == K_PLAY_VIDEO && !item_locked(&l->items[l->sel]);
			ui_hints(v ? "A Watch|LR Audio only|B Back" : "A Open|B Back|C Scroll");
			if ((pushed & kButtonA) && l->n) activate(&l->items[l->sel], false);
			if ((pushed & kButtonRight) && v) activate(&l->items[l->sel], true);
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
		if (lv <= 0) fadeInT0 = 0; else ui_fade(0, 0, 400, 240, false, lv);
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
		ui_load_fonts();
		load_splash_art();
		oldBmp = pd->graphics->newBitmap(400, 240, kColorWhite);
		load_settings();
		if (debugOn) { SDFile* lf = pd->file->open("airwave.log", kFileWrite); if (lf) pd->file->close(lf); }
		dbg("init: fonts %p %p %p %p", (void*)fBody, (void*)fBold, (void*)fBig, (void*)fSmall);
		dbg("font heights: body %d bold %d small %d big %d title %d", fBody ? pd->graphics->getFontHeight(fBody) : -1, fBold ? pd->graphics->getFontHeight(fBold) : -1, fSmall ? pd->graphics->getFontHeight(fSmall) : -1, fBig ? pd->graphics->getFontHeight(fBig) : -1, fTitle ? pd->graphics->getFontHeight(fTitle) : -1);
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

