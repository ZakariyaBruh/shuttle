#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "common.h"

/* ---------- JSON ---------- */

static const char* skip_ws(const char* p, const char* end) { while (p < end && isspace((unsigned char)*p)) p++; return p; }

/* p points at opening quote; returns pointer after closing quote */
static const char* skip_string(const char* p, const char* end)
{
	p++;
	while (p < end && *p != '"') { if (*p == '\\') p++; p++; }
	return p < end ? p + 1 : end;
}

/* p points at '{' or '['; returns pointer after the matching close */
static const char* skip_container(const char* p, const char* end)
{
	int depth = 0;
	while (p < end) {
		char c = *p;
		if (c == '"') { p = skip_string(p, end); continue; }
		if (c == '{' || c == '[') depth++;
		else if (c == '}' || c == ']') { if (--depth == 0) return p + 1; }
		p++;
	}
	return end;
}

const char* json_array_start(const char* json, const char* arrayKey)
{
	if (!arrayKey) { const char* p = strchr(json, '['); return p; }
	char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", arrayKey);
	const char* p = strstr(json, pat);
	if (!p) return NULL;
	p = strchr(p + strlen(pat), '[');
	return p;
}

/* cursor: first call points at the array '[' ; each call yields the next object. */
const char* json_next_object(const char** cursor, const char* end, Span* obj)
{
	const char* p = *cursor;
	if (*p == '[') p++;
	while (p < end && *p != '{' && *p != ']') p++;
	if (p >= end || *p != '{') return NULL;
	const char* e = skip_container(p, end);
	obj->s = p; obj->n = (size_t)(e - p);
	*cursor = e;
	return p;
}

static void unescape_into(const char* s, const char* e, char* out, size_t outsz)
{
	size_t o = 0;
	while (s < e && o + 4 < outsz) {
		if (*s == '\\' && s + 1 < e) {
			s++;
			switch (*s) {
				case 'n': case 't': case 'r': out[o++] = ' '; s++; break;
				case 'u': {
					unsigned v = 0;
					if (s + 4 < e + 0 && sscanf(s + 1, "%4x", &v) == 1) {
						if (v < 0x80) out[o++] = (char)v;
						else if (v < 0x800) { out[o++] = 0xC0 | (v >> 6); out[o++] = 0x80 | (v & 0x3F); }
						else { out[o++] = 0xE0 | (v >> 12); out[o++] = 0x80 | ((v >> 6) & 0x3F); out[o++] = 0x80 | (v & 0x3F); }
						s += 5;
					} else { out[o++] = '?'; s++; }
					break;
				}
				default: out[o++] = *s++; break;       /* \" \\ \/ */
			}
		} else out[o++] = *s++;
	}
	out[o] = 0;
}

/* depth-1 key lookup inside one object */
static bool find_key(Span obj, const char* key, const char** vs, const char** ve, bool* isStr)
{
	const char* p = obj.s + 1;
	const char* end = obj.s + obj.n;
	size_t kl = strlen(key);
	while (p < end) {
		p = skip_ws(p, end);
		if (p >= end || *p == '}') break;
		if (*p == ',') { p++; continue; }
		if (*p != '"') return false;
		const char* ke = skip_string(p, end);
		bool match = (size_t)(ke - p - 2) == kl && strncmp(p + 1, key, kl) == 0;
		p = skip_ws(ke, end);
		if (p >= end || *p != ':') return false;
		p = skip_ws(p + 1, end);
		const char* v = p;
		const char* e;
		if (*p == '"') { e = skip_string(p, end); if (match) { *vs = v + 1; *ve = e - 1; *isStr = true; return true; } }
		else if (*p == '{' || *p == '[') { e = skip_container(p, end); if (match) { *vs = v; *ve = e; *isStr = false; return true; } }
		else { e = p; while (e < end && *e != ',' && *e != '}') e++; if (match) { *vs = v; *ve = e; *isStr = false; return true; } }
		p = e;
	}
	return false;
}

bool json_get_str(Span obj, const char* key, char* out, size_t outsz)
{
	const char *vs, *ve; bool isStr;
	out[0] = 0;
	if (!find_key(obj, key, &vs, &ve, &isStr) || !isStr) return false;
	unescape_into(vs, ve, out, outsz);
	return true;
}

long json_get_int(Span obj, const char* key, long dflt)
{
	const char *vs, *ve; bool isStr;
	if (!find_key(obj, key, &vs, &ve, &isStr)) return dflt;
	return strtol(vs, NULL, 10);
}

/* ---------- RSS ---------- */

static const char* find_ci(const char* hay, const char* end, const char* needle)
{
	size_t nl = strlen(needle);
	for (const char* p = hay; p + nl <= end; p++)
		if (strncasecmp(p, needle, nl) == 0) return p;
	return NULL;
}

bool rss_next_item(const char** cursor, Span* item)
{
	const char* p = *cursor;
	const char* end = p + strlen(p);
	const char* s = find_ci(p, end, "<item");
	if (!s) return false;
	const char* e = find_ci(s, end, "</item>");
	if (!e) e = end;                 /* truncated download: use what we have */
	item->s = s; item->n = (size_t)(e - s);
	*cursor = e < end ? e + 7 : end;
	return true;
}

bool rss_tag_text(Span item, const char* tag, char* out, size_t outsz)
{
	char open[40]; snprintf(open, sizeof open, "<%s", tag);
	const char* end = item.s + item.n;
	const char* s = find_ci(item.s, end, open);
	out[0] = 0;
	if (!s) return false;
	s = memchr(s, '>', (size_t)(end - s));
	if (!s) return false;
	s++;
	char close[40]; snprintf(close, sizeof close, "</%s>", tag);
	const char* e = find_ci(s, end, close);
	if (!e) return false;
	if (e - s >= 12 && strncmp(s, "<![CDATA[", 9) == 0) { s += 9; if (e - s >= 3 && strncmp(e - 3, "]]>", 3) == 0) e -= 3; }
	size_t n = (size_t)(e - s); if (n >= outsz) n = outsz - 1;
	memcpy(out, s, n); out[n] = 0;
	xml_unescape(out);
	return true;
}

bool rss_enclosure(Span item, char* url, size_t sz, bool* isMp3)
{
	const char* end = item.s + item.n;
	const char* s = find_ci(item.s, end, "<enclosure");
	url[0] = 0; *isMp3 = false;
	if (!s) return false;
	const char* tagEnd = memchr(s, '>', (size_t)(end - s));
	if (!tagEnd) tagEnd = end;
	const char* u = find_ci(s, tagEnd, "url=");
	if (!u) return false;
	u += 4;
	char q = *u++;
	const char* ue = memchr(u, q, (size_t)(tagEnd - u));
	if (!ue) return false;
	size_t n = (size_t)(ue - u); if (n >= sz) n = sz - 1;
	memcpy(url, u, n); url[n] = 0;
	xml_unescape(url);
	const char* t = find_ci(s, tagEnd, "audio/mpeg");
	char path[640]; snprintf(path, sizeof path, "%s", url);
	char* qm = strchr(path, '?'); if (qm) *qm = 0;
	size_t pl = strlen(path);
	*isMp3 = t != NULL || (pl > 4 && strcasecmp(path + pl - 4, ".mp3") == 0);
	return true;
}

void xml_unescape(char* s)
{
	char* o = s;
	while (*s) {
		if (*s == '&') {
			if (!strncmp(s, "&amp;", 5)) { *o++ = '&'; s += 5; continue; }
			if (!strncmp(s, "&lt;", 4)) { *o++ = '<'; s += 4; continue; }
			if (!strncmp(s, "&gt;", 4)) { *o++ = '>'; s += 4; continue; }
			if (!strncmp(s, "&quot;", 6)) { *o++ = '"'; s += 6; continue; }
			if (!strncmp(s, "&apos;", 6) || !strncmp(s, "&#39;", 5)) { *o++ = '\''; s += (s[2] == 'p') ? 6 : 5; continue; }
		}
		*o++ = *s++;
	}
	*o = 0;
}

void copy_ascii(char* dst, size_t dstsz, const char* src)
{
	/* The system font is Latin-only: keep ASCII, drop accents, map typographic punctuation, replace the rest (CJK, emoji...) with '?'. */
	size_t o = 0;
	while (*src && o + 2 < dstsz) {
		unsigned char c = (unsigned char)*src;
		if (c < 32) { dst[o++] = ' '; src++; }
		else if (c < 0x80) { dst[o++] = (char)c; src++; }
		else if (c == 0xE2 && (unsigned char)src[1] == 0x80 && src[2]) {      /* common typographic punctuation */
			unsigned char d = (unsigned char)src[2];
			if (d == 0x98 || d == 0x99) dst[o++] = '\'';
			else if (d == 0x9C || d == 0x9D) dst[o++] = '"';
			else if (d == 0x93 || d == 0x94) dst[o++] = '-';
			else if (d == 0xA6) { dst[o++] = '.'; dst[o++] = '.'; if (o + 2 < dstsz) dst[o++] = '.'; }
			else dst[o++] = '?';
			src += 3;
		}
		else if (c == 0xC3 && ((unsigned char)src[1] & 0xC0) == 0x80) {        /* Latin-1 letters: drop the accent */
			static const char* plain = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
			dst[o++] = plain[(unsigned char)src[1] - 0x80];
			src += 2;
		}
		else {
			dst[o++] = '?';
			src++;
			while (((unsigned char)*src & 0xC0) == 0x80) src++;
		}
	}
	dst[o] = 0;
}
