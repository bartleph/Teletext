/*
 * ttxvbi.cpp  -  Broadcast your Teletext pages to a real TV
 * ---------------------------------------------------------
 * Reads the server's pages folder and writes a continuous stream of
 * teletext packets (42 bytes each, the "t42" format) to standard output.
 * Pipe it into raspi-teletext, which puts the packets into the vertical
 * blanking interval of a Raspberry Pi's composite video output. A TV with
 * a teletext decoder then shows your pages when you press Text:
 *
 *     ./ttxvbi -d ../server/pages | sudo teletext -
 *
 * What it does for you, like ttxserver does for its own clients:
 *   - every page and subpage in the folder is sent in turn (.tti files, the
 *     same ones ttxserver, ttxedit, ttxfeed, ttxweather ... write)
 *   - the header carries the service name, page number, date and a live clock
 *   - carousel pages (several subpages) rotate every CT seconds
 *   - Fastext colour links (FL line) are sent, so the TV's coloured buttons work
 *   - the newsflash (pages/newsflash.txt, written by ttxfeed) is put on the
 *     bottom row of every page while there is breaking news
 *   - pages are reloaded by themselves when the files change
 *
 * Usage:  ttxvbi [-d pagesdir] [-n NAME] [-r packets/sec] [-c count] [-p page]
 *   -d  pages folder (default: ../server/pages, or "pages" if that is missing)
 *   -n  service name in the header (default: name= from ../server/ttxserver.cfg,
 *       else TELETEXT)
 *   -r  limit the speed (default: none - the pipe to raspi-teletext sets the pace)
 *   -c  stop after this many packets (for testing)
 *   -p  dump a page as text instead of sending packets, e.g. -p 100. The page is
 *       encoded exactly as it would go on air and then decoded again, so you can
 *       check it (header, rows, Fastext links, newsflash) without any hardware
 * Messages go to standard error, so they never mix with the packets.
 *
 * Build (Linux / Raspberry Pi):   g++ -O2 -o ttxvbi ttxvbi.cpp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

#define ROWS 25
#define COLS 40

struct SubPage { unsigned char cell[ROWS][COLS]; };

struct Page {
    int                  number;
    std::vector<SubPage> subs;
    int                  cycleSecs;
    int                  links[6];
    size_t               current;
    time_t               lastCycle;
    Page() : number(0), cycleSecs(8), current(0), lastCycle(0) { for (int i = 0; i < 6; i++) links[i] = 0; }
};

static std::map<int, Page> g_pages;
static std::string g_dir = "../server/pages";
static std::string g_name = "TELETEXT";
static std::string g_flash;                    /* "ppp headline" or "" */
static long        g_rate = 0;                 /* packets per second, 0 = unlimited */
static long        g_limit = -1;               /* packets left to send, -1 = no limit */
static long        g_sent = 0;
static bool        g_cap = false;              /* -p: collect packets instead of sending */
static std::vector<unsigned char> g_capture;

/* ------------------------------------------------------------------ */
/*  Teletext coding                                                    */
/* ------------------------------------------------------------------ */

static unsigned char g_ham[16];

/* Hamming 8/4: bits are P1 D1 P2 D2 P3 D3 P4 D4 (bit 0 first on the wire) */
static void InitHamming()
{
    for (int n = 0; n < 16; n++) {
        int d1 = n & 1, d2 = (n >> 1) & 1, d3 = (n >> 2) & 1, d4 = (n >> 3) & 1;
        int p1 = 1 ^ d1 ^ d3 ^ d4;
        int p2 = 1 ^ d1 ^ d2 ^ d4;
        int p3 = 1 ^ d1 ^ d2 ^ d3;
        int b = p1 | (d1 << 1) | (p2 << 2) | (d2 << 3) | (p3 << 4) | (d3 << 5) | (d4 << 7);
        int ones = 0;
        for (int i = 0; i < 8; i++) if (b & (1 << i)) ones++;
        if ((ones & 1) == 0) b |= 0x40;                /* P4: whole byte odd parity */
        g_ham[n] = (unsigned char)b;
    }
}

static unsigned char Odd(unsigned char c)              /* 7 bits + odd parity in bit 7 */
{
    c &= 0x7F;
    int ones = 0;
    for (int i = 0; i < 7; i++) if (c & (1 << i)) ones++;
    return (ones & 1) ? c : (unsigned char)(c | 0x80);
}

static void Emit(const unsigned char *pkt)
{
    if (g_cap) { g_capture.insert(g_capture.end(), pkt, pkt + 42); return; }
    if (g_limit == 0) exit(0);
    const unsigned char *p = pkt;
    size_t left = 42;
    while (left) {
        ssize_t n = write(1, p, left);
        if (n < 0) exit(0);                             /* reader went away */
        p += n; left -= (size_t)n;
    }
    g_sent++;
    if (g_limit > 0) g_limit--;
    if (g_rate > 0 && g_sent % 10 == 0) {               /* crude pacing, 10 packets at a time */
        struct timespec ts;
        long ns = 10L * 1000000000L / g_rate;
        ts.tv_sec = ns / 1000000000L; ts.tv_nsec = ns % 1000000000L;
        nanosleep(&ts, NULL);
    }
}

static void Mrag(unsigned char *pkt, int mag, int row)
{
    pkt[0] = g_ham[(mag & 7) | ((row & 1) << 3)];
    pkt[1] = g_ham[(row >> 1) & 15];
}

/* Page header (row 0): erase, serial mode, English character set */
static void SendHeader(int page, int subcode, bool eraseFlag, const unsigned char *row)
{
    unsigned char p[42];
    Mrag(p, page >> 8, 0);
    p[2] = g_ham[page & 15];
    p[3] = g_ham[(page >> 4) & 15];
    p[4] = g_ham[subcode & 15];
    p[5] = g_ham[((subcode >> 4) & 7) | (eraseFlag ? 8 : 0)];          /* S2, C4 erase page */
    p[6] = g_ham[(subcode >> 8) & 15];
    p[7] = g_ham[((subcode >> 12) & 3)];                                /* S4, C5 C6 = 0     */
    p[8] = g_ham[0];                                                    /* C7-C10 = 0        */
    p[9] = g_ham[1];                                                    /* C11 serial; C12-14 English */
    for (int i = 0; i < 32; i++) p[10 + i] = Odd(row[8 + i]);
    Emit(p);
}

static void SendRow(int mag, int rowNo, const unsigned char *row)
{
    unsigned char p[42];
    Mrag(p, mag, rowNo);
    for (int i = 0; i < COLS; i++) p[2 + i] = Odd(row[i]);
    Emit(p);
}

/* Fastext links (packet 27, designation 0): red, green, yellow, cyan, spare, index */
static void SendFastext(int page, const int *links)
{
    unsigned char p[42];
    int mag = (page >> 8) & 7;
    Mrag(p, mag, 27);
    p[2] = g_ham[0];
    for (int i = 0; i < 6; i++) {
        int n = links[i] ? links[i] : (0x100 * (page >> 8)) | 0xFF;   /* xFF = no page */
        int rel = ((n >> 8) ^ mag) & 7;
        unsigned char *q = p + 3 + 6 * i;
        q[0] = g_ham[n & 15];
        q[1] = g_ham[(n >> 4) & 15];
        q[2] = g_ham[0xF];                                              /* subcode 3F7F = any */
        q[3] = g_ham[0x7 | ((rel & 1) << 3)];
        q[4] = g_ham[0xF];
        q[5] = g_ham[0x3 | (((rel >> 1) & 1) << 2) | (((rel >> 2) & 1) << 3)];
    }
    p[39] = g_ham[0xF];                                                 /* row 24 links enabled */
    p[40] = 0; p[41] = 0;                                               /* CRC, unused          */
    Emit(p);
}

/* ------------------------------------------------------------------ */
/*  Header text and newsflash, as ttxserver and the web client do them */
/* ------------------------------------------------------------------ */

static void MakeHeaderRow(int pageNum, unsigned char *row)
{
    static const char *days[]   = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *months[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec" };
    memset(row, ' ', COLS);
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[64];
    snprintf(buf, sizeof(buf), "%-8.8s %03X %s %02d %s\x03%02d:%02d/%02d",
             g_name.c_str(), pageNum, days[tm->tm_wday], tm->tm_mday,
             months[tm->tm_mon], tm->tm_hour, tm->tm_min, tm->tm_sec);
    size_t n = strlen(buf);
    if (n > 32) n = 32;
    memcpy(row + 8, buf, n);
}

static void FlashRow(unsigned char *row)
{
    static const unsigned char head[] = { 0x01, 0x1D, 0x07, 0x08, 'N','E','W','S','F','L','A','S','H',
                                          0x09, 0x04, 0x1D, 0x03 };
    memset(row, ' ', COLS);
    size_t h = sizeof(head);
    memcpy(row, head, h);
    std::string loop = g_flash + "   +++   ";
    size_t tick = (size_t)(time(NULL) % 100000) * 4;                    /* the headline scrolls */
    for (size_t i = h; i < COLS; i++) row[i] = (unsigned char)loop[(tick + i - h) % loop.size()];
}

/* ------------------------------------------------------------------ */
/*  Loading pages (.tti)                                               */
/* ------------------------------------------------------------------ */

static void DecodeOL(const char *text, unsigned char *row)
{
    const unsigned char *p = (const unsigned char *)text;
    int col = 0;
    while (*p && col < COLS) {
        unsigned char c = *p++;
        if (c == 0x1B) { if (!*p) break; c = (unsigned char)((*p++ - 0x40) & 0x7F); }
        else c &= 0x7F;
        row[col++] = c;
    }
}

static int ParseLink(const char *s)
{
    int v = (int)strtol(s, NULL, 16);
    if ((v & 0xFF) == 0xFF) return 0;
    if ((v >> 8) == 0) v |= 0x800;
    if (v < 0x100 || v > 0x8FF) return 0;
    return v;
}

static void LoadTTI(const char *path, std::map<int, Page> &out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[1024];
    int curPage = -1, cycle = 8;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!strncmp(line, "CT,", 3)) {
            cycle = atoi(line + 3);
            if (cycle < 1) cycle = 8;
            if (curPage >= 0) out[curPage].cycleSecs = cycle;
        } else if (!strncmp(line, "PN,", 3) && strlen(line) >= 6) {
            char d[4] = { line[3], line[4], line[5], 0 };
            int num = (int)strtol(d, NULL, 16);
            if ((num >> 8) == 0) num |= 0x800;
            if (num < 0x100 || num > 0x8FF) { curPage = -1; continue; }
            Page &p = out[num];
            p.number = num;
            p.cycleSecs = cycle;
            SubPage s;
            memset(s.cell, ' ', sizeof(s.cell));
            p.subs.push_back(s);
            curPage = num;
        } else if (!strncmp(line, "OL,", 3) && curPage >= 0) {
            int row = atoi(line + 3);
            const char *t = strchr(line + 3, ',');
            if (t && row >= 1 && row < ROWS) DecodeOL(t + 1, out[curPage].subs.back().cell[row]);
        } else if (!strncmp(line, "FL,", 3) && curPage >= 0) {
            const char *q = line + 3;
            for (int i = 0; i < 6 && q; i++) {
                out[curPage].links[i] = ParseLink(q);
                q = strchr(q, ',');
                if (q) q++;
            }
        }
    }
    fclose(f);
}

static bool EndsWithTti(const std::string &s)
{
    return s.size() > 4 && !strcasecmp(s.c_str() + s.size() - 4, ".tti");
}

/* a signature of the folder (names, sizes, times) to spot changes */
static std::string FolderSignature(std::vector<std::string> &names)
{
    names.clear();
    std::string sig;
    DIR *d = opendir(g_dir.c_str());
    if (!d) return sig;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        std::string n = e->d_name;
        if (n[0] == '~' || !EndsWithTti(n)) continue;
        names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i < names.size(); i++) {
        struct stat st;
        if (stat((g_dir + "/" + names[i]).c_str(), &st) != 0) continue;
        char b[96];
        snprintf(b, sizeof(b), "%s:%ld:%ld;", names[i].c_str(), (long)st.st_size, (long)st.st_mtime);
        sig += b;
    }
    return sig;
}

static void ReloadIfChanged()
{
    static std::string last = "\x01";
    std::vector<std::string> names;
    std::string sig = FolderSignature(names);
    if (sig == last) return;
    last = sig;
    std::map<int, Page> fresh;
    for (size_t i = 0; i < names.size(); i++) LoadTTI((g_dir + "/" + names[i]).c_str(), fresh);
    for (std::map<int, Page>::iterator it = fresh.begin(); it != fresh.end(); it++) {
        std::map<int, Page>::iterator old = g_pages.find(it->first);       /* keep the carousel position */
        if (old != g_pages.end()) {
            it->second.current = old->second.current < it->second.subs.size() ? old->second.current : 0;
            it->second.lastCycle = old->second.lastCycle;
        }
    }
    g_pages.swap(fresh);
    fprintf(stderr, "Loaded %d page(s) from %d file(s)\n", (int)g_pages.size(), (int)names.size());
}

static void CheckFlash()
{
    std::string s;
    FILE *f = fopen((g_dir + "/newsflash.txt").c_str(), "rb");
    if (f) {
        char line[512] = "";
        if (!fgets(line, sizeof(line), f)) line[0] = 0;
        fclose(f);
        for (char *p = line; *p && *p != '\n' && *p != '\r'; p++) s += (*p >= 0x20 && *p < 0x7F) ? *p : ' ';
        int page = 0;
        if (sscanf(s.c_str(), "%x", &page) != 1 || page < 0x100 || page > 0x8FF) s.clear();
    }
    if (s == g_flash) return;
    g_flash = s;
    if (s.empty()) fprintf(stderr, "Newsflash cleared\n");
    else           fprintf(stderr, "NEWSFLASH: %s\n", s.c_str());
}

/* ------------------------------------------------------------------ */
/*  Transmission                                                       */
/* ------------------------------------------------------------------ */

static int Bcd(int n) { return ((n / 10) % 10) << 4 | (n % 10); }

static bool Blank(const unsigned char *row)
{
    for (int i = 0; i < COLS; i++) if (row[i] != ' ') return false;
    return true;
}

static void TimeFiller()                        /* page 1FF: keeps the clock running */
{
    unsigned char row[COLS];
    MakeHeaderRow(0x1FF, row);
    SendHeader(0x1FF, 0x3F7F, false, row);
}

static void SendPage(Page &p)
{
    time_t now = time(NULL);
    if (p.subs.size() > 1) {
        if (p.lastCycle == 0) p.lastCycle = now;
        if (now - p.lastCycle >= p.cycleSecs) { p.current = (p.current + 1) % p.subs.size(); p.lastCycle = now; }
    }
    const SubPage &s = p.subs[p.current];
    int mag = p.number >> 8;
    int subcode = p.subs.size() > 1 ? Bcd((int)p.current + 1) : 0;

    unsigned char hdr[COLS];
    MakeHeaderRow(p.number, hdr);
    SendHeader(p.number, subcode, true, hdr);

    for (int r = 1; r < ROWS; r++) {
        unsigned char row[COLS];
        memcpy(row, s.cell[r], COLS);
        if (r == ROWS - 1 && !g_flash.empty()) FlashRow(row);
        if (Blank(row)) continue;
        SendRow(mag, r, row);
    }
    bool any = false;
    for (int i = 0; i < 6; i++) if (p.links[i]) any = true;
    if (any) SendFastext(p.number, p.links);
}

/* ------------------------------------------------------------------ */
/*  -p: show a page as text, decoded back from the packets             */
/* ------------------------------------------------------------------ */

static int Unham(unsigned char b)
{
    for (int n = 0; n < 16; n++) if (g_ham[n] == b) return n;
    return -1;
}

static int Unpar(unsigned char b, int &errors)
{
    int ones = 0;
    for (int i = 0; i < 8; i++) if (b & (1 << i)) ones++;
    if ((ones & 1) == 0) errors++;
    return b & 0x7F;
}

static int DumpPage(int num)
{
    std::map<int, Page>::iterator it = g_pages.find(num);
    if (it == g_pages.end() || it->second.subs.empty()) {
        fprintf(stderr, "Page %03X is not in %s\n", num, g_dir.c_str());
        return 1;
    }
    Page &p = it->second;
    int errors = 0;
    for (size_t sp = 0; sp < p.subs.size(); sp++) {
        p.current = sp;
        g_capture.clear();
        g_cap = true;
        SendPage(p);
        g_cap = false;

        char rows[ROWS][COLS + 1];
        bool have[ROWS];
        memset(have, 0, sizeof(have));
        std::string fastext;
        int sub = 0, erase = 0, packets = (int)(g_capture.size() / 42);
        for (int i = 0; i < packets; i++) {
            const unsigned char *k = &g_capture[(size_t)i * 42];
            int a = Unham(k[0]), b = Unham(k[1]);
            if (a < 0 || b < 0) { errors++; continue; }
            int mag = (a & 7) ? (a & 7) : 8, row = (a >> 3) | (b << 1);
            if (row == 0) {
                int u = Unham(k[2]), t = Unham(k[3]), s1 = Unham(k[4]), s2 = Unham(k[5]),
                    s3 = Unham(k[6]), s4 = Unham(k[7]);
                if (u < 0 || t < 0 || s1 < 0 || s2 < 0 || s3 < 0 || s4 < 0) { errors++; continue; }
                sub = s1 | (s2 & 7) << 4 | s3 << 8 | (s4 & 3) << 12;
                erase = (s2 >> 3) & 1;
                memset(rows[0], ' ', COLS); rows[0][COLS] = 0;
                memcpy(rows[0], " P", 2);
                snprintf(rows[0] + 2, 6, "%X%X%X ", mag, t, u);
                rows[0][7] = ' ';
                for (int c = 0; c < 32; c++) rows[0][8 + c] = (char)Unpar(k[10 + c], errors);
                have[0] = true;
            } else if (row >= 1 && row <= 24) {
                for (int c = 0; c < COLS; c++) rows[row][c] = (char)Unpar(k[2 + c], errors);
                rows[row][COLS] = 0;
                have[row] = true;
            } else if (row == 27) {
                static const char *colour[] = { "red", "green", "yellow", "cyan", "spare", "index" };
                for (int l = 0; l < 6; l++) {
                    const unsigned char *q = k + 3 + 6 * l;
                    int u = Unham(q[0]), t = Unham(q[1]), m1 = Unham(q[3]), m2 = Unham(q[5]);
                    if (u < 0 || t < 0 || m1 < 0 || m2 < 0) { errors++; continue; }
                    int rel = ((m1 >> 3) & 1) | (((m2 >> 2) & 1) << 1) | (((m2 >> 3) & 1) << 2);
                    int m = (mag ^ rel) & 7; if (!m) m = 8;
                    char buf[40];
                    if (t == 0xF && u == 0xF) snprintf(buf, sizeof(buf), "%s -  ", colour[l]);
                    else snprintf(buf, sizeof(buf), "%s %X%X%X  ", colour[l], m, t, u);
                    fastext += buf;
                }
            }
        }
        printf("Page %03X   subpage %04X   %d packets   %s\n", num, sub, packets,
               erase ? "erase-page set" : "");
        printf("    +----------------------------------------+\n");
        for (int r = 0; r < ROWS; r++) {
            char line[COLS + 1];
            for (int c = 0; c < COLS; c++) {
                unsigned char ch = have[r] ? (unsigned char)rows[r][c] : ' ';
                line[c] = (ch < 0x20 || ch > 0x7E) ? ' ' : (char)ch;      /* control codes show as blanks */
            }
            line[COLS] = 0;
            printf(" %2d |%s|%s\n", r, line, have[r] ? "" : " (not sent)");
        }
        printf("    +----------------------------------------+\n");
        printf("    Fastext: %s\n\n", fastext.empty() ? "none" : fastext.c_str());
    }
    if (!g_flash.empty()) printf("(newsflash is on: row 24 carries it)\n");
    printf("%s\n", errors ? "ERRORS found while decoding the packets" : "Hamming and parity checks: all OK");
    return errors ? 2 : 0;
}

static void LoadServiceName()
{
    std::string path = g_dir + "/../ttxserver.cfg";
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "name=", 5)) {
            std::string n = line + 5;
            while (!n.empty() && (n[n.size() - 1] == '\n' || n[n.size() - 1] == '\r' || n[n.size() - 1] == ' '))
                n.erase(n.size() - 1);
            if (!n.empty()) g_name = n;
        }
    }
    fclose(f);
}

int main(int argc, char *argv[])
{
    bool haveDir = false, haveName = false;
    int dumpPage = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) { g_dir = argv[++i]; haveDir = true; }
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) { g_name = argv[++i]; haveName = true; }
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) g_rate = atol(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) g_limit = atol(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) dumpPage = (int)strtol(argv[++i], NULL, 16);
        else { fprintf(stderr, "Usage: %s [-d pagesdir] [-n NAME] [-r packets/sec] [-c count] [-p page]\n", argv[0]); return 1; }
    }
    struct stat st;
    if (!haveDir && stat(g_dir.c_str(), &st) != 0) g_dir = "pages";
    if (!haveName) LoadServiceName();
    InitHamming();

    if (dumpPage) {
        if (dumpPage < 0x100 || dumpPage > 0x8FF) { fprintf(stderr, "Page must be 100-8FF\n"); return 1; }
        ReloadIfChanged();
        CheckFlash();
        return DumpPage(dumpPage);
    }
    fprintf(stderr, "=== Teletext VBI stream '%s' from %s ===\n", g_name.c_str(), g_dir.c_str());
    for (;;) {
        ReloadIfChanged();
        CheckFlash();
        if (g_pages.empty()) {                                  /* nothing to send yet */
            TimeFiller();
            usleep(20000);
            continue;
        }
        std::vector<int> nums;
        for (std::map<int, Page>::iterator it = g_pages.begin(); it != g_pages.end(); it++) nums.push_back(it->first);
        for (size_t i = 0; i < nums.size(); i++) {
            std::map<int, Page>::iterator it = g_pages.find(nums[i]);
            if (it == g_pages.end() || it->second.subs.empty()) continue;
            SendPage(it->second);
            TimeFiller();
        }
    }
}
