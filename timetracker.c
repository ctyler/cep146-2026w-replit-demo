/*
 * timetracker.c - A full-screen terminal time tracker using ncurses
 *
 * Compile: gcc -std=c99 -o timetracker timetracker.c -lncurses
 *
 * Compatible with GCC on Fedora 42 and CentOS 7.
 */

#define _POSIX_C_SOURCE 200112L

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

/* ── Constants ────────────────────────────────────────────────────────────── */
#define MAX_PROJECTS  9
#define MAX_NAME_LEN  48
#define BAR_WIDTH     22
#define CSV_FILE      "timetracker.csv"

/* ── Color pairs ──────────────────────────────────────────────────────────── */
#define CP_TITLE     1   /* White on blue  – title bar            */
#define CP_HEADER    2   /* Magenta on white – column headers     */
#define CP_ACTIVE    3   /* White on green  – selected project    */
#define CP_NORMAL    4   /* Black on white  – normal rows         */
#define CP_BAR_FILL  5   /* White on cyan   – filled bar          */
#define CP_BAR_EMPTY 6   /* Cyan on white   – empty bar dots      */
#define CP_STATUS    7   /* White on green  – status line         */
#define CP_MENU      8   /* Yellow on blue  – bottom menu         */
#define CP_PAUSED    9   /* White on red    – paused indicator    */
#define CP_TOTAL    10   /* Blue on white   – total time          */
#define CP_DIALOG   11   /* Black on yellow – input dialog        */

/* ── Data structures ──────────────────────────────────────────────────────── */
typedef struct {
    char name[MAX_NAME_LEN];
    long seconds;          /* cumulative seconds */
} Project;

/* ── Globals ──────────────────────────────────────────────────────────────── */
static Project projects[MAX_PROJECTS];
static int     num_projects    = 0;
static int     current_project = -1;  /* 0-based; -1 = none */
static int     paused          = 0;
static time_t  last_tick;

/* Menu item click zones */
typedef struct {
    int  y, x, width;
    int  key;               /* key code this click emulates */
} ClickZone;

#define MAX_ZONES 16
static ClickZone zones[MAX_ZONES];
static int       num_zones   = 0;
static int       proj_row0   = 0;   /* screen row of first project */

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void save_data(void);
static void handle_key(int ch);
static void redraw(void);

/* ── Signal handler ───────────────────────────────────────────────────────── */
static void signal_handler(int sig)
{
    (void)sig;
    save_data();
    endwin();
    exit(0);
}

/* ── CSV persistence ──────────────────────────────────────────────────────── */
static void save_data(void)
{
    FILE *f = fopen(CSV_FILE, "w");
    if (!f) return;
    fprintf(f, "name,seconds\n");
    for (int i = 0; i < num_projects; i++) {
        /* Quote the name to handle commas/spaces */
        fprintf(f, "\"%s\",%ld\n", projects[i].name, projects[i].seconds);
    }
    fclose(f);
}

static void load_data(void)
{
    FILE *f = fopen(CSV_FILE, "r");
    if (!f) return;

    char line[256];
    /* Skip header */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    num_projects = 0;
    while (fgets(line, sizeof(line), f) && num_projects < MAX_PROJECTS) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        char  name[MAX_NAME_LEN];
        long  secs = 0;

        if (line[0] == '"') {
            /* Quoted name */
            char *end_q = strchr(line + 1, '"');
            if (!end_q) continue;
            int nlen = (int)(end_q - (line + 1));
            if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
            memcpy(name, line + 1, (size_t)nlen);
            name[nlen] = '\0';
            if (*(end_q + 1) == ',') sscanf(end_q + 2, "%ld", &secs);
        } else {
            char *comma = strchr(line, ',');
            if (!comma) continue;
            *comma = '\0';
            snprintf(name, MAX_NAME_LEN, "%s", line);
            sscanf(comma + 1, "%ld", &secs);
        }
        snprintf(projects[num_projects].name, MAX_NAME_LEN, "%s", name);
        projects[num_projects].seconds = secs;
        num_projects++;
    }
    fclose(f);
}

/* ── Time formatting ──────────────────────────────────────────────────────── */
/* buf must be at least 32 bytes */
static void fmt_time(long seconds, char *buf, int bufsz)
{
    if (seconds < 0) seconds = 0;
    unsigned long h = (unsigned long)seconds / 3600UL;
    unsigned long m = ((unsigned long)seconds % 3600UL) / 60UL;
    unsigned long s = (unsigned long)seconds % 60UL;
    snprintf(buf, (size_t)bufsz, "%02lu:%02lu:%02lu", h, m, s);
}

/* ── Register a clickable zone ────────────────────────────────────────────── */
static void add_zone(int y, int x, int w, int key)
{
    if (num_zones >= MAX_ZONES) return;
    zones[num_zones].y     = y;
    zones[num_zones].x     = x;
    zones[num_zones].width = w;
    zones[num_zones].key   = key;
    num_zones++;
}

/* ── Draw bottom menu bar ─────────────────────────────────────────────────── */
static void draw_menu(int row, int cols)
{
    struct { int key; const char *label; } items[] = {
        {'1',  "1-9:Select" },
        {' ',  "SPC:Pause"  },
        {'+',  "+:Add"      },
        {'-',  "-:Remove"   },
        {'r',  "R:Reset"    },
        {'q',  "Q:Quit"     },
    };
    int n = (int)(sizeof(items) / sizeof(items[0]));

    attron(COLOR_PAIR(CP_MENU) | A_BOLD);
    mvhline(row, 0, ' ', cols);

    int x = 1;
    for (int i = 0; i < n && x < cols - 2; i++) {
        char buf[40];
        snprintf(buf, sizeof(buf), "[ %s ]", items[i].label);
        int w = (int)strlen(buf);
        if (x + w + 1 >= cols) break;
        mvprintw(row, x, "%s", buf);
        add_zone(row, x, w, items[i].key);
        x += w + 2;
    }
    attroff(COLOR_PAIR(CP_MENU) | A_BOLD);
}

/* ── Main redraw ──────────────────────────────────────────────────────────── */
static void redraw(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    num_zones = 0;

    bkgd(COLOR_PAIR(CP_NORMAL));
    erase();

    /* ── Title bar ─────────────────────────────────────── */
    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvhline(0, 0, ' ', cols);
    const char *title = "  TIME TRACKER  ";
    mvprintw(0, (cols - (int)strlen(title)) / 2, "%s", title);
    /* Show clock */
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char clock_buf[16];
    strftime(clock_buf, sizeof(clock_buf), "%H:%M:%S", tm_now);
    mvprintw(0, cols - (int)strlen(clock_buf) - 2, "%s", clock_buf);
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

    /* ── Column header ─────────────────────────────────── */
    attron(COLOR_PAIR(CP_HEADER) | A_BOLD | A_UNDERLINE);
    mvprintw(2, 1, "No %-*s  %-8s  %-7s  %-*s",
             MAX_NAME_LEN - 1, "Project",
             "Time",
             "   %",
             BAR_WIDTH, "Percentage");
    attroff(COLOR_PAIR(CP_HEADER) | A_BOLD | A_UNDERLINE);

    /* ── Total time ────────────────────────────────────── */
    long total = 0;
    for (int i = 0; i < num_projects; i++)
        total += projects[i].seconds;

    proj_row0 = 3;

    /* ── Project rows ──────────────────────────────────── */
    for (int i = 0; i < num_projects; i++) {
        int row = proj_row0 + i;
        char tbuf[32];
        fmt_time(projects[i].seconds, tbuf, (int)sizeof(tbuf));

        double pct = (total > 0) ? (100.0 * projects[i].seconds / total) : 0.0;
        int bar_fill = (int)(pct * BAR_WIDTH / 100.0 + 0.5);
        if (bar_fill > BAR_WIDTH) bar_fill = BAR_WIDTH;

        /* Row background */
        if (i == current_project) {
            attron(COLOR_PAIR(CP_ACTIVE) | A_BOLD);
        } else {
            attron(COLOR_PAIR(CP_NORMAL));
        }
        mvhline(row, 0, ' ', cols);

        /* Print text columns */
        mvprintw(row, 1, " %d %-*s  %s  %5.1f%%  ",
                 i + 1,
                 MAX_NAME_LEN - 1, projects[i].name,
                 tbuf,
                 pct);

        if (i == current_project) {
            attroff(COLOR_PAIR(CP_ACTIVE) | A_BOLD);
        } else {
            attroff(COLOR_PAIR(CP_NORMAL));
        }

        /* Progress bar – solid colour blocks, no ACS characters */
        int bar_x = getcurx(stdscr);
        /* Filled portion: space with cyan background → solid coloured bar */
        attron(COLOR_PAIR(CP_BAR_FILL));
        for (int b = 0; b < bar_fill; b++)
            mvaddch(row, bar_x + b, ' ');
        attroff(COLOR_PAIR(CP_BAR_FILL));
        /* Empty portion: dash with lighter colour */
        attron(COLOR_PAIR(CP_BAR_EMPTY));
        for (int b = bar_fill; b < BAR_WIDTH; b++)
            mvaddch(row, bar_x + b, '-');
        attroff(COLOR_PAIR(CP_BAR_EMPTY));

        /* Register click zone for the whole project row */
        add_zone(row, 0, cols, '1' + i);
    }

    /* ── Separator ─────────────────────────────────────── */
    int sep_row = proj_row0 + num_projects;
    attron(COLOR_PAIR(CP_NORMAL));
    mvhline(sep_row, 0, ACS_HLINE, cols);
    attroff(COLOR_PAIR(CP_NORMAL));

    /* ── Status line ───────────────────────────────────── */
    int status_row = sep_row + 1;
    if (paused) {
        attron(COLOR_PAIR(CP_PAUSED) | A_BOLD);
        mvhline(status_row, 0, ' ', cols);
        const char *pmsg = "  *** PAUSED – press SPACE to resume ***";
        mvprintw(status_row, (cols - (int)strlen(pmsg)) / 2, "%s", pmsg);
        attroff(COLOR_PAIR(CP_PAUSED) | A_BOLD);
    } else if (current_project >= 0 && current_project < num_projects) {
        char tbuf[32];
        fmt_time(projects[current_project].seconds, tbuf, (int)sizeof(tbuf));
        attron(COLOR_PAIR(CP_STATUS) | A_BOLD);
        mvhline(status_row, 0, ' ', cols);
        mvprintw(status_row, 2, "Tracking: [%d] %s   elapsed: %s",
                 current_project + 1,
                 projects[current_project].name,
                 tbuf);
        attroff(COLOR_PAIR(CP_STATUS) | A_BOLD);
    } else {
        attron(COLOR_PAIR(CP_STATUS));
        mvhline(status_row, 0, ' ', cols);
        mvprintw(status_row, 2,
                 "No project selected – press 1-%d to start tracking",
                 (num_projects > 0) ? num_projects : 9);
        attroff(COLOR_PAIR(CP_STATUS));
    }

    /* ── Total time line ───────────────────────────────── */
    char total_buf[32];
    fmt_time(total, total_buf, (int)sizeof(total_buf));
    attron(COLOR_PAIR(CP_TOTAL) | A_BOLD);
    mvprintw(status_row + 1, 2, "Total time tracked: %s", total_buf);
    attroff(COLOR_PAIR(CP_TOTAL) | A_BOLD);

    /* ── Bottom menu ───────────────────────────────────── */
    draw_menu(rows - 1, cols);

    /* Suppress cursor */
    move(rows - 1, cols - 1);
    refresh();
}

/* ── Add a project (prompts for name) ────────────────────────────────────── */
static void add_project(void)
{
    if (num_projects >= MAX_PROJECTS) return;

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    int dh = 5, dw = 52;
    int dy = rows / 2 - dh / 2;
    int dx = cols / 2 - dw / 2;

    /* Draw dialog */
    attron(COLOR_PAIR(CP_DIALOG) | A_BOLD);
    for (int y = dy; y < dy + dh; y++)
        mvhline(y, dx, ' ', dw);
    mvprintw(dy + 1, dx + 2, "Add Project (max %d chars):", MAX_NAME_LEN - 1);
    mvprintw(dy + 3, dx + 2, "Press ENTER to confirm, ESC to cancel.");
    attroff(COLOR_PAIR(CP_DIALOG) | A_BOLD);

    /* Input field */
    attron(COLOR_PAIR(CP_NORMAL) | A_UNDERLINE);
    mvhline(dy + 2, dx + 2, ' ', dw - 4);
    attroff(COLOR_PAIR(CP_NORMAL) | A_UNDERLINE);

    /* Gather input */
    echo();
    curs_set(1);
    timeout(-1);  /* blocking */

    char name[MAX_NAME_LEN];
    memset(name, 0, sizeof(name));
    int pos = 0;
    move(dy + 2, dx + 2);
    refresh();

    int ch;
    while ((ch = getch()) != '\n' && ch != KEY_ENTER && ch != 27 /*ESC*/) {
        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (pos > 0) {
                pos--;
                name[pos] = '\0';
                mvaddch(dy + 2, dx + 2 + pos, ' ');
                move(dy + 2, dx + 2 + pos);
                refresh();
            }
        } else if (isprint(ch) && pos < MAX_NAME_LEN - 1) {
            name[pos++] = (char)ch;
            name[pos]   = '\0';
            mvaddch(dy + 2, dx + 2 + pos - 1, (chtype)ch);
            move(dy + 2, dx + 2 + pos);
            refresh();
        }
    }

    noecho();
    curs_set(0);
    timeout(1000);

    if (ch != 27 && pos > 0) {
        snprintf(projects[num_projects].name, MAX_NAME_LEN, "%s", name);
        projects[num_projects].seconds = 0;
        num_projects++;
    }
    last_tick = time(NULL);  /* reset tick so we don't count dialog time */
}

/* ── Remove a project ─────────────────────────────────────────────────────── */
static void remove_project(void)
{
    if (num_projects == 0) return;
    int idx = (current_project >= 0 && current_project < num_projects)
              ? current_project : num_projects - 1;

    for (int i = idx; i < num_projects - 1; i++)
        projects[i] = projects[i + 1];
    num_projects--;

    if (current_project >= num_projects)
        current_project = num_projects - 1;
}

/* ── Reset all timers ─────────────────────────────────────────────────────── */
static void reset_timers(void)
{
    for (int i = 0; i < num_projects; i++)
        projects[i].seconds = 0;
}

/* ── Key handler ──────────────────────────────────────────────────────────── */
static void handle_key(int ch)
{
    if (ch >= '1' && ch <= '9') {
        int idx = ch - '1';
        if (idx < num_projects) {
            current_project = idx;
            paused = 0;
        }
    } else if (ch == ' ') {
        if (num_projects > 0)
            paused = !paused;
    } else if (ch == '+') {
        add_project();
    } else if (ch == '-') {
        remove_project();
    } else if (ch == 'r' || ch == 'R') {
        reset_timers();
    }
    /* 'q'/'Q' handled in main loop */
}

/* ── Main ─────────────────────────────────────────────────────────────────── */
int main(void)
{
    /* Initialise ncurses */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    /* Mouse – respond only on press so each click fires exactly once */
    mousemask(BUTTON1_PRESSED, NULL);
    mouseinterval(0);

    /* 1-second timeout for getch */
    timeout(1000);

    /* Colors */
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(CP_TITLE,     COLOR_WHITE,   COLOR_BLUE);
        init_pair(CP_HEADER,    COLOR_MAGENTA, COLOR_WHITE);
        init_pair(CP_ACTIVE,    COLOR_WHITE,   COLOR_GREEN);
        init_pair(CP_NORMAL,    COLOR_BLACK,   COLOR_WHITE);
        init_pair(CP_BAR_FILL,  COLOR_WHITE,   COLOR_CYAN);
        init_pair(CP_BAR_EMPTY, COLOR_CYAN,    COLOR_WHITE);
        init_pair(CP_STATUS,    COLOR_WHITE,   COLOR_GREEN);
        init_pair(CP_MENU,      COLOR_YELLOW,  COLOR_BLUE);
        init_pair(CP_PAUSED,    COLOR_WHITE,   COLOR_RED);
        init_pair(CP_TOTAL,     COLOR_BLUE,    COLOR_WHITE);
        init_pair(CP_DIALOG,    COLOR_BLACK,   COLOR_YELLOW);
    }

    bkgd(COLOR_PAIR(CP_NORMAL));

    /* Load saved data */
    load_data();

    /* Signal handlers */
    signal(SIGTERM, signal_handler);
    signal(SIGHUP,  signal_handler);

    /* Prime the tick */
    last_tick = time(NULL);

    /* Main loop */
    int running = 1;
    while (running) {
        /* Advance timer before drawing */
        time_t now = time(NULL);
        if (!paused && current_project >= 0 && current_project < num_projects) {
            long elapsed = (long)(now - last_tick);
            if (elapsed > 0)
                projects[current_project].seconds += elapsed;
        }
        last_tick = now;

        redraw();

        int ch = getch();
        if (ch == ERR) continue;   /* timeout – loop again */

        if (ch == KEY_MOUSE) {
            MEVENT ev;
            if (getmouse(&ev) == OK) {
                int my = ev.y, mx = ev.x;
                /* Check registered click zones */
                for (int z = 0; z < num_zones; z++) {
                    if (my == zones[z].y &&
                        mx >= zones[z].x &&
                        mx <  zones[z].x + zones[z].width)
                    {
                        int k = zones[z].key;
                        if (k == 'q' || k == 'Q') {
                            running = 0;
                        } else {
                            handle_key(k);
                        }
                        break;
                    }
                }
            }
            continue;
        }

        if (ch == 'q' || ch == 'Q') {
            running = 0;
        } else {
            handle_key(ch);
        }
    }

    save_data();
    endwin();
    return 0;
}
