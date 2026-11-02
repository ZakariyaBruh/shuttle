#ifndef UI_H
#define UI_H

#include "common.h"

/* Shared UI types (the app logic in main.c fills them, ui.c draws them). */
typedef enum {
	K_NONE, K_MENU, K_RADIO_Q, K_SEARCH, K_SEARCH_ALL, K_SEARCH_IN, K_POD_CHARTS, K_POD_ID, K_POD_FEED,
	K_RELAY_LIST, K_PLAY, K_PLAY_VIDEO,
	K_TUNEIN_LIST, K_TUNEIN_PLAY, K_IA_Q, K_IA_ITEM,
	K_RELAY_EDIT, K_TOKEN_EDIT, K_RELAY_TEST, K_INFO
} Kind;

typedef enum {
	IC_SEARCH, IC_RADIO, IC_POD, IC_LIB, IC_TUNEIN, IC_TV, IC_CHAT, IC_SETUP,
	IC_PLAY, IC_FOLDER, IC_VIDEO, IC_LOCK, IC_LIST, IC_INFO, IC_WIFI, IC_CHECK
} IconId;

typedef struct { char label[80]; char sub[56]; char arg[1100]; Kind kind; } Item;
typedef struct { char title[40]; Item* items; int n, sel, top; float selY, scrollY; bool primed; } List;

extern LCDFont *fBody, *fBold, *fBig, *fTitle, *fSmall;
extern int viewW;                                   /* width left of the keyboard while it slides in */

uint32_t now_ms(void);
void ui_load_fonts(void);
void T(LCDFont* f, const char* s, int x, int y, LCDBitmapDrawMode m);
int TW(LCDFont* f, const char* s);
void Tc(LCDFont* f, const char* s, int cx, int y, LCDBitmapDrawMode m);

/* hooks implemented by the app */
bool item_locked(const Item* it);                   /* needs the relay and none is set */
IconId item_icon(const Item* it);
bool relay_configured(void);

void ui_fade(int x, int y, int w, int h, bool black, int level);   /* ordered-dither cover, level 0..16 */
void ui_icon(IconId ic, int cx, int cy, int size, LCDColor c);
void ui_header(const char* title, const char* counter);
void ui_hints(const char* spec);                   /* "A Open|B Back|UD Switch|LR Browse|C Scroll" */
int ui_wrap(const char* text, int x, int y, int w, int maxLines, LCDFont* f, int lineH, LCDBitmapDrawMode m);
void ui_list(List* l);
void ui_home(int sel, float pos);
int ui_home_count(void);
void ui_spinner(int cx, int cy, int r);
void ui_card(IconId ic, const char* title, const char* body);
void ui_art(int x, int y, int s, const char* seed, bool active);
void ui_eq(int x, int y, int w, int h, bool active);
int ui_chip(int x, int y, const char* text, bool filled);
void ui_bubble(int x, int y, int w, int h, bool user);

#endif
