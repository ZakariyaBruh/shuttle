/* Airwave UI toolkit: fonts + text alignment, icons, header/hints, animated lists, the home carousel, cards, spinner, cover art. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"

LCDFont *fBody, *fBold, *fBig, *fTitle, *fSmall;
int viewW = 400;

#define ROW_H 38
#define LIST_TOP 36
#define LIST_H 178

uint32_t now_ms(void) { return pd->system->getCurrentTimeMilliseconds(); }

/* ---------- fonts and measured text metrics ---------- */

typedef struct { LCDFont* f; int top, cap; } FMet;           /* top = distance from the line box top to the top of a capital */
static FMet fmet[8];
static int nmet;

static void measure(LCDFont* f)
{
	if (!f || nmet >= 8) return;
	FMet m = { f, 5, 11 };                                   /* sane defaults if measuring fails */
	LCDBitmap* b = pd->graphics->newBitmap(48, 72, kColorWhite);
	if (b) {
		pd->graphics->pushContext(b);
		pd->graphics->setFont(f);
		pd->graphics->setDrawMode(kDrawModeFillBlack);
		pd->graphics->drawText("H", 1, kUTF8Encoding, 6, 6);
		pd->graphics->popContext();
		uint8_t *data, *mask; int w, h, rb;
		pd->graphics->getBitmapData(b, &w, &h, &rb, &mask, &data);
		int first = -1, last = -1;
		if (data) for (int y = 0; y < h; y++) {
			bool ink = false;
			for (int x = 0; x < w && !ink; x++) if (!((data[y * rb + (x >> 3)] >> (7 - (x & 7))) & 1)) ink = true;
			if (ink) { if (first < 0) first = y; last = y; }
		}
		if (first >= 0) { m.top = first - 6; m.cap = last - first + 1; }
		pd->graphics->freeBitmap(b);
	}
	fmet[nmet++] = m;
}

static const FMet* met(LCDFont* f)
{
	for (int i = 0; i < nmet; i++) if (fmet[i].f == f) return &fmet[i];
	static const FMet dflt = { NULL, 5, 11 };
	return &dflt;
}

int font_cap(LCDFont* f) { return met(f)->cap; }

void ui_load_fonts(void)
{
	const char* err;
	fBody = pd->graphics->loadFont("/System/Fonts/Roobert-11-Medium.pft", &err);
	fBold = pd->graphics->loadFont("/System/Fonts/Roobert-11-Bold.pft", &err);
	fSmall = pd->graphics->loadFont("/System/Fonts/Roobert-10-Bold.pft", &err);
	fBig = pd->graphics->loadFont("/System/Fonts/Roobert-20-Medium.pft", &err);
	fTitle = pd->graphics->loadFont("/System/Fonts/Roobert-24-Medium.pft", &err);
	if (!fBody) fBody = pd->graphics->loadFont("/System/Fonts/Asheville-Sans-14-Light.pft", &err);
	if (!fBold) fBold = fBody;
	if (!fSmall) fSmall = fBody;
	if (!fBig) fBig = fBold;
	if (!fTitle) fTitle = fBig;
	measure(fBody); measure(fBold); measure(fSmall); measure(fBig); measure(fTitle);
}

void T(LCDFont* f, const char* s, int x, int y, LCDBitmapDrawMode m)
{
	if (f) pd->graphics->setFont(f);
	pd->graphics->setDrawMode(m);
	pd->graphics->drawText(s, strlen(s), kUTF8Encoding, x, y);
}
int TW(LCDFont* f, const char* s) { return f ? pd->graphics->getTextWidth(f, s, strlen(s), kUTF8Encoding, 0) : (int)strlen(s) * 7; }

void Tm(LCDFont* f, const char* s, int x, int cy, LCDBitmapDrawMode m)
{
	const FMet* k = met(f);
	T(f, s, x, cy - k->top - (k->cap + 1) / 2, m);
}
void Tmc(LCDFont* f, const char* s, int cx, int cy, LCDBitmapDrawMode m) { Tm(f, s, cx - TW(f, s) / 2, cy, m); }
void Tmr(LCDFont* f, const char* s, int xr, int cy, LCDBitmapDrawMode m) { Tm(f, s, xr - TW(f, s), cy, m); }

static const int COS12[12] = { 1000, 866, 500, 0, -500, -866, -1000, -866, -500, 0, 500, 866 };
static const int SIN12[12] = { 0, 500, 866, 1000, 866, 500, 0, -500, -866, -1000, -866, -500 };

/* ordered-dither cover: `level` 0..16 of coverage with a 4x4 Bayer mask, so it dissolves instead of fading flat */
void ui_fade(int x, int y, int w, int h, bool black, int level)
{
	static const uint8_t bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
	if (level <= 0) return;
	static uint8_t pat[16];                          /* 8 rows of colour, then 8 rows of mask */
	for (int r = 0; r < 8; r++) {
		uint8_t mask = 0;
		for (int c = 0; c < 8; c++) if (bayer[r & 3][c & 3] < level) mask |= (uint8_t)(0x80 >> c);
		pat[r] = black ? 0x00 : 0xFF; pat[8 + r] = mask;
	}
	pd->graphics->fillRect(x, y, w, h, (LCDColor)(uintptr_t)pat);
}

/* ---------- icons: designed on a 44x44 grid, scaled to `size`, centred on (cx, cy) ---------- */

void ui_icon(IconId ic, int cx, int cy, int size, LCDColor c)
{
	const struct playdate_graphics* G = pd->graphics;
	float u = size / 44.0f;
	int x0 = cx - (int)(22 * u), y0 = cy - (int)(22 * u);
#define X(v) (x0 + (int)((v) * u + 0.5f))
#define Y(v) (y0 + (int)((v) * u + 0.5f))
#define L(v) ((int)((v) * u + 0.5f) < 1 ? 1 : (int)((v) * u + 0.5f))
	switch (ic) {
		case IC_SEARCH:
			G->drawEllipse(X(4), Y(3), L(26), L(26), L(4), 0, 0, c);
			G->drawLine(X(26), Y(26), X(39), Y(39), L(5), c);
			break;
		case IC_RADIO:
			G->drawRoundRect(X(2), Y(13), L(40), L(28), L(5), L(3), c);
			G->drawLine(X(10), Y(13), X(32), Y(3), L(2), c);
			G->drawEllipse(X(8), Y(19), L(16), L(16), L(3), 0, 0, c);
			for (int i = 0; i < 3; i++) G->fillRect(X(29), Y(20 + i * 6), L(9), L(3), c);
			break;
		case IC_POD:
			G->fillRoundRect(X(15), Y(2), L(14), L(23), L(7), c);
			G->drawEllipse(X(8), Y(11), L(28), L(24), L(3), 90, 270, c);
			G->drawLine(X(22), Y(34), X(22), Y(40), L(3), c);
			G->drawLine(X(13), Y(41), X(31), Y(41), L(3), c);
			break;
		case IC_LIB:                                    /* open book */
			G->drawRoundRect(X(3), Y(7), L(38), L(30), L(4), L(3), c);
			G->drawLine(X(22), Y(8), X(22), Y(36), L(3), c);
			for (int i = 0; i < 3; i++) { G->drawLine(X(8), Y(15 + i * 7), X(18), Y(15 + i * 7), L(2), c); G->drawLine(X(26), Y(15 + i * 7), X(36), Y(15 + i * 7), L(2), c); }
			break;
		case IC_TUNEIN:                                 /* globe */
			G->drawEllipse(X(3), Y(3), L(38), L(38), L(3), 0, 0, c);
			G->drawEllipse(X(14), Y(3), L(16), L(38), L(3), 0, 0, c);
			G->drawLine(X(4), Y(22), X(40), Y(22), L(3), c);
			G->drawLine(X(8), Y(12), X(36), Y(12), L(2), c);
			G->drawLine(X(8), Y(32), X(36), Y(32), L(2), c);
			break;
		case IC_TV:
			G->drawRoundRect(X(2), Y(11), L(40), L(28), L(5), L(3), c);
			G->fillRoundRect(X(8), Y(17), L(28), L(16), L(3), c);
			G->drawLine(X(15), Y(11), X(9), Y(2), L(2), c);
			G->drawLine(X(29), Y(11), X(35), Y(2), L(2), c);
			G->drawLine(X(12), Y(42), X(32), Y(42), L(3), c);
			break;
		case IC_YT:                                     /* rounded screen with a play triangle */
			G->fillRoundRect(X(1), Y(8), L(42), L(28), L(9), c);
			G->fillTriangle(X(17), Y(15), X(17), Y(29), X(30), Y(22), c == kColorBlack ? kColorWhite : kColorBlack);
			break;
		case IC_CHAT:
			G->drawRoundRect(X(2), Y(4), L(40), L(28), L(8), L(3), c);
			G->fillTriangle(X(9), Y(31), X(9), Y(42), X(21), Y(31), c);
			for (int i = 0; i < 3; i++) G->fillEllipse(X(10 + i * 10), Y(14), L(6), L(6), 0, 0, c);
			break;
		case IC_SETUP:                                  /* gear */
			G->drawEllipse(X(9), Y(9), L(26), L(26), L(5), 0, 0, c);
			for (int i = 0; i < 12; i++) G->fillRect(X(20 + (COS12[i] * 17) / 1000.0f), Y(20 + (SIN12[i] * 17) / 1000.0f), L(4), L(4), c);
			break;
		case IC_PLAY:
			G->fillTriangle(X(11), Y(6), X(11), Y(38), X(38), Y(22), c);
			break;
		case IC_FOLDER:                                 /* chevron */
			G->drawLine(X(14), Y(6), X(31), Y(22), L(5), c);
			G->drawLine(X(31), Y(22), X(14), Y(38), L(5), c);
			break;
		case IC_VIDEO:
			G->drawRoundRect(X(3), Y(8), L(38), L(28), L(5), L(3), c);
			G->fillTriangle(X(17), Y(15), X(17), Y(29), X(30), Y(22), c);
			break;
		case IC_LOCK:
			G->fillRoundRect(X(8), Y(20), L(28), L(21), L(4), c);
			G->drawEllipse(X(12), Y(3), L(20), L(26), L(4), 0, 0, c);
			break;
		case IC_LIST:
			for (int i = 0; i < 3; i++) G->fillRoundRect(X(6), Y(8 + i * 11), L(32), L(5), L(2), c);
			break;
		case IC_INFO:
			G->drawEllipse(X(3), Y(3), L(38), L(38), L(4), 0, 0, c);
			G->fillRect(X(20), Y(19), L(4), L(13), c);
			G->fillRect(X(20), Y(11), L(4), L(4), c);
			break;
		case IC_WIFI:
			for (int i = 1; i <= 3; i++) { int r = i * 8; G->drawEllipse(X(22 - r), Y(30 - r), L(r * 2), L(r * 2), L(3), 300, 420, c); }
			G->fillEllipse(X(19), Y(31), L(7), L(7), 0, 0, c);
			break;
		case IC_CHECK:
			G->drawLine(X(8), Y(24), X(18), Y(34), L(5), c);
			G->drawLine(X(18), Y(34), X(37), Y(10), L(5), c);
			break;
	}
#undef X
#undef Y
#undef L
}

/* ---------- status icons (battery, Wi-Fi, relay) ---------- */

static int ui_status(int xr, int cy)                           /* right-aligned at xr; returns its width */
{
	const struct playdate_graphics* G = pd->graphics;
	int x = xr;
	float pct = pd->system->getBatteryPercentage();
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	x -= 24;
	G->drawRoundRect(x, cy - 6, 21, 12, 3, 2, kColorBlack);
	G->fillRect(x + 21, cy - 2, 2, 4, kColorBlack);
	int fw = (int)(15 * pct / 100.0f);
	if (fw > 0) G->fillRect(x + 3, cy - 3, fw, 6, kColorBlack);
	x -= 8 + 16;
	bool wifi = pd->network->getStatus() == kWifiConnected;
	ui_icon(IC_WIFI, x + 8, cy, 16, kColorBlack);
	if (!wifi) G->drawLine(x, cy + 7, x + 16, cy - 7, 2, kColorBlack);
	if (relay_configured()) {
		x -= 8 + 22;
		G->fillRoundRect(x, cy - 7, 22, 14, 4, kColorBlack);
		Tmc(fSmall, "R", x + 11, cy, kDrawModeFillWhite);
	}
	return xr - x;
}

void ui_header(const char* title, const char* counter)
{
	const struct playdate_graphics* G = pd->graphics;
	int stW = 0;
	if (viewW >= 300) stW = ui_status(viewW - 12, 17) + 8;
	int cw = (counter && counter[0]) ? TW(fSmall, counter) + 12 : 0;
	int avail = viewW - 22 - stW - cw;
	static char t[80]; snprintf(t, sizeof t, "%s", title);
	LCDFont* hf = viewW < 300 ? fBold : fBig;                    /* a smaller title while the keyboard has the screen */
	if (TW(hf, t) > avail) {
		while (strlen(t) > 4 && TW(hf, t) > avail - 16) t[strlen(t) - 1] = 0;
		strcat(t, "..");
	}
	Tm(hf, t, 10, 16, kDrawModeFillBlack);
	if (cw) Tmr(fSmall, counter, viewW - 12 - stW, 17, kDrawModeFillBlack);
	G->fillRect(8, 33, viewW - 16, 2, kColorBlack);
}

/* ---------- button hints ---------- */

static void glyph(const char* g, int x, int cy)                  /* 16x16, centred vertically on cy */
{
	const struct playdate_graphics* G = pd->graphics;
	int y = cy - 8;
	if (!strcmp(g, "A") || !strcmp(g, "B")) {
		G->fillEllipse(x, y, 16, 16, 0, 0, kColorBlack);
		Tmc(fSmall, g, x + 8, cy, kDrawModeFillWhite);
	} else if (!strcmp(g, "UD")) {
		G->fillTriangle(x + 8, y, x + 2, y + 6, x + 14, y + 6, kColorBlack);
		G->fillTriangle(x + 8, y + 15, x + 2, y + 9, x + 14, y + 9, kColorBlack);
	} else if (!strcmp(g, "LR")) {
		G->fillTriangle(x, y + 8, x + 6, y + 2, x + 6, y + 14, kColorBlack);
		G->fillTriangle(x + 15, y + 8, x + 9, y + 2, x + 9, y + 14, kColorBlack);
	} else if (!strcmp(g, "C")) {                                   /* crank */
		G->drawEllipse(x + 1, y + 4, 11, 11, 2, 0, 0, kColorBlack);
		G->drawLine(x + 11, y + 5, x + 15, y + 1, 2, kColorBlack);
	}
}

void ui_hints(const char* spec)
{
	const struct playdate_graphics* G = pd->graphics;
	G->fillRect(0, 218, viewW, 22, kColorWhite);
	ui_fade(8, 218, viewW - 16, 1, true, 8);
	int x = 10, cy = 229;
	const char* p = spec;
	while (*p) {
		char tok[40]; int n = 0;
		while (p[n] && p[n] != '|' && n < 39) { tok[n] = p[n]; n++; }
		tok[n] = 0; p += n; if (*p == '|') p++;
		char* sp = strchr(tok, ' ');
		if (!sp) continue;
		*sp = 0;
		const char* label = sp + 1;
		int w = 16 + 5 + TW(fSmall, label);
		if (x + w > viewW - 6) break;
		glyph(tok, x, cy);
		Tm(fSmall, label, x + 21, cy, kDrawModeFillBlack);
		x += w + 16;
	}
}

/* word-wrap helper; y = vertical centre of the first line; returns the number of lines drawn */
int ui_wrap(const char* text, int x, int y, int w, int maxLines, LCDFont* f, int lineH, LCDBitmapDrawMode m)
{
	static char line[120];
	int lines = 0;
	const char* s = text;
	while (*s && lines < maxLines) {
		int n = 0, lastSpace = -1;
		while (s[n] && s[n] != '\n' && n < 110) {
			snprintf(line, sizeof line, "%.*s", n + 1, s);
			if (TW(f, line) > w) break;
			if (s[n] == ' ') lastSpace = n;
			n++;
		}
		if (s[n] && s[n] != '\n' && lastSpace > 0 && n < 110) n = lastSpace;   /* break at the last space */
		if (n == 0) n = 1;
		snprintf(line, sizeof line, "%.*s", n, s);
		Tm(f, line, x, y + lines * lineH, m);
		lines++;
		s += n;
		if (*s == '\n' || *s == ' ') s++;
		while (*s == ' ') s++;
	}
	return lines;
}

/* ---------- animated lists ---------- */

static void row_glyph(const Item* it, int cx, int cy, LCDColor c)
{
	if (item_locked(it)) { ui_icon(IC_LOCK, cx, cy, 14, c); return; }
	switch (it->kind) {
		case K_MENU: case K_RADIO_Q: case K_RELAY_LIST: case K_TUNEIN_LIST: case K_IA_Q: case K_POD_CHARTS: case K_POD_GENRES: case K_POD_GENRE:
		case K_POD_ID: case K_POD_FEED: case K_IA_ITEM:
			ui_icon(IC_FOLDER, cx, cy, 14, c); break;
		case K_PLAY: case K_PLAY_VIDEO: case K_TUNEIN_PLAY: ui_icon(IC_PLAY, cx, cy, 14, c); break;
		default: break;
	}
}

void ui_list(List* l)
{
	const struct playdate_graphics* G = pd->graphics;
	if (l->n == 0) { Tmc(fBold, "Nothing here yet", viewW / 2, 110, kDrawModeFillBlack); return; }
	int maxScroll = l->n * ROW_H > LIST_H ? l->n * ROW_H - LIST_H : 0;
	float target = l->sel * ROW_H - 1.5f * ROW_H;
	if (target < 0) target = 0;
	if (target > maxScroll) target = (float)maxScroll;
	if (!l->primed) { l->primed = true; l->selY = (float)(l->sel * ROW_H); l->scrollY = target; }
	l->selY += (l->sel * ROW_H - l->selY) * 0.45f;
	l->scrollY += (target - l->scrollY) * 0.35f;
	if (l->selY - (l->sel * ROW_H) < 0.6f && l->selY - (l->sel * ROW_H) > -0.6f) l->selY = (float)(l->sel * ROW_H);
	int barW = viewW - 12 - (l->n * ROW_H > LIST_H ? 8 : 0);

	G->setClipRect(0, LIST_TOP - 1, viewW, LIST_H + 2);
	int by = LIST_TOP + (int)l->selY - (int)l->scrollY;
	G->fillRoundRect(6, by + 1, barW, ROW_H - 3, 10, kColorBlack);
	int first = (int)(l->scrollY / ROW_H) - 1; if (first < 0) first = 0;
	for (int i = first; i < l->n && i <= first + LIST_H / ROW_H + 2; i++) {
		int y = LIST_TOP + i * ROW_H - (int)l->scrollY;
		Item* it = &l->items[i];
		float d = l->selY - i * ROW_H;
		bool onBar = d < 19 && d > -19;                                   /* text flips colour as the bar passes */
		LCDColor ink = onBar ? kColorWhite : kColorBlack;
		LCDBitmapDrawMode tm = onBar ? kDrawModeFillWhite : kDrawModeFillBlack;
		int cy = y + 18;                                                  /* the row's centre line */
		/* badge */
		if (onBar) G->fillRoundRect(14, cy - 14, 28, 28, 8, kColorWhite); else G->drawRoundRect(14, cy - 14, 28, 28, 8, 2, kColorBlack);
		ui_icon(item_icon(it), 28, cy, 18, kColorBlack);
		/* text, shortened with an ellipsis to the room left of the glyph */
		int tx = 52, room = barW - 46 - 26;
		static char lab[96]; snprintf(lab, sizeof lab, "%.80s", it->label);
		if (TW(fBold, lab) > room) {
			while (strlen(lab) > 4 && TW(fBold, lab) > room - 12) lab[strlen(lab) - 1] = 0;
			while (strlen(lab) > 1 && (lab[strlen(lab) - 1] == ' ' || lab[strlen(lab) - 1] == '|' || lab[strlen(lab) - 1] == '-')) lab[strlen(lab) - 1] = 0;
			strcat(lab, "...");
		}
		if (it->sub[0]) {
			Tm(fBold, lab, tx, cy - 7, tm);
			static char sb[64]; snprintf(sb, sizeof sb, "%s", it->sub);
			if (TW(fSmall, sb) > room) { while (strlen(sb) > 4 && TW(fSmall, sb) > room - 10) sb[strlen(sb) - 1] = 0; strcat(sb, ".."); }
			Tm(fSmall, sb, tx, cy + 8, tm);
		} else Tm(fBold, lab, tx, cy, tm);
		row_glyph(it, 6 + barW - 16, cy, ink);
	}
	G->clearClipRect();
	if (viewW < 400) G->setClipRect(0, 0, viewW, 240);
	if (maxScroll) {                                                  /* slim scrollbar */
		int th = LIST_H * LIST_H / (l->n * ROW_H); if (th < 14) th = 14;
		int ty = LIST_TOP + (int)((LIST_H - th) * l->scrollY / maxScroll);
		ui_fade(viewW - 8, LIST_TOP, 3, LIST_H, true, 6);
		G->fillRoundRect(viewW - 9, ty, 5, th, 2, kColorBlack);
	}
}

/* ---------- home carousel ---------- */

static const struct { const char* name; const char* sub; IconId ic; } HOME[] = {
	{ "Radio", "Stations from around the world", IC_RADIO },
	{ "TuneIn", "The radio directory", IC_TUNEIN },
	{ "Podcasts", "Charts, categories, search", IC_POD },
	{ "Library", "Old-time radio, audiobooks", IC_LIB },
	{ "TV", "Live channels and classic shows", IC_TV },
	{ "YouTube", "Documentaries and Shorts", IC_YT },
	{ "AI Chat", "Ask anything, no sign-in", IC_CHAT },
	{ "Search", "Find anything at once", IC_SEARCH },
	{ "Setup", "Wi-Fi and relay", IC_SETUP },
};
int ui_home_count(void) { return (int)(sizeof HOME / sizeof HOME[0]); }

static void home_card(int i, float dx, bool selected)
{
	const struct playdate_graphics* G = pd->graphics;
	float adx = dx < 0 ? -dx : dx;
	float t = adx > 1 ? 1 : adx;
	float k = 1.0f - 0.36f * t;
	int w = (int)(150 * k), h = (int)(160 * k);
	int cx = 200 + (int)(dx * 134), cy = 118;
	int x = cx - w / 2, y = cy - h / 2;
	if (x > 400 || x + w < 0) return;
	if (selected) {
		ui_fade(x + 4, y + 5, w, h, true, 7);                                  /* soft drop shadow */
		G->fillRoundRect(x, y, w, h, 14, kColorBlack);
		ui_icon(HOME[i].ic, cx, y + 46, 66, kColorWhite);
		Tmc(fTitle, HOME[i].name, cx, y + 92, kDrawModeFillWhite);
		ui_wrap(HOME[i].sub, x + 12, y + 120, w - 24, 2, fSmall, 14, kDrawModeFillWhite);
	} else {
		G->fillRoundRect(x, y, w, h, 12, kColorWhite);
		G->drawRoundRect(x, y, w, h, 12, 2, kColorBlack);
		ui_icon(HOME[i].ic, cx, cy, (int)(46 * k), kColorBlack);
	}
	if (selected && !relay_configured() && (i == 4 || i == 5)) {                  /* TV and YouTube need the relay: small padlock badge */
		G->fillEllipse(x + w - 30, y + 8, 22, 22, 0, 0, kColorWhite);
		ui_icon(IC_LOCK, x + w - 19, y + 19, 14, kColorBlack);
	}
}

void ui_home(int sel, float pos)
{
	const struct playdate_graphics* G = pd->graphics;
	ui_header("Airwave", "");
	int n = ui_home_count();
	for (int i = 0; i < n; i++) {                                              /* neighbours first, the centre card on top */
		float dx = i - pos;
		if (dx < -2.4f || dx > 2.4f || i == sel) continue;
		home_card(i, dx, false);
	}
	home_card(sel, sel - pos, true);
	for (int i = 0; i < n; i++) {                                              /* page dots */
		int x = 200 + (int)((i - (n - 1) / 2.0f) * 14);
		if (i == sel) G->fillRoundRect(x - 6, 207, 12, 6, 3, kColorBlack);
		else G->fillEllipse(x - 3, 208, 5, 5, 0, 0, kColorBlack);
	}
	ui_hints("A Open|LR Browse|C Spin");
}

/* ---------- small widgets ---------- */

void ui_spinner(int cx, int cy, int r)
{
	int head = (int)((now_ms() / 70) % 12);
	for (int i = 0; i < 12; i++) {
		int age = (head - i + 12) % 12;
		int sz = 7 - age / 2; if (sz < 2) sz = 2;
		int x = cx + (COS12[i] * r) / 1000, y = cy + (SIN12[i] * r) / 1000;
		pd->graphics->fillEllipse(x - sz / 2, y - sz / 2, sz, sz, 0, 0, kColorBlack);
	}
}

void ui_card(IconId ic, const char* title, const char* body)
{
	const struct playdate_graphics* G = pd->graphics;
	int w = viewW - 40 > 350 ? 350 : viewW - 40, x = (viewW - w) / 2, y = 44, h = 160;
	ui_fade(x + 4, y + 5, w, h, true, 7);
	G->fillRoundRect(x, y, w, h, 12, kColorWhite);
	G->drawRoundRect(x, y, w, h, 12, 2, kColorBlack);
	G->fillEllipse(x + 16, y + 16, 44, 44, 0, 0, kColorBlack);
	ui_icon(ic, x + 38, y + 38, 26, kColorWhite);
	Tm(fBig, title, x + 72, y + 38, kDrawModeFillBlack);
	ui_wrap(body, x + 18, y + 82, w - 36, 5, fBody, 19, kDrawModeFillBlack);
}

static uint32_t hash_str(const char* s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }

void ui_art(int x, int y, int s, const char* seed, bool active)
{
	const struct playdate_graphics* G = pd->graphics;
	uint32_t h = hash_str(seed);
	int style = (int)(h % 4);
	uint32_t t = now_ms();
	G->fillRoundRect(x, y, s, s, 12, kColorBlack);
	G->setClipRect(x + 2, y + 2, s - 4, s - 4);
	switch (style) {
		case 0: {                                                     /* rings */
			int ox = x + (int)((h >> 3) % 60) * s / 100 + s / 5, oy = y + s - (int)((h >> 9) % 40) * s / 100;
			int phase = active ? (int)((t / 60) % 14) : 0;
			for (int r = 10 + phase; r < s * 2; r += 14) G->drawEllipse(ox - r, oy - r, r * 2, r * 2, 3, 0, 0, kColorWhite);
			break;
		}
		case 1:                                                       /* sunburst */
			for (int i = 0; i < 12; i++) {
				int rot = active ? (int)((t / 400) % 2) : 0;
				if ((i + rot) % 2) continue;
				G->drawLine(x + s / 2, y + s / 2, x + s / 2 + (COS12[i] * s) / 700, y + s / 2 + (SIN12[i] * s) / 700, 5, kColorWhite);
			}
			G->fillEllipse(x + s / 2 - s / 7, y + s / 2 - s / 7, s / 7 * 2, s / 7 * 2, 0, 0, kColorBlack);
			G->drawEllipse(x + s / 2 - s / 7, y + s / 2 - s / 7, s / 7 * 2, s / 7 * 2, 3, 0, 0, kColorWhite);
			break;
		case 2:                                                       /* dot grid */
			for (int gy = 0; gy < 5; gy++) for (int gx = 0; gx < 5; gx++) {
				int d = 4 + (int)(((h >> (gx + gy * 5)) & 1) * 8) + (active ? (int)((t / 120 + gx + gy) % 3) * 2 : 0);
				int px = x + (gx * 2 + 1) * s / 10, py = y + (gy * 2 + 1) * s / 10;
				G->fillEllipse(px - d / 2, py - d / 2, d, d, 0, 0, kColorWhite);
			}
			break;
		default:                                                      /* bars */
			for (int b = 0; b < 9; b++) {
				int bh = 14 + (int)((h >> (b * 3)) & 7) * s / 14;
				if (active) bh = 12 + (int)(((t / 110) * 2654435761u + (uint32_t)b * 40503u) >> 25) % (s - 24);
				if (bh > s - 10) bh = s - 10;
				G->fillRoundRect(x + 8 + b * ((s - 16) / 9), y + s - 8 - bh, (s - 16) / 9 - 3, bh, 2, kColorWhite);
			}
			break;
	}
	G->clearClipRect();
	if (viewW < 400) G->setClipRect(0, 0, viewW, 240);
	G->drawRoundRect(x, y, s, s, 12, 3, kColorBlack);
}

void ui_eq(int x, int y, int w, int h, bool active)
{
	uint32_t t = now_ms();
	int n = w / 7;
	for (int i = 0; i < n; i++) {
		int bh = 3;
		if (active) { uint32_t v = ((t / 95) + (uint32_t)i * 37) * 2654435761u; bh = 4 + (int)((v >> 24) % (uint32_t)(h - 4)); }
		pd->graphics->fillRoundRect(x + i * 7, y + h - bh, 5, bh, 2, kColorBlack);
	}
}

int ui_chip(int x, int y, const char* text, bool filled)                      /* 18 high; y = top */
{
	int w = TW(fSmall, text) + 16;
	if (filled) { pd->graphics->fillRoundRect(x, y, w, 18, 9, kColorBlack); Tmc(fSmall, text, x + w / 2, y + 9, kDrawModeFillWhite); }
	else { pd->graphics->drawRoundRect(x, y, w, 18, 9, 2, kColorBlack); Tmc(fSmall, text, x + w / 2, y + 9, kDrawModeFillBlack); }
	return w;
}

void ui_bubble(int x, int y, int w, int h, bool user)
{
	if (user) pd->graphics->fillRoundRect(x, y, w, h, 10, kColorBlack);
	else { pd->graphics->fillRoundRect(x, y, w, h, 10, kColorWhite); pd->graphics->drawRoundRect(x, y, w, h, 10, 2, kColorBlack); }
}
