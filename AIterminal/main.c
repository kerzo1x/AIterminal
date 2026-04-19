#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#endif

// ========================CONSTANTS====================================

#define SCREEN_WIDTH    1110
#define SCREEN_HEIGHT   620
#define FONT_SIZE       18
#define MAX_INPUT       512
#define MAX_LINES       512
#define MAX_LINE_LEN    1024
#define MAX_HISTORY     64
#define ATLAS_SIZE      512
#define TITLEBAR_H      32
#define BORDER_W        2
#define SCROLL_SPEED    3

// Цвета для SDL_SetRenderDrawColor (R,G,B,A)
#define COL_BG         10,  10,  18, 255
#define COL_BORDER    255,  80, 180, 255
#define COL_TITLEBAR   18,  14,  28, 255
#define COL_BTN_CLOSE 220,  60, 100, 255
#define COL_BTN_MIN   180, 140, 255, 255

// Цвета текста (R,G,B) — alpha передаётся отдельным аргументом в draw_char/draw_string
#define TCOL_TITLE    255,  80, 180
#define TCOL_TEXT     220, 220, 235
#define TCOL_PROMPT   255,  80, 180
#define TCOL_WHITE    255, 255, 255

static const char* PROMPT_STR = "~> ";

// ============================================================
//  СТРУКТУРЫ — объявляются ДО любых функций, которые их используют
// ============================================================

typedef struct {
    SDL_Rect rect;
    int      advance;
} GlyphInfo;

typedef enum { LINE_OUTPUT, LINE_PROMPT } LineType;

typedef struct {
    char     text[MAX_LINE_LEN];
    LineType type;
    Uint32   birth_tick;
} TermLine;

typedef struct {
    Uint32 tick;
    bool   active;
} LetterAnim;

typedef struct {
    // SDL
    SDL_Window* window;
    SDL_Renderer* renderer;
    TTF_Font* font;
    SDL_Texture* atlas;
    GlyphInfo     glyphs[128];
    int           glyph_w;
    int           glyph_h;

    // Буфер вывода
    TermLine lines[MAX_LINES];
    int      line_count;
    int      scroll_offset;

    // Строка ввода
    char       input[MAX_INPUT];
    int        input_len;
    int        inp_cursor;
    LetterAnim letter_anims[MAX_INPUT];

    // Выделение в строке ввода (Shift+стрелки)
    bool inp_sel_active;
    int  inp_sel_start;
    int  inp_sel_end;

    // Выделение мышью в буфере вывода
    bool sel_active;
    bool sel_dragging;
    int  sel_anchor_line;
    int  sel_anchor_col;
    int  sel_end_line;
    int  sel_end_col;

    // История команд
    char history[MAX_HISTORY][MAX_INPUT];
    int  history_count;
    int  history_pos;

    // Перетаскивание окна
    bool dragging;
    int  drag_off_x, drag_off_y;

    bool running;

    // Область контента
    int content_x, content_y, content_w, content_h;
    int visible_lines;

    // Асинхронное выполнение команды
    SDL_Thread* cmd_thread;
    bool        cmd_running;
    SDL_mutex* lines_mutex;
} Terminal;

// Пользовательские SDL-события (инициализируются в main)
static Uint32 EV_CMD_LINE = 0;  // data1 = char* строка (heap, нужно free после)
static Uint32 EV_CMD_DONE = 0;  // команда завершена

typedef struct {
    Terminal* term;
    char      cmd[MAX_LINE_LEN];
} CmdArgs;

// ============================================================
//  ATLAS
// ============================================================

static bool build_atlas(Terminal* t) {
    t->atlas = SDL_CreateTexture(t->renderer,
        SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
        ATLAS_SIZE, ATLAS_SIZE);
    if (!t->atlas) return false;

    SDL_SetTextureBlendMode(t->atlas, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(t->renderer, t->atlas);
    SDL_SetRenderDrawColor(t->renderer, 0, 0, 0, 0);
    SDL_RenderClear(t->renderer);

    SDL_Color white = { 255, 255, 255, 255 };
    int cx = 0, cy = 0, max_h = 0;

    for (unsigned char c = 32; c <= 126; c++) {
        SDL_Surface* surf = TTF_RenderGlyph_Blended(t->font, c, white);
        if (!surf) continue;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(t->renderer, surf);
        SDL_Rect dst = { cx, cy, surf->w, surf->h };
        SDL_RenderCopy(t->renderer, tex, NULL, &dst);
        t->glyphs[c].rect = dst;
        TTF_GlyphMetrics(t->font, c, NULL, NULL, NULL, NULL, &t->glyphs[c].advance);
        if (surf->h > max_h) max_h = surf->h;
        cx += surf->w + 2;
        if (cx > ATLAS_SIZE - 32) { cx = 0; cy += max_h + 2; max_h = 0; }
        SDL_FreeSurface(surf);
        SDL_DestroyTexture(tex);
    }

    SDL_SetRenderTarget(t->renderer, NULL);
    t->glyph_w = t->glyphs['M'].advance;
    t->glyph_h = TTF_FontLineSkip(t->font);
    return true;
}

// ============================================================
//  РИСОВАНИЕ
// ============================================================

static void draw_char(Terminal* t, int x, int y, unsigned char c,
    Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (c < 32 || c > 126) c = '?';
    GlyphInfo* gi = &t->glyphs[c];
    SDL_SetTextureColorMod(t->atlas, r, g, b);
    SDL_SetTextureAlphaMod(t->atlas, a);
    SDL_Rect dst = { x, y, gi->rect.w, gi->rect.h };
    SDL_RenderCopy(t->renderer, t->atlas, &gi->rect, &dst);
}

static void draw_string(Terminal* t, int x, int y, const char* s,
    Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    for (int i = 0; s[i]; i++)
        draw_char(t, x + i * t->glyph_w, y, (unsigned char)s[i], r, g, b, a);
}

// ============================================================
//  БУФЕР ВЫВОДА
// ============================================================

static void term_add_line(Terminal* t, const char* text, LineType type) {
    // Блокируем мьютекс если он уже создан (из фонового потока)
    if (t->lines_mutex) SDL_LockMutex(t->lines_mutex);

    int len = (int)strlen(text);
    int max_chars = (t->glyph_w > 0) ? (t->content_w / t->glyph_w) : 80;
    if (max_chars < 1) max_chars = 1;

    if (len == 0) {
        TermLine* l = &t->lines[t->line_count % MAX_LINES];
        l->text[0] = '\0';
        l->type = type;
        l->birth_tick = SDL_GetTicks();
        t->line_count++;
        if (t->lines_mutex) SDL_UnlockMutex(t->lines_mutex);
        return;
    }
    for (int offset = 0; offset < len; ) {
        int chunk = len - offset;
        if (chunk > max_chars) chunk = max_chars;
        TermLine* l = &t->lines[t->line_count % MAX_LINES];
        strncpy(l->text, text + offset, chunk);
        l->text[chunk] = '\0';
        l->type = type;
        l->birth_tick = SDL_GetTicks();
        t->line_count++;
        offset += chunk;
    }

    if (t->lines_mutex) SDL_UnlockMutex(t->lines_mutex);
}

// ============================================================
//  ВЫДЕЛЕНИЕ — УТИЛИТЫ
// ============================================================

static void sel_clear(Terminal* t) {
    t->sel_active = false;
    t->sel_dragging = false;
}

static void inp_sel_clear(Terminal* t) {
    t->inp_sel_active = false;
    t->inp_sel_start = t->inp_cursor;
    t->inp_sel_end = t->inp_cursor;
}

static void sel_normalized(Terminal* t, int* sl, int* sc, int* el, int* ec) {
    int al = t->sel_anchor_line, ac = t->sel_anchor_col;
    int bl = t->sel_end_line, bc = t->sel_end_col;
    if (al < bl || (al == bl && ac <= bc)) {
        *sl = al; *sc = ac; *el = bl; *ec = bc;
    }
    else {
        *sl = bl; *sc = bc; *el = al; *ec = ac;
    }
}

static bool in_buf_sel(Terminal* t, int line, int col) {
    if (!t->sel_active) return false;
    int sl, sc, el, ec;
    sel_normalized(t, &sl, &sc, &el, &ec);
    if (line < sl || line > el)   return false;
    if (line == sl && col < sc)   return false;
    if (line == el && col >= ec)  return false;
    return true;
}

static void mouse_to_linecol(Terminal* t, int mx, int my, int* out_line, int* out_col) {
    int total = t->line_count;
    int bottom_line = total - 1 - t->scroll_offset;
    int top_line = bottom_line - t->visible_lines + 1;
    int vi = (t->glyph_h > 0) ? ((my - t->content_y) / t->glyph_h) : 0;
    int li = top_line + vi;
    if (li < 0)      li = 0;
    if (li >= total && total > 0) li = total - 1;
    int col = (t->glyph_w > 0) ? ((mx - t->content_x) / t->glyph_w) : 0;
    if (li >= 0 && li < total) {
        int len = (int)strlen(t->lines[li % MAX_LINES].text);
        if (col < 0)   col = 0;
        if (col > len) col = len;
    }
    else col = 0;
    *out_line = li;
    *out_col = col;
}

static void copy_buf_sel(Terminal* t) {
    if (!t->sel_active) return;
    int sl, sc, el, ec;
    sel_normalized(t, &sl, &sc, &el, &ec);
    if (sl == el && sc == ec) return;

    char buf[65536];
    int  pos = 0;
    for (int li = sl; li <= el && li < t->line_count; li++) {
        TermLine* line = &t->lines[li % MAX_LINES];
        int len = (int)strlen(line->text);
        int from = (li == sl) ? sc : 0;
        int to = (li == el) ? ec : len;
        if (from > len) from = len;
        if (to > len) to = len;
        for (int ci = from; ci < to && pos < (int)sizeof(buf) - 3; ci++)
            buf[pos++] = line->text[ci];
        if (li < el && pos < (int)sizeof(buf) - 3) {
            buf[pos++] = '\r'; buf[pos++] = '\n';
        }
    }
    buf[pos] = '\0';
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wlen * sizeof(WCHAR));
    if (!hMem) return;
    WCHAR* wbuf = (WCHAR*)GlobalLock(hMem);
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, wlen);
    GlobalUnlock(hMem);
    if (OpenClipboard(NULL)) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, hMem); CloseClipboard(); }
    else GlobalFree(hMem);
#endif
}

static void copy_inp_sel(Terminal* t) {
    if (!t->inp_sel_active) return;
    int s = t->inp_sel_start, e = t->inp_sel_end;
    if (s > e) { int tmp = s; s = e; e = tmp; }
    if (s == e) return;
    int len = e - s;
    if (len >= MAX_INPUT) len = MAX_INPUT - 1;
    char buf[MAX_INPUT];
    strncpy(buf, t->input + s, len);
    buf[len] = '\0';
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wlen * sizeof(WCHAR));
    if (!hMem) return;
    WCHAR* wbuf = (WCHAR*)GlobalLock(hMem);
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, wlen);
    GlobalUnlock(hMem);
    if (OpenClipboard(NULL)) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, hMem); CloseClipboard(); }
    else GlobalFree(hMem);
#endif
}

// ============================================================
//  ВЫПОЛНЕНИЕ КОМАНДЫ — фоновый поток
// ============================================================

// Отправляет строку в главный поток через SDL event queue (thread-safe)
static void push_line(Terminal* t, const char* text, LineType type) {
    // Кодируем тип в старший бит указателя — нет, лучше просто
    // вызываем term_add_line напрямую (мьютекс внутри)
    term_add_line(t, text, type);
    // Будим главный поток чтобы он перерисовал экран
    SDL_Event wake;
    SDL_memset(&wake, 0, sizeof(wake));
    wake.type = EV_CMD_LINE;
    SDL_PushEvent(&wake);
}

static int cmd_thread_func(void* arg) {
    CmdArgs* args = (CmdArgs*)arg;
    Terminal* t = args->term;
    const char* cmd = args->cmd;

#ifdef _WIN32
    char full_cmd[MAX_LINE_LEN + 16];
    snprintf(full_cmd, sizeof(full_cmd), "cmd.exe /C %s", cmd);

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        push_line(t, "[error: pipe failed]", LINE_OUTPUT);
        goto done;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessA(NULL, full_cmd, NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        push_line(t, "[error: CreateProcess failed]", LINE_OUTPUT);
        CloseHandle(hWrite); CloseHandle(hRead);
        goto done;
    }
    CloseHandle(hWrite);

    char accum[MAX_LINE_LEN * 2] = "";
    char chunk[512];
    DWORD bytes_read;
    while (ReadFile(hRead, chunk, (DWORD)(sizeof(chunk) - 1), &bytes_read, NULL) && bytes_read > 0) {
        chunk[bytes_read] = '\0';
        if (strlen(accum) + bytes_read < sizeof(accum) - 1)
            strcat(accum, chunk);
        char* p = accum;
        char* nl;
        while ((nl = strchr(p, '\n')) != NULL) {
            int ll = (int)(nl - p);
            if (ll > 0 && p[ll - 1] == '\r') ll--;
            char line_buf[MAX_LINE_LEN];
            if (ll >= MAX_LINE_LEN) ll = MAX_LINE_LEN - 1;
            strncpy(line_buf, p, ll);
            line_buf[ll] = '\0';
            push_line(t, line_buf, LINE_OUTPUT);
            p = nl + 1;
        }
        memmove(accum, p, strlen(p) + 1);
    }
    if (strlen(accum) > 0) {
        int l = (int)strlen(accum);
        if (l > 0 && accum[l - 1] == '\r') accum[l - 1] = '\0';
        push_line(t, accum, LINE_OUTPUT);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hRead);
#else
    FILE* fp = popen(cmd, "r");
    if (!fp) { push_line(t, "[error]", LINE_OUTPUT); goto done; }
    char buf[MAX_LINE_LEN];
    while (fgets(buf, sizeof(buf), fp)) {
        int l = (int)strlen(buf);
        if (l > 0 && buf[l - 1] == '\n') buf[l - 1] = '\0';
        push_line(t, buf, LINE_OUTPUT);
    }
    pclose(fp);
#endif

    push_line(t, "", LINE_OUTPUT);

done:
    free(args);

    // Сигнал главному потоку: команда завершена
    SDL_Event done_ev;
    SDL_memset(&done_ev, 0, sizeof(done_ev));
    done_ev.type = EV_CMD_DONE;
    SDL_PushEvent(&done_ev);
    return 0;
}

// Запускает команду асинхронно
static void execute_command(Terminal* t, const char* cmd) {
    // Печатаем промпт сразу в главном потоке
    char prompt_line[MAX_LINE_LEN];
    snprintf(prompt_line, sizeof(prompt_line), "%s%s", PROMPT_STR, cmd);
    term_add_line(t, prompt_line, LINE_PROMPT);

    if (strlen(cmd) == 0) return;

    // Если уже выполняется — ждём завершения предыдущего потока
    if (t->cmd_thread) {
        SDL_WaitThread(t->cmd_thread, NULL);
        t->cmd_thread = NULL;
    }

    CmdArgs* args = (CmdArgs*)malloc(sizeof(CmdArgs));
    if (!args) { term_add_line(t, "[error: out of memory]", LINE_OUTPUT); return; }
    args->term = t;
    strncpy(args->cmd, cmd, MAX_LINE_LEN - 1);
    args->cmd[MAX_LINE_LEN - 1] = '\0';

    t->cmd_running = true;
    t->cmd_thread = SDL_CreateThread(cmd_thread_func, "cmd", args);
    if (!t->cmd_thread) {
        term_add_line(t, "[error: thread failed]", LINE_OUTPUT);
        free(args);
        t->cmd_running = false;
    }
}

// ============================================================
//  ИСТОРИЯ
// ============================================================

static void history_up(Terminal* t) {
    if (t->history_count == 0) return;
    if (t->history_pos == -1) t->history_pos = t->history_count - 1;
    else if (t->history_pos > 0) t->history_pos--;
    strncpy(t->input, t->history[t->history_pos], MAX_INPUT - 1);
    t->input[MAX_INPUT - 1] = '\0';
    t->input_len = (int)strlen(t->input);
    t->inp_cursor = t->input_len;
    inp_sel_clear(t);
    memset(t->letter_anims, 0, sizeof(t->letter_anims));
}

static void history_down(Terminal* t) {
    if (t->history_pos == -1) return;
    t->history_pos++;
    if (t->history_pos >= t->history_count) {
        t->history_pos = -1;
        t->input[0] = '\0';
        t->input_len = 0;
    }
    else {
        strncpy(t->input, t->history[t->history_pos], MAX_INPUT - 1);
        t->input[MAX_INPUT - 1] = '\0';
        t->input_len = (int)strlen(t->input);
    }
    t->inp_cursor = t->input_len;
    inp_sel_clear(t);
    memset(t->letter_anims, 0, sizeof(t->letter_anims));
}

static void handle_return(Terminal* t) {
    if (t->input_len > 0) {
        if (t->history_count < MAX_HISTORY) {
            strncpy(t->history[t->history_count], t->input, MAX_INPUT - 1);
            t->history[t->history_count][MAX_INPUT - 1] = '\0';
            t->history_count++;
        }
        else {
            memmove(t->history[0], t->history[1], (MAX_HISTORY - 1) * MAX_INPUT);
            strncpy(t->history[MAX_HISTORY - 1], t->input, MAX_INPUT - 1);
        }
        t->history_pos = -1;
    }
    char cmd[MAX_INPUT];
    strncpy(cmd, t->input, MAX_INPUT - 1);
    cmd[MAX_INPUT - 1] = '\0';

    t->input[0] = '\0';
    t->input_len = 0;
    t->inp_cursor = 0;
    t->inp_sel_active = false;
    t->inp_sel_start = 0;
    t->inp_sel_end = 0;
    memset(t->letter_anims, 0, sizeof(t->letter_anims));
    t->scroll_offset = 0;
    execute_command(t, cmd);
}

// ============================================================
//  HIT-TEST
// ============================================================

static bool is_close_btn(int mx, int my) {
    int bs = 16, by = BORDER_W + (TITLEBAR_H - 16) / 2;
    int cx = SCREEN_WIDTH - BORDER_W - 10 - bs;
    return mx >= cx && mx <= cx + bs && my >= by && my <= by + bs;
}

static bool is_min_btn(int mx, int my) {
    int bs = 16, by = BORDER_W + (TITLEBAR_H - 16) / 2;
    int cx = SCREEN_WIDTH - BORDER_W - 10 - bs;
    int mn = cx - bs - 8;
    return mx >= mn && mx <= mn + bs && my >= by && my <= by + bs;
}

static bool in_titlebar(int my) {
    return my >= BORDER_W && my <= BORDER_W + TITLEBAR_H;
}

// ============================================================
//  ОТРИСОВКА — РАМКА
// ============================================================

static void draw_border(Terminal* t) {
    SDL_SetRenderDrawColor(t->renderer, COL_BORDER);
    SDL_Rect sides[4] = {
        { 0, 0, SCREEN_WIDTH, BORDER_W },
        { 0, SCREEN_HEIGHT - BORDER_W, SCREEN_WIDTH, BORDER_W },
        { 0, 0, BORDER_W, SCREEN_HEIGHT },
        { SCREEN_WIDTH - BORDER_W, 0, BORDER_W, SCREEN_HEIGHT }
    };
    for (int i = 0; i < 4; i++) SDL_RenderFillRect(t->renderer, &sides[i]);

    SDL_SetRenderDrawBlendMode(t->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(t->renderer, 255, 80, 180, 35);
    SDL_Rect glow[4] = {
        { 0, BORDER_W, SCREEN_WIDTH, 2 },
        { 0, SCREEN_HEIGHT - BORDER_W - 2, SCREEN_WIDTH, 2 },
        { BORDER_W, 0, 2, SCREEN_HEIGHT },
        { SCREEN_WIDTH - BORDER_W - 2, 0, 2, SCREEN_HEIGHT }
    };
    for (int i = 0; i < 4; i++) SDL_RenderFillRect(t->renderer, &glow[i]);
    SDL_SetRenderDrawBlendMode(t->renderer, SDL_BLENDMODE_NONE);
}

// ============================================================
//  ОТРИСОВКА — ЗАГОЛОВОК
// ============================================================

static void draw_titlebar(Terminal* t) {


    int ty = BORDER_W + (TITLEBAR_H - t->glyph_h) / 2;
    draw_string(t, BORDER_W + 12, ty, "kerzoix terminal v1.0", TCOL_TITLE, 255);

    int bs = 16, by = BORDER_W + (TITLEBAR_H - bs) / 2;
    int cx = SCREEN_WIDTH - BORDER_W - 10 - bs;
    int mn = cx - bs - 8;

    SDL_SetRenderDrawColor(t->renderer, COL_BTN_CLOSE);
    SDL_Rect cr = { cx, by, bs, bs };
    SDL_RenderFillRect(t->renderer, &cr);
    draw_string(t, cx + 3, by + 1, "x", TCOL_WHITE, 220);

    SDL_SetRenderDrawColor(t->renderer, COL_BTN_MIN);
    SDL_Rect mr = { mn, by, bs, bs };
    SDL_RenderFillRect(t->renderer, &mr);
    draw_string(t, mn + 4, by + 1, "_", TCOL_WHITE, 220);
}

// ============================================================
//  ОТРИСОВКА — БУФЕР ВЫВОДА
// ============================================================

static void draw_output(Terminal* t) {
    Uint32 now = SDL_GetTicks();

    if (t->lines_mutex) SDL_LockMutex(t->lines_mutex);

    int    total = t->line_count;
    int    bottom_line = total - 1 - t->scroll_offset;
    int    top_line = bottom_line - t->visible_lines + 1;
    int    prompt_len = (int)strlen(PROMPT_STR);

    SDL_SetRenderDrawBlendMode(t->renderer, SDL_BLENDMODE_BLEND);

    for (int vi = 0; vi < t->visible_lines; vi++) {
        int li = top_line + vi;
        if (li < 0 || li >= total) continue;

        TermLine* line = &t->lines[li % MAX_LINES];
        int       len = (int)strlen(line->text);
        int       dy = t->content_y + vi * t->glyph_h;

        // Fade-in за 200мс
        float age = (float)(now - line->birth_tick);
        float fade = age / 200.0f;
        if (fade > 1.0f) fade = 1.0f;
        Uint8 alpha = (Uint8)(fade * 255.0f);

        // Подсветка выделения (фон)
        if (t->sel_active) {
            int sl, sc, el, ec;
            sel_normalized(t, &sl, &sc, &el, &ec);
            if (li >= sl && li <= el) {
                int from = (li == sl) ? sc : 0;
                int to = (li == el) ? ec : len;
                if (from < 0)   from = 0;
                if (to > len) to = len;
                if (from < to) {
                    SDL_SetRenderDrawColor(t->renderer, 255, 80, 180, 55);
                    SDL_Rect hl = {
                        t->content_x + from * t->glyph_w, dy,
                        (to - from) * t->glyph_w, t->glyph_h
                    };
                    SDL_RenderFillRect(t->renderer, &hl);
                }
            }
        }

        // Текст посимвольно
        for (int ci = 0; ci < len; ci++) {
            bool sel = in_buf_sel(t, li, ci);
            Uint8 r, g, b;
            if (sel) {
                r = 10; g = 10; b = 18;
            }
            else if (line->type == LINE_PROMPT && ci < prompt_len) {
                r = 255; g = 80; b = 180;
            }
            else {
                r = 220; g = 220; b = 235;
            }
            draw_char(t, t->content_x + ci * t->glyph_w, dy,
                (unsigned char)line->text[ci], r, g, b, alpha);
        }
    }

    SDL_SetRenderDrawBlendMode(t->renderer, SDL_BLENDMODE_NONE);

    if (t->lines_mutex) SDL_UnlockMutex(t->lines_mutex);
}

// ============================================================
//  ОТРИСОВКА — СТРОКА ВВОДА
// ============================================================

static void draw_input_line(Terminal* t) {
    Uint32 now = SDL_GetTicks();
    int    prompt_len = (int)strlen(PROMPT_STR);
    int    y = t->content_y + t->visible_lines * t->glyph_h;
    int    base_x = t->content_x + prompt_len * t->glyph_w;

    // Если команда выполняется — показываем спиннер вместо строки ввода
    if (t->cmd_running) {
        static const char* frames[] = { "|", "/", "-", "\\" };
        int frame = (now / 120) % 4;
        draw_string(t, t->content_x, y, "running ", TCOL_PROMPT, 180);
        draw_string(t, t->content_x + 8 * t->glyph_w, y,
            frames[frame], TCOL_PROMPT, 255);
        return;
    }

    draw_string(t, t->content_x, y, PROMPT_STR, TCOL_PROMPT, 255);

    // Нормализованные границы inp-выделения
    int isel_s = t->inp_sel_start, isel_e = t->inp_sel_end;
    if (isel_s > isel_e) { int tmp = isel_s; isel_s = isel_e; isel_e = tmp; }

    // Фон выделения
    if (t->inp_sel_active && isel_s < isel_e) {
        SDL_SetRenderDrawBlendMode(t->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(t->renderer, 255, 80, 180, 70);
        SDL_Rect hl = { base_x + isel_s * t->glyph_w, y,
                        (isel_e - isel_s) * t->glyph_w, t->glyph_h };
        SDL_RenderFillRect(t->renderer, &hl);
        SDL_SetRenderDrawBlendMode(t->renderer, SDL_BLENDMODE_NONE);
    }

    // Буквы с анимацией
    for (int i = 0; i < t->input_len; i++) {
        LetterAnim* la = &t->letter_anims[i];
        float offset_y = 0.0f;
        Uint8 alpha = 255;

        if (la->active) {
            float age = (float)(now - la->tick);
            float t01 = age / 120.0f;
            if (t01 >= 1.0f) { t01 = 1.0f; la->active = false; }
            float ease = 1.0f - (1.0f - t01) * (1.0f - t01) * (1.0f - t01);
            offset_y = (1.0f - ease) * (-8.0f);
            alpha = (Uint8)(ease * 255.0f);
        }

        bool  sel = t->inp_sel_active && i >= isel_s && i < isel_e;
        Uint8 r = sel ? 10 : 255;
        Uint8 g = sel ? 10 : 255;
        Uint8 b = sel ? 18 : 255;
        draw_char(t, base_x + i * t->glyph_w, y + (int)offset_y,
            (unsigned char)t->input[i], r, g, b, alpha);
    }

    // Курсор — плавное мигание через sin
    int   cursor_x = base_x + t->inp_cursor * t->glyph_w;
    float blink = sinf((float)now / 500.0f * 3.14159f) * 0.5f + 0.5f;
    Uint8 ca = (Uint8)(blink * 190.0f + 65.0f);
    SDL_SetRenderDrawBlendMode(t->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(t->renderer, 255, 80, 180, ca);
    SDL_Rect cur_rect = { cursor_x, y + 2, 2, t->glyph_h - 2 };
    SDL_RenderFillRect(t->renderer, &cur_rect);
    SDL_SetRenderDrawBlendMode(t->renderer, SDL_BLENDMODE_NONE);
}

// ============================================================
//  ОТРИСОВКА — СКРОЛЛБАР
// ============================================================

static void draw_scrollbar(Terminal* t) {
    if (t->line_count <= t->visible_lines) return;

    int track_x = SCREEN_WIDTH - BORDER_W - 6;
    int track_y = BORDER_W + TITLEBAR_H + 4;
    int track_h = t->content_h - 4;

    SDL_SetRenderDrawBlendMode(t->renderer, SDL_BLENDMODE_BLEND);

    SDL_SetRenderDrawColor(t->renderer, 255, 80, 180, 30);
    SDL_Rect track = { track_x, track_y, 3, track_h };
    SDL_RenderFillRect(t->renderer, &track);

    float ratio_h = (float)t->visible_lines / (float)t->line_count;
    int   ms = t->line_count - t->visible_lines;
    float ratio_pos = (ms > 0) ?
        (float)(ms - t->scroll_offset) / (float)ms : 1.0f;
    int thumb_h = (int)(ratio_h * (float)track_h);
    if (thumb_h < 8) thumb_h = 8;
    int thumb_y = track_y + (int)(ratio_pos * (float)(track_h - thumb_h));

    SDL_SetRenderDrawColor(t->renderer, 255, 80, 180, 160);
    SDL_Rect thumb = { track_x, thumb_y, 3, thumb_h };
    SDL_RenderFillRect(t->renderer, &thumb);

    SDL_SetRenderDrawBlendMode(t->renderer, SDL_BLENDMODE_NONE);
}

// ============================================================
//  MAIN
// ============================================================

int main(int argc, char* argv[]) {
    Terminal t;
    memset(&t, 0, sizeof(t));
    t.running = true;
    t.history_pos = -1;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    if (TTF_Init() < 0) { fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); return 1; }

    // Регистрируем пользовательские события для связи потоков
    EV_CMD_LINE = SDL_RegisterEvents(2);
    EV_CMD_DONE = EV_CMD_LINE + 1;

    t.lines_mutex = SDL_CreateMutex();
    if (!t.lines_mutex) { fprintf(stderr, "Mutex failed.\n"); return 1; }

    t.window = SDL_CreateWindow("Kerzoix Terminal",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT,
        SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS);
    if (!t.window) { fprintf(stderr, "Window: %s\n", SDL_GetError()); return 1; }

    t.renderer = SDL_CreateRenderer(t.window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!t.renderer) { fprintf(stderr, "Renderer: %s\n", SDL_GetError()); return 1; }

#ifdef _WIN32
    SDL_SysWMinfo wmi;
    SDL_VERSION(&wmi.version);
    SDL_GetWindowWMInfo(t.window, &wmi);
    HWND hwnd = wmi.info.win.window;
    SetWindowLong(hwnd, GWL_EXSTYLE,
        GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, 235, LWA_ALPHA);
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
        &corner, sizeof(corner));
#endif

    t.font = TTF_OpenFont("consola.ttf", FONT_SIZE);
    if (!t.font)
        t.font = TTF_OpenFont("C:\\Windows\\Fonts\\consola.ttf", FONT_SIZE);
    if (!t.font) { fprintf(stderr, "Font not found.\n"); return 1; }

    if (!build_atlas(&t)) { fprintf(stderr, "Atlas failed.\n"); return 1; }

    int pad = 14;
    t.content_x = BORDER_W + pad;
    t.content_y = BORDER_W + TITLEBAR_H + pad / 2;
    t.content_w = SCREEN_WIDTH - BORDER_W * 2 - pad * 2 - 10;
    t.content_h = SCREEN_HEIGHT - BORDER_W * 2 - TITLEBAR_H - pad - t.glyph_h - 4;
    t.visible_lines = t.content_h / t.glyph_h;

    term_add_line(&t, "Kerzoix Terminal v1.0", LINE_OUTPUT);
    term_add_line(&t, "Ctrl+C: copy/^C  |  Ctrl+A: select all  |  Ctrl+L: clear  |  Shift+arrows / mouse drag: select", LINE_OUTPUT);
    term_add_line(&t, "", LINE_OUTPUT);

    SDL_StartTextInput();
    SDL_Event ev;

    while (t.running) {
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {

            case SDL_QUIT:
                t.running = false;
                break;

                // ------------------------------------------------
                // Фоновый поток добавил строку — просто перерисуем
            default:
                if (ev.type == EV_CMD_LINE) {
                    // Автоскролл вниз если пользователь не скроллил вверх
                    if (t.scroll_offset == 0)
                        t.scroll_offset = 0; // уже 0, просто перерисуем
                    break;
                }
                if (ev.type == EV_CMD_DONE) {
                    t.cmd_running = false;
                    if (t.cmd_thread) {
                        SDL_WaitThread(t.cmd_thread, NULL);
                        t.cmd_thread = NULL;
                    }
                    // Автоскролл вниз после завершения команды
                    t.scroll_offset = 0;
                    break;
                }
                break;

                // ------------------------------------------------
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    int mx = ev.button.x, my = ev.button.y;
                    if (is_close_btn(mx, my)) {
                        t.running = false;
                    }
                    else if (is_min_btn(mx, my)) {
#ifdef _WIN32
                        ShowWindow(hwnd, SW_MINIMIZE);
#endif
                    }
                    else if (in_titlebar(my)) {
                        t.dragging = true;
                        t.drag_off_x = mx;
                        t.drag_off_y = my;
                    }
                    else {
                        sel_clear(&t);
                        inp_sel_clear(&t);
                        int li, col;
                        mouse_to_linecol(&t, mx, my, &li, &col);
                        t.sel_anchor_line = li;
                        t.sel_anchor_col = col;
                        t.sel_end_line = li;
                        t.sel_end_col = col;
                        t.sel_active = false;
                        t.sel_dragging = true;
                    }
                }
                break;

                // ------------------------------------------------
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    t.dragging = false;
                    t.sel_dragging = false;
                }
                break;

                // ------------------------------------------------
            case SDL_MOUSEMOTION:
                if (t.dragging) {
                    int wx, wy;
                    SDL_GetWindowPosition(t.window, &wx, &wy);
                    SDL_SetWindowPosition(t.window,
                        wx + ev.motion.x - t.drag_off_x,
                        wy + ev.motion.y - t.drag_off_y);
                }
                else if (t.sel_dragging) {
                    int li, col;
                    mouse_to_linecol(&t, ev.motion.x, ev.motion.y, &li, &col);
                    t.sel_end_line = li;
                    t.sel_end_col = col;
                    if (li != t.sel_anchor_line || col != t.sel_anchor_col)
                        t.sel_active = true;
                }
                break;

                // ------------------------------------------------
            case SDL_MOUSEWHEEL: {
                t.scroll_offset -= ev.wheel.y * SCROLL_SPEED;
                if (t.scroll_offset < 0) t.scroll_offset = 0;
                int ms = t.line_count - t.visible_lines;
                if (ms < 0) ms = 0;
                if (t.scroll_offset > ms) t.scroll_offset = ms;
                break;
            }

                               // ------------------------------------------------
            case SDL_TEXTINPUT:
                if (t.cmd_running) break;  // Блокируем ввод пока выполняется команда
                if (t.input_len < MAX_INPUT - 1) {
                    if (t.inp_sel_active) {
                        int s = t.inp_sel_start, e = t.inp_sel_end;
                        if (s > e) { int tmp = s; s = e; e = tmp; }
                        memmove(t.input + s, t.input + e, t.input_len - e + 1);
                        t.input_len -= (e - s);
                        t.inp_cursor = s;
                        inp_sel_clear(&t);
                    }
                    int cur = t.inp_cursor;
                    memmove(t.input + cur + 1, t.input + cur, t.input_len - cur + 1);
                    t.input[cur] = ev.text.text[0];
                    t.input_len++;
                    t.inp_cursor++;
                    t.letter_anims[cur].active = true;
                    t.letter_anims[cur].tick = SDL_GetTicks();
                    sel_clear(&t);
                    t.scroll_offset = 0;
                }
                break;

                // ------------------------------------------------
            case SDL_KEYDOWN: {
                int  mod = ev.key.keysym.mod;
                bool shift = (mod & KMOD_SHIFT) != 0;
                bool ctrl = (mod & KMOD_CTRL) != 0;

                switch (ev.key.keysym.sym) {

                case SDLK_BACKSPACE:
                    if (t.inp_sel_active) {
                        int s = t.inp_sel_start, e = t.inp_sel_end;
                        if (s > e) { int tmp = s; s = e; e = tmp; }
                        memmove(t.input + s, t.input + e, t.input_len - e + 1);
                        t.input_len -= (e - s);
                        t.inp_cursor = s;
                        inp_sel_clear(&t);
                    }
                    else if (t.inp_cursor > 0) {
                        memmove(t.input + t.inp_cursor - 1,
                            t.input + t.inp_cursor,
                            t.input_len - t.inp_cursor + 1);
                        t.input_len--;
                        t.inp_cursor--;
                    }
                    break;

                case SDLK_DELETE:
                    if (t.inp_sel_active) {
                        int s = t.inp_sel_start, e = t.inp_sel_end;
                        if (s > e) { int tmp = s; s = e; e = tmp; }
                        memmove(t.input + s, t.input + e, t.input_len - e + 1);
                        t.input_len -= (e - s);
                        t.inp_cursor = s;
                        inp_sel_clear(&t);
                    }
                    else if (t.inp_cursor < t.input_len) {
                        memmove(t.input + t.inp_cursor,
                            t.input + t.inp_cursor + 1,
                            t.input_len - t.inp_cursor);
                        t.input_len--;
                    }
                    break;

                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    inp_sel_clear(&t);
                    sel_clear(&t);
                    handle_return(&t);
                    break;

                case SDLK_LEFT:
                    if (shift) {
                        if (!t.inp_sel_active) { t.inp_sel_active = true; t.inp_sel_start = t.inp_cursor; }
                        if (t.inp_cursor > 0) t.inp_cursor--;
                        t.inp_sel_end = t.inp_cursor;
                    }
                    else { inp_sel_clear(&t); if (t.inp_cursor > 0) t.inp_cursor--; }
                    break;

                case SDLK_RIGHT:
                    if (shift) {
                        if (!t.inp_sel_active) { t.inp_sel_active = true; t.inp_sel_start = t.inp_cursor; }
                        if (t.inp_cursor < t.input_len) t.inp_cursor++;
                        t.inp_sel_end = t.inp_cursor;
                    }
                    else { inp_sel_clear(&t); if (t.inp_cursor < t.input_len) t.inp_cursor++; }
                    break;

                case SDLK_HOME:
                    if (shift) {
                        if (!t.inp_sel_active) { t.inp_sel_active = true; t.inp_sel_start = t.inp_cursor; }
                        t.inp_cursor = 0; t.inp_sel_end = 0;
                    }
                    else { inp_sel_clear(&t); t.inp_cursor = 0; }
                    break;

                case SDLK_END:
                    if (shift) {
                        if (!t.inp_sel_active) { t.inp_sel_active = true; t.inp_sel_start = t.inp_cursor; }
                        t.inp_cursor = t.input_len; t.inp_sel_end = t.input_len;
                    }
                    else { inp_sel_clear(&t); t.inp_cursor = t.input_len; }
                    break;

                case SDLK_UP:    if (!shift) history_up(&t);   break;
                case SDLK_DOWN:  if (!shift) history_down(&t); break;

                case SDLK_PAGEUP: {
                    t.scroll_offset += t.visible_lines;
                    int ms = t.line_count - t.visible_lines;
                    if (ms < 0) ms = 0;
                    if (t.scroll_offset > ms) t.scroll_offset = ms;
                    break;
                }
                case SDLK_PAGEDOWN:
                    t.scroll_offset -= t.visible_lines;
                    if (t.scroll_offset < 0) t.scroll_offset = 0;
                    break;

                case SDLK_ESCAPE:
                    sel_clear(&t);
                    inp_sel_clear(&t);
                    break;

                case SDLK_a:
                    if (ctrl) {
                        t.inp_sel_active = true;
                        t.inp_sel_start = 0;
                        t.inp_sel_end = t.input_len;
                        t.inp_cursor = t.input_len;
                    }
                    break;

                case SDLK_c:
                    if (ctrl) {
                        if (t.sel_active)
                            copy_buf_sel(&t);           // Выделение в буфере — копируем
                        else if (t.inp_sel_active)
                            copy_inp_sel(&t);           // Выделение в строке ввода — копируем
                        else {
                            // Нет выделения — ^C как в bash
                            t.input[0] = '\0';
                            t.input_len = 0;
                            t.inp_cursor = 0;
                            t.inp_sel_active = false;
                            memset(t.letter_anims, 0, sizeof(t.letter_anims));
                            term_add_line(&t, "^C", LINE_OUTPUT);
                        }
                    }
                    break;

                case SDLK_l:
                    if (ctrl) { t.line_count = 0; t.scroll_offset = 0; }
                    break;

                } // switch sym
                break;
            } // SDL_KEYDOWN

            } // switch ev.type
        } // PollEvent

        SDL_SetRenderDrawColor(t.renderer, COL_BG);
        SDL_RenderClear(t.renderer);
        draw_border(&t);
        draw_titlebar(&t);
        draw_output(&t);
        draw_input_line(&t);
        draw_scrollbar(&t);
        SDL_RenderPresent(t.renderer);
    }

    SDL_StopTextInput();
    SDL_DestroyTexture(t.atlas);
    TTF_CloseFont(t.font);
    SDL_DestroyRenderer(t.renderer);
    SDL_DestroyWindow(t.window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}