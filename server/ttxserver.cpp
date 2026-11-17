/*
 * ttxserver.cpp  -  Multi-client Teletext page server
 * ----------------------------------------------------------------
 * Builds on Windows (Dev-C++) and on Linux / Raspberry Pi (g++).
 *
 *  - Pages are loaded from .tti files in the "pages" folder. If the folder
 *    is empty, a set of demo pages is created there for you to edit.
 *  - Pages with several subpages rotate as a "carousel"; every client
 *    viewing the page gets the new subpage pushed to it automatically.
 *  - Row 0 (the header) is built live with a running clock.
 *  - The pages folder is checked every 2 seconds and reloaded when a
 *    file is added, changed or deleted.
 *  - ttxedit can list, open, save and delete pages over the network.
 *    Set password= in ttxserver.cfg so only you can change pages.
 *
 * Phones and other browsers: http://<server>:8080  (webport= in the cfg;
 * the page itself is web/index.html next to the server)
 *
 * Usage:   ttxserver [port] [service-name]
 * ttxserver.cfg (optional, in the folder you run it from):
 *          port=8023
 *          name=TELETEXT
 *          password=secret        (needed by ttxedit to save/delete)
 *          webport=8080           browser client port (0 = off)
 *          webdir=web             folder holding index.html etc.
 *          webeditremote=0        1 = allow web editing through a Cloudflare tunnel
 * Console: R = reload  L = list clients  P = list pages  Q = quit
 *          (on Linux type the letter and press Enter)
 *
 * Build:
 *   Dev-C++ : Console Application, Linker: -lws2_32
 *   Linux   : g++ -O2 -o ttxserver ttxserver.cpp
 *
 * Protocol (one command per line):
 *   client -> server
 *     GET ppp [s]            view page ppp (hex), optional fixed subpage
 *     LIST                   -> "PAGES ppp ppp ...\n"
 *     GETTTI ppp             -> "TTI ppp len\n" + len bytes of .tti text
 *     AUTH password          -> "OK\n" or "ERR ...\n"
 *     PUTTTI ppp len         followed by len bytes: save page -> "OK ppp\n"
 *     DELETE ppp             delete pages/Pppp.tti -> "OK ppp\n"
 *     QUIT
 *   server -> client (viewing)
 *     "PAGE ppp s n red green yellow cyan index\n" + 1000 bytes
 *     "HDR\n" + 40 bytes   (header row with clock)
 *     "NOPAGE ppp\n"
 *     "FLASH ppp headline\n"  newsflash (from pages/newsflash.txt), "FLASH\n" = clear
 */

#ifdef _WIN32
  #define FD_SETSIZE 256          /* must be before winsock2.h */
  #include <winsock2.h>
  #include <windows.h>
  #include <conio.h>
  typedef int socklen_t;
  #define SEND_FLAGS 0
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <sys/stat.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <dirent.h>
  #include <signal.h>
  #include <errno.h>
  #include <strings.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  #define closesocket    close
  #ifdef MSG_NOSIGNAL
    #define SEND_FLAGS   MSG_NOSIGNAL
  #else
    #define SEND_FLAGS   0           /* macOS has no MSG_NOSIGNAL; main() ignores SIGPIPE instead */
  #endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#define TTX_ROWS      25
#define TTX_COLS      40
#define DEFAULT_PORT  8023
#define MAX_CLIENTS   200
#define MAX_PUT       65536
#define PAGE_DIR      "pages"

/* ---- Teletext spacing attributes, as string pieces for building pages ----
 * Always put these as separate literals, e.g.  A_RED "Hello"
 * (never "\x01Hello" - the compiler would read \x01He as one hex number!)  */
#define A_RED         "\x01"
#define A_GREEN       "\x02"
#define A_YELLOW      "\x03"
#define A_BLUE        "\x04"
#define A_MAGENTA     "\x05"
#define A_CYAN        "\x06"
#define A_WHITE       "\x07"
#define FLASH         "\x08"
#define STEADY        "\x09"
#define NORMAL_SIZE   "\x0C"
#define DOUBLE_HEIGHT "\x0D"
#define G_RED         "\x11"
#define G_GREEN       "\x12"
#define G_YELLOW      "\x13"
#define G_BLUE        "\x14"
#define G_MAGENTA     "\x15"
#define G_CYAN        "\x16"
#define G_WHITE       "\x17"
#define CONCEAL       "\x18"
#define CONTIGUOUS    "\x19"
#define SEPARATED     "\x1A"
#define BLACK_BG      "\x1C"
#define NEW_BG        "\x1D"
#define HOLD_GFX      "\x1E"
#define RELEASE_GFX   "\x1F"

/* ------------------------------------------------------------------ */
/*  Data                                                               */
/* ------------------------------------------------------------------ */

struct SubPage {
    unsigned char cell[TTX_ROWS][TTX_COLS];
};

struct Page {
    int                  number;      /* 0x100 .. 0x8FF                     */
    std::vector<SubPage> subs;        /* one or more subpages               */
    int                  cycleSecs;   /* carousel time                      */
    int                  links[6];    /* fastext: R G Y C (unused) index    */
    int                  current;     /* subpage currently "on air"         */
    time_t               lastCycle;
    std::string          source;      /* file it came from                  */
    std::string          desc;        /* DE, line                           */

    Page() : number(0), cycleSecs(8), current(0), lastCycle(0)
    {
        for (int i = 0; i < 6; i++) links[i] = 0;
    }
};

struct Client {
    SOCKET      sock;
    std::string inbuf;
    int         page;       /* page being viewed, 0 = none            */
    int         fixedSub;   /* -1 = follow carousel, else subpage idx */
    bool        dead;
    bool        authed;     /* gave the edit password                 */
    int         putPage;    /* PUTTTI in progress: page and length    */
    int         putLen;
    bool        putAllowed;
    char        addr[48];
};

static std::map<int, Page>  g_pages;
static std::vector<Client>  g_clients;
static std::string          g_serviceName = "TELETEXT";
static std::string          g_password;
static std::string          g_signature;
static std::string          g_flash;          /* current newsflash "ppp headline", or "" */

/* the web (browser) client: a small HTTP server on its own port */
struct HttpConn { SOCKET sock; std::string buf; time_t opened; bool dead; char addr[48]; };
static std::vector<HttpConn> g_http;
static int                   g_webPort = 8080;     /* 0 = off */
static std::string           g_webDir  = "web";
static bool                  g_webEditRemote = false;  /* allow editing via a Cloudflare tunnel */
static int                   g_badTries  = 0;          /* wrong web passwords in a row      */
static time_t                g_lockUntil = 0;          /* no web logins until this time     */

static void ClearSub(SubPage &s) { memset(s.cell, ' ', sizeof(s.cell)); }

/* ------------------------------------------------------------------ */
/*  Windows / Linux differences                                        */
/* ------------------------------------------------------------------ */

static void SleepMs(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void MakeDir(const char *path)
{
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0755);
#endif
}

/* Replace dst with tmp in one step, so nobody reads a half-written file */
static bool ReplaceWith(const char *tmp, const char *dst)
{
#ifdef _WIN32
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(tmp, dst) == 0;
#endif
}

/* A key typed at the server console, if any */
static bool KeyPressed(int &key)
{
#ifdef _WIN32
    if (!_kbhit()) return false;
    key = _getch();
    return true;
#else
    if (!isatty(0)) return false;               /* running as a service */
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(0, &rd);
    timeval tv = { 0, 0 };
    if (select(1, &rd, NULL, NULL, &tv) <= 0) return false;
    char c;
    if (read(0, &c, 1) != 1) return false;
    if (c == '\n' || c == '\r') return false;
    key = c;
    return true;
#endif
}

static bool EndsWithTti(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return false;
    const char *e = name + n - 4;
    return (e[0] == '.') && (e[1] == 't' || e[1] == 'T') &&
           (e[2] == 't' || e[2] == 'T') && (e[3] == 'i' || e[3] == 'I');
}

/* All .tti file names in the pages folder (sorted), plus a fingerprint of
 * their sizes and times so we can spot changes. */
static void ListPageFiles(std::vector<std::string> &names, std::string &sig)
{
    names.clear();
    sig.clear();
    std::vector<std::string> entries;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(PAGE_DIR "/*.tti", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!EndsWithTti(fd.cFileName)) continue;
            char buf[64];
            sprintf(buf, "|%lu:%lu:%lu|", fd.nFileSizeLow,
                    fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime);
            names.push_back(fd.cFileName);
            entries.push_back(std::string(fd.cFileName) + buf);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(PAGE_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!EndsWithTti(e->d_name)) continue;
            std::string path = std::string(PAGE_DIR "/") + e->d_name;
            struct stat st;
            if (stat(path.c_str(), &st) != 0) continue;
            char buf[64];
            sprintf(buf, "|%ld:%ld|", (long)st.st_size, (long)st.st_mtime);
            names.push_back(e->d_name);
            entries.push_back(std::string(e->d_name) + buf);
        }
        closedir(d);
    }
#endif
    std::sort(names.begin(), names.end());
    std::sort(entries.begin(), entries.end());
    for (size_t i = 0; i < entries.size(); i++) sig += entries[i];
}

/* ------------------------------------------------------------------ */
/*  TTI file loading / saving                                          */
/* ------------------------------------------------------------------ */

/* Decode the text part of an "OL," line into a 40 byte row.
 * Control codes may be stored as ESC + (code + 0x40), or with bit 7 set. */
static void DecodeOL(const char *text, unsigned char *row)
{
    const unsigned char *p = (const unsigned char *)text;
    int col = 0;
    while (*p && col < TTX_COLS) {
        unsigned char c = *p++;
        if (c == 0x1B) {
            if (!*p) break;
            c = (unsigned char)((*p++ - 0x40) & 0x7F);
        } else {
            c &= 0x7F;
        }
        row[col++] = c;
    }
}

static int ParseLink(const char *s)
{
    int v = (int)strtol(s, NULL, 16);
    if ((v & 0xFF) == 0xFF) return 0;          /* xFF = "no page" */
    if ((v >> 8) == 0) v |= 0x800;             /* magazine 0 is 8 */
    if (v < 0x100 || v > 0x8FF) return 0;
    return v;
}

/* Load one .tti file. If a page also appears in a file loaded earlier,
 * this file's copy replaces it (keep one page per file to avoid this). */
static int LoadTTI(const char *path, const char *fileName)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    char line[1024];
    int  curPage = -1, curSub = -1, cycle = 8, found = 0;
    std::string desc;

    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;

        if (strncmp(line, "DE,", 3) == 0) {
            desc = line + 3;
        }
        else if (strncmp(line, "CT,", 3) == 0) {
            cycle = atoi(line + 3);
            if (cycle < 1) cycle = 8;
            if (curPage >= 0) g_pages[curPage].cycleSecs = cycle;
        }
        else if (strncmp(line, "PN,", 3) == 0) {
            /* PN,mppss  ->  first three hex digits are the page number */
            char digits[4] = { 0, 0, 0, 0 };
            memcpy(digits, line + 3, 3);
            int num = (int)strtol(digits, NULL, 16);
            if ((num >> 8) == 0) num |= 0x800;
            if (num < 0x100 || num > 0x8FF) { curPage = -1; continue; }

            Page &p = g_pages[num];
            if (!p.subs.empty() && p.source != fileName) {
                printf("Note: page %03X is in both %s and %s - using %s\n",
                       num, p.source.c_str(), fileName, fileName);
                Page fresh;
                p = fresh;
            }
            if (p.subs.empty()) {
                p.number = num;
                p.source = fileName;
                p.desc = desc;
                found++;
            }
            p.cycleSecs = cycle;
            SubPage s;
            ClearSub(s);
            p.subs.push_back(s);
            curPage = num;
            curSub  = (int)p.subs.size() - 1;
        }
        else if (strncmp(line, "OL,", 3) == 0 && curPage >= 0) {
            int row = atoi(line + 3);
            const char *t = strchr(line + 3, ',');
            if (t && row >= 1 && row < TTX_ROWS)       /* row 0 = our header */
                DecodeOL(t + 1, g_pages[curPage].subs[curSub].cell[row]);
        }
        else if (strncmp(line, "FL,", 3) == 0 && curPage >= 0) {
            Page &p = g_pages[curPage];
            const char *q = line + 3;
            for (int i = 0; i < 6 && q; i++) {
                p.links[i] = ParseLink(q);
                q = strchr(q, ',');
                if (q) q++;
            }
        }
        /* DS, SP, SC, PS, RE etc. are ignored */
    }
    fclose(f);
    return found;
}

static void EncodeRow(const unsigned char *row, std::string &out)
{
    int len = TTX_COLS;
    while (len > 0 && row[len - 1] == ' ') len--;
    for (int i = 0; i < len; i++) {
        unsigned char c = row[i] & 0x7F;
        if (c < 0x20) { out += (char)0x1B; out += (char)(c + 0x40); }
        else          out += (char)c;
    }
}

/* A page as .tti text (used for saving and for sending to ttxedit) */
static std::string MakeTTI(const Page &p)
{
    std::string s;
    char buf[96];
    s += "DE," + p.desc + "\r\n";
    sprintf(buf, "CT,%d,T\r\n", p.cycleSecs);
    s += buf;
    bool multi = p.subs.size() > 1;
    for (size_t sp = 0; sp < p.subs.size(); sp++) {
        sprintf(buf, "PN,%03X%02d\r\nSC,%04d\r\nPS,8000\r\n",
                p.number, multi ? (int)sp + 1 : 0, multi ? (int)sp + 1 : 0);
        s += buf;
        for (int r = 1; r < TTX_ROWS; r++) {
            std::string t;
            EncodeRow(p.subs[sp].cell[r], t);
            if (t.empty()) continue;
            sprintf(buf, "OL,%d,", r);
            s += buf;
            s += t;
            s += "\r\n";
        }
    }
    int l[6];
    for (int i = 0; i < 6; i++) l[i] = p.links[i] ? p.links[i] : 0x8FF;
    sprintf(buf, "FL,%03X,%03X,%03X,%03X,%03X,%03X\r\n", l[0], l[1], l[2], l[3], l[4], l[5]);
    s += buf;
    return s;
}

/* Write a file safely: write a temporary file, then swap it in */
static bool WriteFileAtomic(const std::string &path, const std::string &data)
{
    std::string tmp = std::string(PAGE_DIR "/~upload.tmp");
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    size_t n = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    if (n != data.size() || !ReplaceWith(tmp.c_str(), path.c_str())) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

static void SaveTTI(const Page &p)
{
    char path[64];
    sprintf(path, PAGE_DIR "/P%03X.tti", p.number);
    WriteFileAtomic(path, MakeTTI(p));
}

/* ------------------------------------------------------------------ */
/*  Demo pages (created when the pages folder is empty)                */
/* ------------------------------------------------------------------ */

static void Put(SubPage &s, int row, int col, const char *text)
{
    while (*text && col < TTX_COLS)
        s.cell[row][col++] = (unsigned char)(*text++ & 0x7F);
}

static void PutByte(SubPage &s, int row, int col, unsigned char c)
{
    if (row >= 0 && row < TTX_ROWS && col >= 0 && col < TTX_COLS)
        s.cell[row][col] = c & 0x7F;
}

static void Title(SubPage &s, const char *text)
{
    Put(s, 1, 0, A_BLUE NEW_BG DOUBLE_HEIGHT A_YELLOW);
    Put(s, 1, 4, text);
}

static void SetLinks(Page &p, int r, int g, int y, int c)
{
    p.links[0] = r; p.links[1] = g; p.links[2] = y; p.links[3] = c;
    p.links[5] = 0x100;
}

/* Draw a filled circle with mosaic graphics. Each cell is 2x3 "sixels". */
static void MosaicCircle(SubPage &s, int row0, int row1, int col0, int col1,
                         double cx, double cy, double rx, double ry)
{
    static const unsigned char bit[3][2] = { {0x01,0x02}, {0x04,0x08}, {0x10,0x40} };
    for (int r = row0; r <= row1; r++)
        for (int c = col0; c <= col1; c++) {
            unsigned char ch = 0x20;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 2; sx++) {
                    double px = (c - col0) * 2 + sx + 0.5;
                    double py = (r - row0) * 3 + sy + 0.5;
                    double dx = (px - cx) / rx, dy = (py - cy) / ry;
                    if (dx * dx + dy * dy <= 1.0) ch |= bit[sy][sx];
                }
            PutByte(s, r, c, ch);
        }
}

static void CreateDemoPages()
{
    /* ---------- 100 Index ---------- */
    {
        Page p; p.number = 0x100;
        SubPage s; ClearSub(s);
        Title(s, "TELETEXT  SERVER");
        Put(s, 3, 0, A_CYAN "Welcome to your own teletext service");
        Put(s, 5, 0, G_BLUE ",,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,");
        Put(s, 7, 0,  A_WHITE "Colours, flash and conceal" A_YELLOW);  Put(s, 7, 36, "101");
        Put(s, 9, 0,  A_WHITE "Mosaic graphics"           A_YELLOW);  Put(s, 9, 36, "102");
        Put(s, 11, 0, A_WHITE "Rotating pages (carousel)" A_YELLOW);  Put(s, 11, 36, "103");
        Put(s, 13, 0, A_WHITE "Adding your own pages"     A_YELLOW);  Put(s, 13, 36, "104");
        Put(s, 15, 0, A_WHITE "Keys for the client"       A_YELLOW);  Put(s, 15, 36, "105");
        Put(s, 19, 0, G_BLUE ",,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,");
        Put(s, 21, 0, A_GREEN "Type a page number, click one, or use");
        Put(s, 22, 0, A_GREEN "the coloured keys F1 F2 F3 F4");
        Put(s, 24, 0, A_RED "Colours  " A_GREEN "Graphics " A_YELLOW "Carousel  " A_CYAN "Help");
        p.subs.push_back(s);
        SetLinks(p, 0x101, 0x102, 0x103, 0x104);
        g_pages[p.number] = p;
    }

    /* ---------- 101 Colours ---------- */
    {
        Page p; p.number = 0x101;
        SubPage s; ClearSub(s);
        Title(s, "Colours and effects");
        Put(s, 4, 0,  A_RED     "Red");     Put(s, 4, 18,  A_RED     NEW_BG A_WHITE  " White on red");
        Put(s, 5, 0,  A_GREEN   "Green");   Put(s, 5, 18,  A_GREEN   NEW_BG A_BLUE   " Blue on green");
        Put(s, 6, 0,  A_YELLOW  "Yellow");  Put(s, 6, 18,  A_YELLOW  NEW_BG A_RED    " Red on yellow");
        Put(s, 7, 0,  A_BLUE    "Blue");    Put(s, 7, 18,  A_BLUE    NEW_BG A_YELLOW " Yellow on blue");
        Put(s, 8, 0,  A_MAGENTA "Magenta"); Put(s, 8, 18,  A_MAGENTA NEW_BG A_WHITE  " White on magenta");
        Put(s, 9, 0,  A_CYAN    "Cyan");    Put(s, 9, 18,  A_CYAN    NEW_BG A_BLUE   " Blue on cyan");
        Put(s, 10, 0, A_WHITE   "White");   Put(s, 10, 18, A_WHITE   NEW_BG A_BLUE   " Blue on white");
        Put(s, 12, 0, A_WHITE "This " FLASH A_YELLOW "flashes" STEADY A_WHITE "and this does not");
        Put(s, 14, 0, DOUBLE_HEIGHT A_CYAN "Double height text");
        Put(s, 17, 0, A_WHITE "Quiz: when did BBC CEEFAX begin?");
        Put(s, 18, 0, A_WHITE "Answer:" A_GREEN CONCEAL "1974");
        Put(s, 19, 0, A_YELLOW "Press R to reveal the answer");
        Put(s, 21, 0, A_WHITE "Specials: # [ \\ ] ^ _ ` { | } ~");
        Put(s, 24, 0, A_RED "Index    " A_GREEN "Graphics " A_YELLOW "Carousel  " A_CYAN "Help");
        p.subs.push_back(s);
        SetLinks(p, 0x100, 0x102, 0x103, 0x104);
        g_pages[p.number] = p;
    }

    /* ---------- 102 Graphics ---------- */
    {
        Page p; p.number = 0x102;
        SubPage s; ClearSub(s);
        Title(s, "Mosaic graphics");
        Put(s, 3, 0, A_WHITE "All 64 mosaics, contiguous:");
        PutByte(s, 4, 1, 0x17); PutByte(s, 5, 1, 0x17);
        for (int i = 0; i < 32; i++) {
            PutByte(s, 4, 3 + i, (unsigned char)(0x20 + i));
            PutByte(s, 5, 3 + i, (unsigned char)(0x60 + i));
        }
        Put(s, 6, 0, A_WHITE "...and separated:");
        PutByte(s, 7, 0, 0x17); PutByte(s, 7, 1, 0x1A);
        PutByte(s, 8, 0, 0x17); PutByte(s, 8, 1, 0x1A);
        for (int i = 0; i < 32; i++) {
            PutByte(s, 7, 3 + i, (unsigned char)(0x20 + i));
            PutByte(s, 8, 3 + i, (unsigned char)(0x60 + i));
        }
        /* colour bars */
        for (int r = 10; r <= 11; r++)
            for (int c = 0; c < 7; c++) {
                PutByte(s, r, c * 5 + 1, (unsigned char)(0x11 + c));
                for (int k = 1; k <= 4; k++) PutByte(s, r, c * 5 + 1 + k, 0x7F);
            }
        Put(s, 13, 0, A_WHITE "Without and with hold graphics:");
        Put(s, 14, 0, G_RED "\x7F\x7F" G_YELLOW "\x7F\x7F" G_GREEN "\x7F\x7F" G_CYAN "\x7F\x7F");
        Put(s, 14, 20, HOLD_GFX G_RED "\x7F\x7F" G_YELLOW "\x7F\x7F" G_GREEN "\x7F\x7F" G_CYAN "\x7F\x7F");
        /* a sun on a blue sky */
        for (int r = 16; r <= 22; r++) {
            Put(s, r, 6, G_BLUE NEW_BG G_YELLOW);
            Put(s, r, 31, BLACK_BG);
        }
        MosaicCircle(s, 16, 22, 9, 30, 22.0, 10.5, 8.5, 9.5);
        Put(s, 24, 0, A_RED "Index    " A_GREEN "Colours  " A_YELLOW "Carousel  " A_CYAN "Help");
        p.subs.push_back(s);
        SetLinks(p, 0x100, 0x101, 0x103, 0x104);
        g_pages[p.number] = p;
    }

    /* ---------- 103 Carousel (3 subpages) ---------- */
    {
        Page p; p.number = 0x103; p.cycleSecs = 6;
        const char *facts[3][3] = {
            { "CEEFAX, the BBC teletext service,",  "began in 1974 and closed in 2012.",  "" },
            { "A teletext page is 40 characters",   "wide and 24 rows deep, plus the",    "header row at the top." },
            { "Pages were sent in spare lines of",  "the TV picture, in the vertical",    "blanking interval." }
        };
        for (int n = 0; n < 3; n++) {
            SubPage s; ClearSub(s);
            Title(s, "Did you know?");
            char buf[48];
            sprintf(buf, A_WHITE "Page %d of 3 - changes every 6 secs", n + 1);
            Put(s, 4, 0, buf);
            for (int k = 0; k < 3; k++) {
                Put(s, 8 + k * 2, 0, A_CYAN);
                Put(s, 8 + k * 2, 1, facts[n][k]);
            }
            Put(s, 20, 0, A_YELLOW "Left/Right arrows step subpages.");
            Put(s, 21, 0, A_YELLOW "H holds the page on screen.");
            Put(s, 24, 0, A_RED "Index    " A_GREEN "Colours  " A_YELLOW "Graphics  " A_CYAN "Help");
            p.subs.push_back(s);
        }
        SetLinks(p, 0x100, 0x101, 0x102, 0x104);
        g_pages[p.number] = p;
    }

    /* ---------- 104 Help: adding pages ---------- */
    {
        Page p; p.number = 0x104;
        SubPage s; ClearSub(s);
        Title(s, "Adding your own pages");
        Put(s, 4, 0,  A_WHITE "Put .tti files in the server's");
        Put(s, 5, 0,  A_WHITE "pages folder, then press R in the");
        Put(s, 6, 0,  A_WHITE "server window to reload them.");
        Put(s, 8, 0,  A_CYAN  "PN,20000      page 200");
        Put(s, 9, 0,  A_CYAN  "PN,20001      next subpage of 200");
        Put(s, 10, 0, A_CYAN  "OL,5,Hello    text on row 5");
        Put(s, 11, 0, A_CYAN  "CT,8,T        8 secs per subpage");
        Put(s, 12, 0, A_CYAN  "FL,100,...    fastext links");
        Put(s, 14, 0, A_WHITE "Control codes are written as ESC");
        Put(s, 15, 0, A_WHITE "followed by (code + 40 hex).");
        Put(s, 17, 0, A_WHITE "Easiest: use a teletext editor");
        Put(s, 18, 0, A_WHITE "that saves .tti files, or edit the");
        Put(s, 19, 0, A_WHITE "demo files the server created.");
        Put(s, 24, 0, A_RED "Index    " A_GREEN "Colours  " A_YELLOW "Graphics  " A_CYAN "Keys");
        p.subs.push_back(s);
        SetLinks(p, 0x100, 0x101, 0x102, 0x105);
        g_pages[p.number] = p;
    }

    /* ---------- 105 Client keys ---------- */
    {
        Page p; p.number = 0x105;
        SubPage s; ClearSub(s);
        Title(s, "Keys for the client");
        Put(s, 4, 0,  A_YELLOW "0-9      " A_WHITE "type a page number");
        Put(s, 5, 0,  A_YELLOW "Up/Down  " A_WHITE "next / previous page");
        Put(s, 6, 0,  A_YELLOW "Lft/Rgt  " A_WHITE "previous / next subpage");
        Put(s, 7, 0,  A_YELLOW "F1-F4    " A_WHITE "red green yellow cyan");
        Put(s, 8, 0,  A_YELLOW "F5 or I  " A_WHITE "index page");
        Put(s, 9, 0,  A_YELLOW "R        " A_WHITE "reveal concealed text");
        Put(s, 10, 0, A_YELLOW "H        " A_WHITE "hold (stop page changing)");
        Put(s, 11, 0, A_YELLOW "Esc      " A_WHITE "cancel page entry");
        Put(s, 12, 0, A_YELLOW "Mouse    " A_WHITE "click a page number or");
        Put(s, 13, 0, A_YELLOW "         " A_WHITE "a coloured fastext link");
        Put(s, 24, 0, A_RED "Index    " A_GREEN "Colours  " A_YELLOW "Graphics  " A_CYAN "Help");
        p.subs.push_back(s);
        SetLinks(p, 0x100, 0x101, 0x102, 0x104);
        g_pages[p.number] = p;
    }
}

/* ------------------------------------------------------------------ */
/*  Sending to clients                                                 */
/* ------------------------------------------------------------------ */

static void MakeHeader(int pageNum, unsigned char *row)
{
    static const char *days[]   = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *months[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec" };
    memset(row, ' ', TTX_COLS);
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[64];
    /* 32 characters for columns 8-39. Columns 0-7 are filled in by the client. */
    sprintf(buf, "%-8.8s %03X %s %02d %s" A_YELLOW "%02d:%02d/%02d",
            g_serviceName.c_str(), pageNum, days[tm->tm_wday], tm->tm_mday,
            months[tm->tm_mon], tm->tm_hour, tm->tm_min, tm->tm_sec);
    size_t n = strlen(buf);
    if (n > 32) n = 32;
    memcpy(row + 8, buf, n);
}

static bool SendAll(Client &c, const char *data, int len)
{
    if (c.dead) return false;
    while (len > 0) {
        int n = send(c.sock, data, len, SEND_FLAGS);
        if (n == SOCKET_ERROR || n == 0) { c.dead = true; return false; }
        data += n; len -= n;
    }
    return true;
}

static void SendText(Client &c, const std::string &s)
{
    SendAll(c, s.data(), (int)s.size());
}

static void SendPage(Client &c)
{
    std::map<int, Page>::iterator it = g_pages.find(c.page);
    if (it == g_pages.end()) {
        char msg[32];
        sprintf(msg, "NOPAGE %03X\n", c.page);
        SendText(c, msg);
        return;
    }
    Page &p = it->second;
    int si = p.current;
    if (c.fixedSub >= 0 && c.fixedSub < (int)p.subs.size()) si = c.fixedSub;

    char head[96];
    sprintf(head, "PAGE %03X %d %d %03X %03X %03X %03X %03X\n",
            p.number, si + 1, (int)p.subs.size(),
            p.links[0], p.links[1], p.links[2], p.links[3],
            p.links[5] ? p.links[5] : 0x100);

    std::string frame(head);
    unsigned char row0[TTX_COLS];
    MakeHeader(p.number, row0);
    frame.append((const char *)row0, TTX_COLS);
    for (int r = 1; r < TTX_ROWS; r++)
        frame.append((const char *)p.subs[si].cell[r], TTX_COLS);

    SendText(c, frame);
}

static void SendHeaderToAll()
{
    for (size_t i = 0; i < g_clients.size(); i++) {
        Client &c = g_clients[i];
        if (!c.page || c.dead) continue;
        std::string frame("HDR\n");
        unsigned char row0[TTX_COLS];
        MakeHeader(c.page, row0);
        frame.append((const char *)row0, TTX_COLS);
        SendText(c, frame);
    }
}

static void DoCarousels(time_t now)
{
    std::map<int, Page>::iterator it;
    for (it = g_pages.begin(); it != g_pages.end(); ++it) {
        Page &p = it->second;
        if (p.subs.size() < 2) continue;
        if (now - p.lastCycle < p.cycleSecs) continue;
        p.current = (p.current + 1) % (int)p.subs.size();
        p.lastCycle = now;
        for (size_t i = 0; i < g_clients.size(); i++)
            if (g_clients[i].page == p.number && g_clients[i].fixedSub < 0)
                SendPage(g_clients[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Page loading                                                       */
/* ------------------------------------------------------------------ */

static void LoadAllPages()
{
    g_pages.clear();
    MakeDir(PAGE_DIR);

    std::vector<std::string> names;
    ListPageFiles(names, g_signature);
    for (size_t i = 0; i < names.size(); i++) {
        std::string path = std::string(PAGE_DIR "/") + names[i];
        LoadTTI(path.c_str(), names[i].c_str());
    }

    if (g_pages.empty()) {
        printf("No pages found - writing demo pages to the %s folder\n", PAGE_DIR);
        CreateDemoPages();
        std::map<int, Page>::iterator it;
        for (it = g_pages.begin(); it != g_pages.end(); ++it) {
            char name[32];
            sprintf(name, "P%03X.tti", it->first);
            it->second.source = name;
            it->second.desc = "Demo page created by ttxserver";
            SaveTTI(it->second);
        }
        ListPageFiles(names, g_signature);
    }

    time_t now = time(NULL);
    std::map<int, Page>::iterator it;
    for (it = g_pages.begin(); it != g_pages.end(); ++it) {
        it->second.current = 0;
        it->second.lastCycle = now;
    }
    printf("Loaded %d page(s) from %d file(s)\n", (int)g_pages.size(), (int)names.size());

    /* refresh anyone already watching */
    for (size_t i = 0; i < g_clients.size(); i++)
        if (g_clients[i].page) SendPage(g_clients[i]);
}

/* ------------------------------------------------------------------ */
/*  Commands from clients                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Newsflash: ttxfeed writes pages/newsflash.txt ("ppp headline")     */
/*  while there is breaking news; we pass it on to every viewer.       */
/* ------------------------------------------------------------------ */

static std::string ReadFlash()
{
    FILE *f = fopen(PAGE_DIR "/newsflash.txt", "rb");
    if (!f) return "";
    char line[512] = "";
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    std::string s;
    for (char *p = line; *p && *p != '\n' && *p != '\r'; p++)
        s += (*p >= 0x20 && *p < 0x7F) ? *p : ' ';
    int page = 0;
    if (sscanf(s.c_str(), "%x", &page) != 1 || page < 0x100 || page > 0x8FF) return "";
    return s;
}

static void SendFlash(Client &c)
{
    SendText(c, g_flash.empty() ? std::string("FLASH\n") : "FLASH " + g_flash + "\n");
}

static void CheckFlash()
{
    std::string f = ReadFlash();
    if (f == g_flash) return;
    g_flash = f;
    if (f.empty()) printf("Newsflash cleared\n");
    else           printf("NEWSFLASH: %s\n", f.c_str());
    for (size_t i = 0; i < g_clients.size(); i++)
        if (g_clients[i].page) SendFlash(g_clients[i]);
}

/* ------------------------------------------------------------------ */
/*  Web client                                                         */
/*    GET /                    -> web/index.html                       */
/*    GET /<file>              -> web/<file>  (manifest, icons)        */
/*    GET /api/page?p=100&s=0  -> the page as JSON (s = fixed subpage) */
/* ------------------------------------------------------------------ */

static void HttpReply(HttpConn &h, const char *status, const char *type, const std::string &body)
{
    char head[256];
    sprintf(head, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
            status, type, (int)body.size());
    std::string all = std::string(head) + body;
    const char *data = all.data();
    int len = (int)all.size();
    while (len > 0) {
        int n = send(h.sock, data, len, SEND_FLAGS);
        if (n <= 0) break;
        data += n; len -= n;
    }
    h.dead = true;
}

static std::string Hex(const unsigned char *p, int n)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < n; i++) { s += d[(p[i] >> 4) & 15]; s += d[p[i] & 15]; }
    return s;
}

static std::string JsonText(const std::string &t)
{
    std::string r = "\"";
    for (size_t i = 0; i < t.size(); i++) {
        char c = t[i];
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) r += c;
    }
    return r + "\"";
}

static std::string QueryValue(const std::string &q, const char *name)
{
    std::string key = std::string(name) + "=";
    size_t p = 0;
    while (p < q.size()) {
        size_t e = q.find('&', p);
        if (e == std::string::npos) e = q.size();
        if (q.compare(p, key.size(), key) == 0) return q.substr(p + key.size(), e - p - key.size());
        p = e + 1;
    }
    return "";
}

static std::string PageJson(int pageNum, int fixedSub)
{
    char b[96];
    std::string j = "{";
    unsigned char row0[TTX_COLS];
    MakeHeader(pageNum, row0);
    std::map<int, Page>::iterator it = g_pages.find(pageNum);
    if (it == g_pages.end()) {
        sprintf(b, "\"found\":false,\"page\":\"%03X\",", pageNum & 0xFFF);
        j += b;
        j += "\"header\":\"" + Hex(row0, TTX_COLS) + "\"";
    } else {
        Page &p = it->second;
        int si = p.current;
        if (fixedSub > 0 && fixedSub <= (int)p.subs.size()) si = fixedSub - 1;
        sprintf(b, "\"found\":true,\"page\":\"%03X\",\"sub\":%d,\"count\":%d,",
                p.number, si + 1, (int)p.subs.size());
        j += b;
        sprintf(b, "\"links\":[\"%03X\",\"%03X\",\"%03X\",\"%03X\",\"%03X\"],",
                p.links[0], p.links[1], p.links[2], p.links[3], p.links[5] ? p.links[5] : 0x100);
        j += b;
        std::string rows = Hex(row0, TTX_COLS);
        for (int r = 1; r < TTX_ROWS; r++) rows += Hex(p.subs[si].cell[r], TTX_COLS);
        j += "\"rows\":\"" + rows + "\"";
    }
    if (!g_flash.empty() && g_flash.size() > 4) {
        j += ",\"flash\":{\"page\":\"" + g_flash.substr(0, 3) + "\",\"text\":" +
             JsonText(g_flash.substr(4)) + "}";
    }
    return j + "}";
}

static const char *ContentType(const std::string &name)
{
    size_t dot = name.rfind('.');
    std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "js")   return "text/javascript";
    if (ext == "css")  return "text/css";
    if (ext == "json" || ext == "webmanifest") return "application/manifest+json";
    if (ext == "png")  return "image/png";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "ico")  return "image/x-icon";
    return "application/octet-stream";
}

/* Save or delete a page file. Returns "" when it worked, or the reason it didn't.
 * Used by ttxedit (TCP) and by the web editor (HTTP). */
static std::string SavePageData(int page, const std::string &data, const char *who)
{
    if (page < 0x100 || page > 0x8FF) return "page must be 100-8FF";
    if (data.find("PN,") == std::string::npos) return "that is not a .tti page";
    char path[64];
    sprintf(path, PAGE_DIR "/P%03X.tti", page);
    if (!WriteFileAtomic(path, data)) return "server could not write the file";
    printf("[%s] saved page %03X\n", who, page);
    LoadAllPages();
    char msg[64];
    if (g_pages.find(page) == g_pages.end()) {
        sprintf(msg, "saved, but page %03X was not in the data", page);
        return msg;
    }
    return "";
}

static std::string DeletePageFile(int page, const char *who)
{
    char path[64];
    sprintf(path, PAGE_DIR "/P%03X.tti", page);
    if (remove(path) != 0) {
        char msg[64];
        sprintf(msg, "no file P%03X.tti on the server", page);
        return msg;
    }
    printf("[%s] deleted page %03X\n", who, page);
    LoadAllPages();
    return "";
}

/* value of an HTTP header (name in lower case), "" if absent */
static std::string HeaderValue(const std::string &req, const char *name)
{
    size_t end = req.find("\r\n\r\n");
    std::string head = req.substr(0, end);
    std::string low = head;
    for (size_t i = 0; i < low.size(); i++) low[i] = (char)tolower((unsigned char)low[i]);
    std::string key = std::string("\r\n") + name + ":";
    size_t p = low.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    size_t e = head.find("\r\n", p);
    std::string v = head.substr(p, e == std::string::npos ? std::string::npos : e - p);
    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
    while (!v.empty() && v[v.size() - 1] == ' ') v.erase(v.size() - 1);
    return v;
}

/* Is the whole request (headers + body) here yet? */
static bool HttpComplete(const HttpConn &h)
{
    size_t end = h.buf.find("\r\n\r\n");
    if (end == std::string::npos) return false;
    int len = atoi(HeaderValue(h.buf, "content-length").c_str());
    return h.buf.size() >= end + 4 + (size_t)(len > 0 ? len : 0);
}

static std::string JsonResult(const std::string &err)
{
    return err.empty() ? "{\"ok\":true}" : "{\"ok\":false,\"error\":" + JsonText(err) + "}";
}

/* May this web request edit pages? "" = yes, otherwise why not */
static std::string WebEditCheck(HttpConn &h)
{
    if (g_password.empty())
        return "Web editing needs a password: set password= in ttxserver.cfg";
    if (!g_webEditRemote && !HeaderValue(h.buf, "cf-connecting-ip").empty())
        return "Editing over the internet is turned off (webeditremote=0)";
    if (time(NULL) < g_lockUntil)
        return "Too many wrong passwords - wait a minute and try again";
    if (HeaderValue(h.buf, "x-password") != g_password) {
        if (++g_badTries >= 5) { g_lockUntil = time(NULL) + 60; g_badTries = 0; }
        printf("[%s] wrong web edit password\n", h.addr);
        return "Wrong password";
    }
    g_badTries = 0;
    return "";
}

static void HandleHttp(HttpConn &h)
{
    size_t sp1 = h.buf.find(' '), sp2 = h.buf.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) { h.dead = true; return; }
    std::string method = h.buf.substr(0, sp1);
    std::string target = h.buf.substr(sp1 + 1, sp2 - sp1 - 1);
    if (method != "GET" && method != "POST") {
        HttpReply(h, "405 Method Not Allowed", "text/plain", "GET or POST only\n");
        return;
    }

    std::string path = target, query;
    size_t q = target.find('?');
    if (q != std::string::npos) { path = target.substr(0, q); query = target.substr(q + 1); }
    int qpage = (int)strtol(QueryValue(query, "p").c_str(), NULL, 16);

    /* ---- web editor ---- */
    if (path == "/api/list") {
        std::string j = "{\"pages\":[";
        char b[16];
        std::map<int, Page>::iterator it;
        for (it = g_pages.begin(); it != g_pages.end(); ++it) {
            sprintf(b, "%s\"%03X\"", it == g_pages.begin() ? "" : ",", it->first);
            j += b;
        }
        HttpReply(h, "200 OK", "application/json", j + "]}");
        return;
    }
    if (path == "/api/tti") {
        std::map<int, Page>::iterator it = g_pages.find(qpage);
        if (it == g_pages.end()) HttpReply(h, "404 Not Found", "text/plain", "No such page\n");
        else HttpReply(h, "200 OK", "text/plain; charset=us-ascii", MakeTTI(it->second));
        return;
    }
    if (method == "POST" && (path == "/api/auth" || path == "/api/save" || path == "/api/delete")) {
        std::string err = WebEditCheck(h);
        if (err.empty() && path == "/api/save") {
            size_t end = h.buf.find("\r\n\r\n");
            err = SavePageData(qpage, h.buf.substr(end + 4), h.addr);
        } else if (err.empty() && path == "/api/delete") {
            err = DeletePageFile(qpage, h.addr);
        }
        HttpReply(h, "200 OK", "application/json", JsonResult(err));
        return;
    }
    if (method != "GET") { HttpReply(h, "404 Not Found", "text/plain", "Not found\n"); return; }

    if (path == "/api/page") {
        int page = (int)strtol(QueryValue(query, "p").c_str(), NULL, 16);
        int sub = atoi(QueryValue(query, "s").c_str());
        if (page < 0x100 || page > 0x8FF) page = 0x100;
        HttpReply(h, "200 OK", "application/json", PageJson(page, sub));
        return;
    }

    /* static files from the web folder - plain names only, nothing outside it */
    std::string name = path == "/" ? "index.html" : path.substr(1);
    bool ok = !name.empty() && name[0] != '.';
    for (size_t i = 0; i < name.size() && ok; i++) {
        char c = name[i];
        ok = isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_';
    }
    if (ok) {
        std::string file = g_webDir + "/" + name;
        FILE *f = fopen(file.c_str(), "rb");
        if (f) {
            std::string body;
            char b[8192];
            size_t n;
            while ((n = fread(b, 1, sizeof(b), f)) > 0) body.append(b, n);
            fclose(f);
            HttpReply(h, "200 OK", ContentType(name), body);
            return;
        }
    }
    HttpReply(h, "404 Not Found", "text/plain", "Not found\n");
}

static bool MayEdit(Client &c)
{
    return g_password.empty() || c.authed;
}

static void HandlePut(Client &c, const std::string &data)
{
    char msg[64];
    if (!c.putAllowed) {
        SendText(c, "ERR password needed - set password= in ttxedit.cfg\n");
        return;
    }
    std::string err = SavePageData(c.putPage, data, c.addr);
    if (err.empty()) sprintf(msg, "OK %03X\n", c.putPage);
    else             sprintf(msg, "ERR %.58s\n", err.c_str());
    SendText(c, msg);
}

static void HandleLine(Client &c, const std::string &line)
{
    char msg[128];
    int page = 0, sub = 0, len = 0;

    if (line.compare(0, 4, "GET ") == 0) {
        int n = sscanf(line.c_str(), "GET %x %d", &page, &sub);
        if (n < 1) return;
        if (page < 0x100 || page > 0x8FF) {
            sprintf(msg, "NOPAGE %03X\n", page & 0xFFF);
            SendText(c, msg);
            return;
        }
        bool first = (c.page == 0);
        c.page     = page;
        c.fixedSub = (n == 2 && sub > 0) ? sub - 1 : -1;
        SendPage(c);
        if (first && !g_flash.empty()) SendFlash(c);     /* newcomer: show current newsflash */
        printf("[%s] page %03X%s\n", c.addr, page, n == 2 ? " (subpage)" : "");
    }
    else if (line == "LIST") {
        std::string s = "PAGES";
        std::map<int, Page>::iterator it;
        for (it = g_pages.begin(); it != g_pages.end(); ++it) {
            sprintf(msg, " %03X", it->first);
            s += msg;
        }
        SendText(c, s + "\n");
    }
    else if (sscanf(line.c_str(), "GETTTI %x", &page) == 1) {
        std::map<int, Page>::iterator it = g_pages.find(page);
        if (it == g_pages.end()) { SendText(c, "ERR no such page\n"); return; }
        std::string t = MakeTTI(it->second);
        sprintf(msg, "TTI %03X %d\n", page, (int)t.size());
        SendText(c, msg + t);
        printf("[%s] opened page %03X for editing\n", c.addr, page);
    }
    else if (line.compare(0, 5, "AUTH ") == 0) {
        c.authed = !g_password.empty() && line.substr(5) == g_password;
        if (g_password.empty() || c.authed) SendText(c, "OK\n");
        else {
            SendText(c, "ERR wrong password\n");
            printf("[%s] wrong edit password\n", c.addr);
        }
    }
    else if (sscanf(line.c_str(), "PUTTTI %x %d", &page, &len) == 2) {
        if (len <= 0 || len > MAX_PUT || page < 0x100 || page > 0x8FF) { c.dead = true; return; }
        c.putPage = page;
        c.putLen = len;                     /* the data follows - see main loop */
        c.putAllowed = MayEdit(c);
    }
    else if (sscanf(line.c_str(), "DELETE %x", &page) == 1) {
        if (!MayEdit(c)) { SendText(c, "ERR password needed\n"); return; }
        std::string err = DeletePageFile(page, c.addr);
        if (err.empty()) sprintf(msg, "OK %03X\n", page);
        else             sprintf(msg, "ERR %.58s\n", err.c_str());
        SendText(c, msg);
    }
    else if (line == "QUIT") {
        c.dead = true;
    }
}

/* Work through everything a client has sent so far */
static void ProcessInput(Client &c)
{
    for (;;) {
        if (c.putLen > 0) {                          /* waiting for PUTTTI data */
            if ((int)c.inbuf.size() < c.putLen) return;
            std::string data = c.inbuf.substr(0, c.putLen);
            c.inbuf.erase(0, c.putLen);
            c.putLen = 0;
            HandlePut(c, data);
            continue;
        }
        size_t nl = c.inbuf.find('\n');
        if (nl == std::string::npos) {
            if (c.inbuf.size() > 1024) c.dead = true;   /* junk - drop it */
            return;
        }
        std::string line = c.inbuf.substr(0, nl);
        c.inbuf.erase(0, nl + 1);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        HandleLine(c, line);
        if (c.dead) return;
    }
}

/* ------------------------------------------------------------------ */
/*  Start-up                                                           */
/* ------------------------------------------------------------------ */

static void LoadConfig(int &port)
{
    FILE *f = fopen("ttxserver.cfg", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        if (!strcmp(line, "port"))          port = atoi(eq + 1);
        else if (!strcmp(line, "name"))     g_serviceName = eq + 1;
        else if (!strcmp(line, "password")) g_password = eq + 1;
        else if (!strcmp(line, "webport"))  g_webPort = atoi(eq + 1);
        else if (!strcmp(line, "webdir"))   g_webDir = eq + 1;
        else if (!strcmp(line, "webeditremote")) g_webEditRemote = atoi(eq + 1) != 0;
    }
    fclose(f);
}

static void ShowAddresses(int port)
{
    printf("Clients can connect to port %d on:\n", port);
    printf("   127.0.0.1  (this computer)\n");
    char name[256];
    if (gethostname(name, sizeof(name)) != 0) return;
    hostent *he = gethostbyname(name);
    if (!he) return;
    for (int i = 0; he->h_addr_list[i]; i++) {
        in_addr a;
        memcpy(&a, he->h_addr_list[i], sizeof(a));
        if (strncmp(inet_ntoa(a), "127.", 4) != 0)
            printf("   %s\n", inet_ntoa(a));
    }
#ifndef _WIN32
    printf("   (or run 'hostname -I' to see this machine's addresses)\n");
#endif
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    LoadConfig(port);
    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) g_serviceName = argv[2];
    setvbuf(stdout, NULL, _IOLBF, 1024);      /* log lines appear promptly */

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        printf("socket() failed\n");
        return 1;
    }
#ifndef _WIN32
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#endif

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((unsigned short)port);

    if (bind(listener, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("Cannot use port %d - is another server already running?\n", port);
        return 1;
    }
    listen(listener, SOMAXCONN);

    /* second port for browsers */
    SOCKET web = INVALID_SOCKET;
    if (g_webPort > 0) {
        web = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifndef _WIN32
        setsockopt(web, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#endif
        addr.sin_port = htons((unsigned short)g_webPort);
        if (web == INVALID_SOCKET || bind(web, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
            printf("Cannot use web port %d - browser client is off\n", g_webPort);
            if (web != INVALID_SOCKET) closesocket(web);
            web = INVALID_SOCKET;
        } else {
            listen(web, SOMAXCONN);
        }
    }

    printf("=== Teletext server '%s' on port %d ===\n", g_serviceName.c_str(), port);
    if (web != INVALID_SOCKET)
        printf("Browser client: http://<this computer's address>:%d\n", g_webPort);
    LoadAllPages();
    g_flash = ReadFlash();
    ShowAddresses(port);
    if (g_password.empty())
        printf("NOTE: no password set - anyone on your network can change pages.\n"
               "      Add password=... to ttxserver.cfg to stop that.\n");
    printf("Keys: R=reload pages  L=list clients  P=list pages  Q=quit\n\n");

    time_t lastSecond = time(NULL);
    bool running = true;

    while (running) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(listener, &rd);
        SOCKET maxfd = listener;
        for (size_t i = 0; i < g_clients.size(); i++) {
            FD_SET(g_clients[i].sock, &rd);
            if (g_clients[i].sock > maxfd) maxfd = g_clients[i].sock;
        }
        if (web != INVALID_SOCKET) {
            FD_SET(web, &rd);
            if (web > maxfd) maxfd = web;
            for (size_t i = 0; i < g_http.size(); i++) {
                FD_SET(g_http[i].sock, &rd);
                if (g_http[i].sock > maxfd) maxfd = g_http[i].sock;
            }
        }

        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;               /* wake 5 times a second */
        int ready = select((int)maxfd + 1, &rd, NULL, NULL, &tv);

        if (ready > 0) {
            /* new connection */
            if (FD_ISSET(listener, &rd)) {
                sockaddr_in ca;
                socklen_t calen = sizeof(ca);
                SOCKET s = accept(listener, (sockaddr *)&ca, &calen);
                if (s != INVALID_SOCKET) {
                    if (g_clients.size() >= MAX_CLIENTS) {
                        closesocket(s);
                    } else {
                        Client c;
                        c.sock = s; c.page = 0; c.fixedSub = -1; c.dead = false;
                        c.authed = false; c.putPage = 0; c.putLen = 0; c.putAllowed = false;
                        sprintf(c.addr, "%s:%d", inet_ntoa(ca.sin_addr), ntohs(ca.sin_port));
                        g_clients.push_back(c);
                        printf("[%s] connected (%d client(s))\n", c.addr, (int)g_clients.size());
                    }
                }
            }
            /* browser requests */
            if (web != INVALID_SOCKET && FD_ISSET(web, &rd)) {
                sockaddr_in wa;
                socklen_t walen = sizeof(wa);
                SOCKET s = accept(web, (sockaddr *)&wa, &walen);
                if (s != INVALID_SOCKET) {
                    if (g_http.size() >= 64) closesocket(s);
                    else {
                        HttpConn h;
                        h.sock = s; h.opened = time(NULL); h.dead = false;
                        sprintf(h.addr, "web %s", inet_ntoa(wa.sin_addr));
                        g_http.push_back(h);
                    }
                }
            }
            for (size_t i = 0; i < g_http.size(); i++) {
                HttpConn &h = g_http[i];
                if (h.dead || !FD_ISSET(h.sock, &rd)) continue;
                char buf[2048];
                int n = recv(h.sock, buf, sizeof(buf), 0);
                if (n <= 0) { h.dead = true; continue; }
                h.buf.append(buf, n);
                if (HttpComplete(h)) HandleHttp(h);
                else if (h.buf.size() > (size_t)MAX_PUT + 8192) h.dead = true;
            }

            /* data from clients */
            for (size_t i = 0; i < g_clients.size(); i++) {
                Client &c = g_clients[i];
                if (c.dead || !FD_ISSET(c.sock, &rd)) continue;
                char buf[4096];
                int n = recv(c.sock, buf, sizeof(buf), 0);
                if (n <= 0) { c.dead = true; continue; }
                c.inbuf.append(buf, n);
                ProcessInput(g_clients[i]);
            }
        }

        /* once a second: rotate carousels and update clocks */
        time_t now = time(NULL);
        if (now != lastSecond) {
            lastSecond = now;
            DoCarousels(now);
            SendHeaderToAll();

            /* every 2 seconds: reload if any page file was added, changed or removed */
            if (now % 2 == 0) {
                std::vector<std::string> names;
                std::string sig;
                ListPageFiles(names, sig);
                if (sig != g_signature) {
                    SleepMs(200);               /* let the writer finish */
                    printf("Pages folder changed - reloading\n");
                    LoadAllPages();
                }
                CheckFlash();
            }
        }

        /* finished or stuck browser connections */
        for (size_t i = g_http.size(); i-- > 0; ) {
            if (g_http[i].dead || time(NULL) - g_http[i].opened > 10) {
                closesocket(g_http[i].sock);
                g_http.erase(g_http.begin() + i);
            }
        }

        /* remove disconnected clients */
        for (size_t i = g_clients.size(); i-- > 0; ) {
            if (g_clients[i].dead) {
                printf("[%s] disconnected\n", g_clients[i].addr);
                closesocket(g_clients[i].sock);
                g_clients.erase(g_clients.begin() + i);
            }
        }

        /* console keys */
        int k;
        while (KeyPressed(k)) {
            switch (k) {
            case 'r': case 'R':
                LoadAllPages();
                break;
            case 'l': case 'L':
                printf("%d client(s):\n", (int)g_clients.size());
                for (size_t i = 0; i < g_clients.size(); i++)
                    printf("   %-22s page %03X\n", g_clients[i].addr, g_clients[i].page);
                break;
            case 'p': case 'P': {
                std::map<int, Page>::iterator it;
                for (it = g_pages.begin(); it != g_pages.end(); ++it)
                    printf("   %03X  %d subpage(s)  %s\n", it->first,
                           (int)it->second.subs.size(), it->second.source.c_str());
                break;
            }
            case 'q': case 'Q':
                running = false;
                break;
            }
        }
    }

    for (size_t i = 0; i < g_clients.size(); i++) closesocket(g_clients[i].sock);
    for (size_t i = 0; i < g_http.size(); i++) closesocket(g_http[i].sock);
    if (web != INVALID_SOCKET) closesocket(web);
    closesocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
