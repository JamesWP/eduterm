#define _XOPEN_SOURCE 600
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>
#include <argp.h>

/* Launching /bin/sh may launch a GNU Bash and that can have nasty side
 * effects. On my system, it clobbers ~/.bash_history because it doesn't
 * respect $HISTSIZE from my ~/.bashrc. That's very annoying. So, launch
 * /bin/dash which does nothing of the sort. */
#define SHELL "/bin/bash"

struct PTY {
    int master, slave;
};

struct RGB {
    unsigned char r, g, b;
};

bool exit_mode = false;
bool print_child = false;

static struct RGB col_os_vals[8 + 8] = {{0, 0, 0},         // black
                                        {205, 0, 0},       // red
                                        {0, 205, 0},       // green
                                        {205, 205, 0},     // yellow
                                        {0, 0, 238},       // blue
                                        {205, 0, 205},     // magenta
                                        {0, 205, 205},     // cyan
                                        {229, 229, 229},   // white
                                        {127, 127, 127},   // bright black
                                        {255, 0, 0},       // bright red
                                        {0, 255, 0},       // bright green
                                        {255, 255, 0},     // bright yellow
                                        {92, 92, 255},     // bright blue
                                        {255, 0, 255},     // bright magenta
                                        {0, 255, 255},     // bright cyan
                                        {255, 255, 255}};  // bright white

static unsigned char grayramp[24] = {1,  2,  3,  5,  6,  7,  8,  9,
                                     11, 12, 13, 14, 16, 17, 18, 19,
                                     20, 22, 23, 24, 25, 27, 28, 29};

static unsigned char colorramp[6] = {0, 12, 16, 21, 26, 31};

#define col_os_length (sizeof(col_os_vals) / sizeof(col_os_vals[0]))

struct cell {
    wchar_t       g;
    unsigned long fg;
    unsigned long bg;
    bool          bold;
    bool          italic;
    bool          dirty;
};

bool equals(struct cell* a, struct cell* b)
{
    if (a == b)
        return true;
    if (a == NULL || b == NULL)
        return false;
    if (a->g != b->g)
        return false;
    if (a->fg != b->fg)
        return false;
    if (a->bg != b->bg)
        return false;
    if (a->bold != b->bold)
        return false;
    if (a->italic != b->italic)
        return false;
    
    return true;
}

void copy(struct cell* dest, struct cell* source)
{
    if (equals(dest, source))
        return;
    
    *dest = *source;
    
    dest->dirty = true;
}

#define eexit(i)                                            \
    do {                                                    \
        printf("Error file:%s, function:%s() and line:%d\n",\
               __FILE__,__func__,__LINE__);                 \
        if (exit_mode) exit((i));                           \
    } while(0);
    

struct X11 {
    int      fd;
    Display *dpy;
    int      screen;
    Window   root;

    Window        termwin;
    GC            termgc;
    unsigned long col_fg, col_bg, col_bk;
    int           w, h;

    XFontSet     xfontset;
    XFontSet     xboldfontset;
    XFontSet     xitalicfontset;
    int          font_width, font_height, font_yadg;

    struct cell *buf_alt;
    struct cell *buf;
    int          buf_w, buf_h;
    int          buf_x, buf_y;
    int          buf_alt_x, buf_alt_y;
    bool         blink, cur;

    int scr_begin, scr_end;

    unsigned long sgr_fg_col;
    unsigned long sgr_bg_col;
    bool          sgr_bold;
    bool          sgr_italic;

    // oldscool 3/4 bit colors, normal and bright versions
    unsigned long col_os[col_os_length];
    unsigned long col_256[256 /* duh */];

    bool application_keypad;
};

void clear(struct X11 *x11, struct cell *c)
{
    struct cell backup = *c;

    c->g     = L' ';
    c->fg    = x11->col_fg;
    c->bg    = x11->col_bg;
    c->bold  = false;
    c->italic = false;

    c->dirty |= !equals(&backup, c);
}

void dirty(struct cell *c)
{
    c->dirty = true;
}

// does not handle moving cursor or wrapping
void putch(struct X11 *x11, wchar_t g)
{
    struct cell *c = x11->buf + x11->buf_y * x11->buf_w + x11->buf_x;

    c->g     = g;
    c->fg    = x11->sgr_fg_col;
    c->bg    = x11->sgr_bg_col;
    c->bold  = x11->sgr_bold;
    c->italic  = x11->sgr_italic;
    c->dirty = true;
}

void clear_cells(struct X11* x11, struct cell* begin, struct cell* end)
{
    for (; begin != end; ++begin)
        clear(x11, begin);
}

void clear_all_cells(struct X11 *x11)
{
    clear_cells(x11, x11->buf, x11->buf + x11->buf_w * x11->buf_h);
}

void dirty_cells(struct cell* begin, struct cell* end)
{
    for (; begin != end; ++begin)
        dirty(begin);
}

void dirty_all_cells(struct X11 *x11)
{
    dirty_cells(x11->buf, x11->buf + x11->buf_w * x11->buf_h);
}

void switch_buffers(struct X11* x11) 
{
    struct cell *tmp = x11->buf;

    x11->buf     = x11->buf_alt;
    x11->buf_alt = tmp;

    int tmpc;
    tmpc           = x11->buf_x;
    x11->buf_x     = x11->buf_alt_x;
    x11->buf_alt_x = tmpc;

    tmpc           = x11->buf_y;
    x11->buf_y     = x11->buf_alt_y;
    x11->buf_alt_y = tmpc;
}

bool term_set_size(struct PTY *pty, struct X11 *x11)
{
    struct winsize ws = {
        .ws_col = x11->buf_w,
        .ws_row = x11->buf_h,
    };

    /* This is the very same ioctl that normal programs use to query the
     * window size. Normal programs are actually able to do this, too,
     * but it makes little sense: Setting the size has no effect on the
     * PTY driver in the kernel (it just keeps a record of it) or the
     * terminal emulator. IIUC, all that's happening is that subsequent
     * ioctls will report the new size -- until another ioctl sets a new
     * size.
     *
     * I didn't see any response to ioctls of normal programs in any of
     * the popular terminals (XTerm, VTE, st). They are not informed by
     * the kernel when a normal program issues an ioctl like that.
     *
     * On the other hand, if we were to issue this ioctl during runtime
     * and the size actually changed, child programs would get a
     * SIGWINCH. */
    if (ioctl(pty->master, TIOCSWINSZ, &ws) == -1) {
        perror("ioctl(TIOCSWINSZ)");
        return false;
    }

    return true;
}

bool pt_pair(struct PTY *pty)
{
    char *slave_name;

    /* Opens the PTY master device. This is the file descriptor that
     * we're reading from and writing to in our terminal emulator.
     *
     * We're going for BSD-style management of the controlling terminal:
     * Don't try to change anything now (O_NOCTTY), we'll issue an
     * ioctl() later on. */
    pty->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (pty->master == -1) {
        perror("posix_openpt");
        return false;
    }

    fcntl(pty->master, F_SETFL, fcntl(pty->master, F_GETFL) | O_NONBLOCK);

    /* grantpt() and unlockpt() are housekeeping functions that have to
     * be called before we can open the slave FD. Refer to the manpages
     * on what they do. */
    if (grantpt(pty->master) == -1) {
        perror("grantpt");
        return false;
    }

    if (unlockpt(pty->master) == -1) {
        perror("grantpt");
        return false;
    }

    /* Up until now, we only have the master FD. We also need a file
     * descriptor for our child process. We get it by asking for the
     * actual path in /dev/pts which we then open using a regular
     * open(). So, unlike pipe(), you don't get two corresponding file
     * descriptors in one go. */

    slave_name = ptsname(pty->master);
    if (slave_name == NULL) {
        perror("ptsname");
        return false;
    }

    pty->slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (pty->slave == -1) {
        perror("open(slave_name)");
        return false;
    }

    return true;
}

bool IsTtyFunctionOrSpaceKey(KeySym keysym)
{
    KeySym keysyms[] = {
        XK_BackSpace,
        XK_Tab,
        XK_Linefeed,
        XK_Clear,
        XK_Return,
        XK_Pause,
        XK_Scroll_Lock,
        XK_Sys_Req,
        XK_Escape,
        XK_Delete
        //XK_space
    };
    for (size_t i = 0; i < sizeof(keysyms) / sizeof(keysyms[0]); ++i) {
        if (keysyms[i] == keysym)
            return true;
    }
    return false;
}

char IsKeypad(KeySym keysym)
{
    switch (keysym) {
      case XK_Up:
        return 'A';
      case XK_Left:
        return 'D';
      case XK_Right:
        return 'C';
      case XK_Down:
        return 'B';
      default:
        return '\0';
    };
}

wchar_t utf8_to_utf32(char *buf, size_t size)
{
    if (size == 1)
        return (buf[0] & 0x7F);
    else if (size == 2)
        return (buf[0] & 0x1F) << 6 | (buf[1] & 0x3F);
    else if (size == 3) {
        return (buf[0] & 0x0F) << 12 | (buf[1] & 0x3F) << 6 | (buf[2] & 0x3F);
    }
    else if (size == 4)
        return (buf[0] & 0x07) << 18 | (buf[1] & 0x3F) << 12 |
               (buf[2] & 0x3F) << 6 | (buf[3] & 0x3F);
    else
        eexit(1);
    return L' ';
}

void print_utf32(wchar_t ch)
{
    char   buf[5];  // 4 utf8 bytes plus \0
    size_t len;     // length minus \0

    char *target = buf;

    static const wchar_t byteMask         = 0xBF;
    static const wchar_t byteMark         = 0x80;
    static const char    firstByteMark[7] = {
        0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC};

    if (ch < (wchar_t)0x80) {
        len = 1;
    }
    else if (ch < (wchar_t)0x800) {
        len = 2;
    }
    else if (ch < (wchar_t)0x10000) {
        len = 3;
    }
    else {
        len = 4;
    }

    target += len;

    switch (len) { /* note: everything falls through. */
      case 4: 
        *--target = (char)((ch | byteMark) & byteMask);
        ch >>= 6;
        __attribute__((fallthrough));
      case 3:
        *--target = (char)((ch | byteMark) & byteMask);
        ch >>= 6;
        __attribute__((fallthrough));
      case 2:
        *--target = (char)((ch | byteMark) & byteMask);
        ch >>= 6;
        __attribute__((fallthrough));
      case 1:
        *--target = (char)(ch | firstByteMark[len]);
    }

    buf[len] = '\0';

    printf("%s", buf);
}

void swap(unsigned long* a, unsigned long* b)
{
    unsigned long tmp = *a;
    *a = *b;
    *b = tmp;
}

void x11_redraw(struct X11 *x11)
{
    if (!x11->cur)
        return;

    size_t total = 0;
    int     x, y;

    for (y = 0; y < x11->buf_h; y++) {
        for (x = 0; x < x11->buf_w; x++) {
            struct cell *c = x11->buf + (y * x11->buf_w + x);

            bool is_cursor = x == x11->buf_x && y == x11->buf_y;

            if (!is_cursor && !c->dirty)
                continue;

            total++;
            wchar_t       g  = c->g;
            unsigned long bg = c->bg;
            unsigned long fg = c->fg;
            bool          bold = c->bold;
            bool          italic = c->italic;

            if (is_cursor && x11->blink) swap(&fg, &bg);

            XSetForeground(x11->dpy, x11->termgc, bg);

            XFillRectangle(x11->dpy,
                           x11->termwin,
                           x11->termgc,
                           x * x11->font_width,
                           y * x11->font_height,
                           x11->font_width,
                           x11->font_height);

            XSetForeground(x11->dpy, x11->termgc, fg);

            if (bold) {
                XwcDrawString(x11->dpy,
                              x11->termwin,
                              x11->xboldfontset,
                              x11->termgc,
                              x * x11->font_width,
                              y * x11->font_height + x11->font_yadg,
                              &g,
                              1);
            } else if (italic) {
                XwcDrawString(x11->dpy,
                              x11->termwin,
                              x11->xitalicfontset,
                              x11->termgc,
                              x * x11->font_width,
                              y * x11->font_height + x11->font_yadg,
                              &g,
                              1);
            } else {
                XwcDrawString(x11->dpy,
                              x11->termwin,
                              x11->xfontset,
                              x11->termgc,
                              x * x11->font_width,
                              y * x11->font_height + x11->font_yadg,
                              &g,
                              1);
            }

            if (x == x11->buf_x && y == x11->buf_y)
                c->dirty = true;
            else c->dirty = false;
        }
    }

    if (x11->blink) {
        XSetForeground(x11->dpy, x11->termgc, x11->col_fg);
    }
    else {
        XSetForeground(x11->dpy, x11->termgc, x11->col_bk);
    }

    XFlush(x11->dpy);

    // printf("Total cells drawn %d\n", (int)total);
}

char ascii_char(const struct cell* c)
{
    if (iswspace(c->g))
        return ' ';
    else if (iswcntrl(c->g))
        return ' ';
    else if (iswcntrl(c->g))
        return ' ';
    else if (iswprint(c->g))
        return c->g;
    else
        return '?';
}

char bold_char(const struct cell* c)
{
    if (c->bold)
        return '!';
    else
        return ' ';
}
char italic_char(const struct cell* c)
{
    if (c->italic)
        return '!';
    else
        return ' ';
}

void print_screen(struct X11* x11, char(*cell_val)(const struct cell *c))
{
    printf("\n");
    char row[x11->buf_w + 1];
    row[x11->buf_w] = '\0';

    for (int x = 0; x < x11->buf_w; x++)
        row[x] = '_';

    printf(" . %s . \n", row);

    for(int y=0;y<x11->buf_h; y++){
        for (int x = 0; x < x11->buf_w; x++) {
            const struct cell *c = x11->buf + (x11->buf_w * y + x);

            row[x] = cell_val(c);
        }
        printf(" | %s | \n", row);
    }

    for (int x = 0; x < x11->buf_w; x++)
        row[x] = '-';

    printf(" ` %s ` \n", row);

    printf("\n");
}

void x11_key(XKeyEvent *ev, struct PTY *pty, struct X11* x11)
{
    char   buf[32];
    int    num;
    KeySym ksym;

    num      = XLookupString(ev, buf, sizeof(buf) - 1, &ksym, 0);
    buf[num] = 0;

    if (IsTtyFunctionOrSpaceKey(ksym)) {
        printf("XKeyEvent non character = (%x) len() == %d\n", 0xFF & buf[0], num);
        if(ksym == XK_BackSpace){
            printf("XBackspace \n");
            // backspace
            num = snprintf(buf, sizeof(buf), "\33[3~");
        }
    }
    else if (IsKeypad(ksym) != '\0') {
        printf("XKeyEvent arrow key\n");
        if(x11->application_keypad)
            num = snprintf(buf, sizeof(buf), "\33O%c", IsKeypad(ksym));
        else
            num = snprintf(buf, sizeof(buf), "\33[%c", IsKeypad(ksym));
    }
    else {
        printf("XKeyEvent string = '%s'\n", buf);
    }

    switch(ksym) {
      case XK_Home: {
        dirty_all_cells(x11);
        clear_all_cells(x11);
        x11_redraw(x11);
      } break;
      case XK_Insert: {
        print_screen(x11, bold_char);
        print_screen(x11, italic_char);
        print_screen(x11, ascii_char);
      } break;
      default: {
        int ignore = write(pty->master, buf, num);
        (void)ignore;
      } break;
    }

}


bool x11_setup(struct X11 *x11)
{
    Colormap             cmap;
    XColor               color;
    Atom                 atom_net_wmname;
    XSetWindowAttributes wa = {
        .background_pixmap = ParentRelative,
        .event_mask        = KeyPressMask | KeyReleaseMask | ExposureMask,
    };

    x11->blink = true;
    x11->cur   = true;

    x11->dpy = XOpenDisplay(NULL);
    if (x11->dpy == NULL) {
        fprintf(stderr, "Cannot open display\n");
        return false;
    }

    x11->screen = DefaultScreen(x11->dpy);
    x11->root   = RootWindow(x11->dpy, x11->screen);
    x11->fd     = ConnectionNumber(x11->dpy);

    const char *font;

    font = "-*-fixed-medium-*-normal-*-*-140-*-*-*-90-*-";
    font = "-*-fixed-medium-r-normal-*-13-*-*-*-*-*-*-1";

    char **missing_charsets;
    int    num_missing_charsets;
    char  *default_string;

    x11->xfontset = XCreateFontSet(x11->dpy,
                                   font,
                                   &missing_charsets,
                                   &num_missing_charsets,
                                   &default_string);

    font = "-*-fixed-bold-r-normal-*-13-*-*-*-*-*-*-1";

    x11->xboldfontset = XCreateFontSet(x11->dpy,
                                       font,
                                       &missing_charsets,
                                       &num_missing_charsets,
                                       &default_string);

    font = "-*-fixed-*-o-*-*-13-*-*-*-*-*-*-1";

    x11->xitalicfontset = XCreateFontSet(x11->dpy,
                                         font,
                                         &missing_charsets,
                                         &num_missing_charsets,
                                         &default_string);

    XFontSetExtents* ext = XExtentsOfFontSet(x11->xfontset);

    if (!ext) {
        printf("Could not load font size\n");
        exit(1);
    }

    x11->font_width  = ext->max_logical_extent.width;
    x11->font_height = ext->max_logical_extent.height;
    x11->font_yadg   = -ext->max_logical_extent.y;

    cmap = DefaultColormap(x11->dpy, x11->screen);

    if (!XAllocNamedColor(x11->dpy, cmap, "#000000", &color, &color)) {
        fprintf(stderr, "Could not load bg color\n");
        return false;
    }
    x11->col_bg = color.pixel;

    if (!XAllocNamedColor(x11->dpy, cmap, "#aaaaaa", &color, &color)) {
        fprintf(stderr, "Could not load fg color\n");
        return false;
    }

    x11->col_fg     = color.pixel;
    x11->sgr_fg_col = x11->col_fg;
    x11->sgr_bg_col = x11->col_bg;
    x11->sgr_bold   = false;

    if (!XAllocNamedColor(x11->dpy, cmap, "#444444", &color, &color)) {
        fprintf(stderr, "Could not load blink color\n");
        return false;
    }
    x11->col_bk = color.pixel;

    for (int i = 0; i < (int)col_os_length; i++) {
        XColor c;
        c.red   = col_os_vals[i].r * 255;
        c.green = col_os_vals[i].g * 255;
        c.blue  = col_os_vals[i].b * 255;
        if (!XAllocColor(x11->dpy, cmap, &c)) {
            fprintf(stderr, "Could not load col_os[%d] color\n", i);
            return false;
        }
        x11->col_os[i] = c.pixel;
        x11->col_256[i] = c.pixel;
    }

    size_t col_map_dest = 16;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                XColor c;
                c.red   = (colorramp[r] * 255 / 31) * 255;
                c.green = (colorramp[g] * 255 / 31) * 255;
                c.blue  = (colorramp[b] * 255 / 31) * 255;
                if (!XAllocColor(x11->dpy, cmap, &c)) {
                    fprintf(stderr,
                            "Could not load col_256[%d] color\n",
                            (int)col_map_dest);
                    return false;
                }
                x11->col_256[col_map_dest++] = c.pixel;
            }
        }
    }

    for (int i = 0; i < 24; i++) {
        XColor c;
        c.red = c.green = c.blue = (grayramp[i] * 255 / 31) * 255;
        if (!XAllocColor(x11->dpy, cmap, &c)) {
            fprintf(stderr,
                    "Could not load col_256[%d] color\n",
                    (int)col_map_dest);
            return false;
        }
        x11->col_256[col_map_dest++] = c.pixel;
    }

    /* The terminal will have a fixed size of 80x25 cells. This is an
     * arbitrary number. No resizing has been implemented and child
     * processes can't even ask us for the current size (for now).
     *
     * buf_x, buf_y will be the current cursor position. */
    x11->buf_w = 80;
    x11->buf_h = 45;
    x11->buf_x = x11->buf_alt_x = 0;
    x11->buf_y = x11->buf_alt_y = 0;
    x11->buf   = calloc(x11->buf_w * x11->buf_h * sizeof(x11->buf[0]), 1);
    clear_all_cells(x11);
    dirty_all_cells(x11);

    if (x11->buf == NULL) {
        perror("calloc");
        return false;
    }

    switch_buffers(x11);

    x11->buf   = calloc(x11->buf_w * x11->buf_h * sizeof(x11->buf[0]), 1);
    clear_all_cells(x11);
    dirty_all_cells(x11);

    if (x11->buf == NULL) {
        perror("calloc");
        return false;
    }

    x11->application_keypad = false;

    x11->scr_begin = 0;
    x11->scr_end   = x11->buf_h - 1;

    x11->w = x11->buf_w * x11->font_width;
    x11->h = x11->buf_h * x11->font_height;

    x11->termwin = XCreateWindow(x11->dpy,
                                 x11->root,
                                 0,
                                 0,
                                 x11->w,
                                 x11->h,
                                 0,
                                 DefaultDepth(x11->dpy, x11->screen),
                                 CopyFromParent,
                                 DefaultVisual(x11->dpy, x11->screen),
                                 CWBackPixmap | CWEventMask,
                                 &wa);
    XMapWindow(x11->dpy, x11->termwin);
    x11->termgc = XCreateGC(x11->dpy, x11->termwin, 0, NULL);

    atom_net_wmname = XInternAtom(x11->dpy, "_NET_WM_NAME", False);
    XChangeProperty(x11->dpy,
                    x11->termwin,
                    atom_net_wmname,
                    XInternAtom(x11->dpy, "UTF8_STRING", False),
                    8,
                    PropModeReplace,
                    (unsigned char *)"eduterm",
                    strlen("eduterm"));

    XSync(x11->dpy, False);

    return true;
}

bool spawn(struct PTY *pty)
{
    pid_t  p;

    p = fork();
    if (p == 0) {
        close(pty->master);

        /* Create a new session and make our terminal this process'
         * controlling terminal. The shell that we'll spawn in a second
         * will inherit the status of session leader. */
        setsid();
        if (ioctl(pty->slave, TIOCSCTTY, NULL) == -1) {
            perror("ioctl(TIOCSCTTY)");
            return false;
        }

        dup2(pty->slave, 0);
        dup2(pty->slave, 1);
        dup2(pty->slave, 2);
        close(pty->slave);

        //putenv("TERM=xterm-256color");

        execlp(SHELL, "-"SHELL, NULL);
        return false;
    }
    else if (p > 0) {
        close(pty->slave);
        return true;
    }

    perror("fork");
    return false;
}

bool is_final_csi_byte(char b)
{
    return b >= 0x40 && b <= 0x7c;
}

bool is_final_osi_byte(char b)
{
    return b == 7;
}

int atoi_range(char *buf, size_t len, int def)
{
    int s = def;
    if (len >= 1) {
        s = atoi(buf);
    }
    return s;
}

void process_csi(char *buf, size_t len, struct X11 *x11, struct PTY *pty)
{
    char op = buf[len];

    switch (op) {
      case 'm':
        break;
      default:
        printf("Processing CSI '%s' op %c\n", buf, op);
    }

    struct cell *const lstart = x11->buf + x11->buf_w * x11->buf_y;
    struct cell *const cursor = lstart + x11->buf_x;
    struct cell *const lend   = lstart + x11->buf_w - 1;

    switch (op) {
      case '@': {
        // insert character into line
        //             lstart
        //                 cursor
        //                        lend
        //                   bend
        //  insert 2 : |---c123456|
        //             |---__c1234|
        int num = atoi_range(buf, len - 1, 1);
        for (struct cell *source = lend - num, *dest = lend; source >= cursor;
             --dest, --source)
            copy(dest, source);

        for (struct cell *bend = cursor + num - 1; bend != cursor - 1;
             --bend) 
            clear(x11, bend);

      } break;
      case 'B':
      case 'A': {
        int num = 1;
        sscanf(buf, "%d", &num);
        bool up = buf[num-1] == 'A';
        x11->buf_y += (up ? -1 : 1) * num;
        x11->buf_y = x11->buf_y > x11->buf_h - 1 ? x11->buf_h - 1 : x11->buf_y;
        x11->buf_y = x11->buf_y < 0 ? 0 : x11->buf_y;
      } break;
      case 'P': {
        // Delete characters
        int num = 1;
        sscanf(buf, "%dP", &num);
        for (struct cell *source = cursor + num, *dest = cursor;
             source != lend + 1;
             ++source, ++dest)
            copy(dest, source);
        clear_cells(x11 ,lend - (num-1), lend +1);

      } break;
      case 'm': {
        // SGR - Select Graphic Rendition
        char *arg_s = strtok(buf, ";");
        while (arg_s) {
            int arg = atoi(arg_s);
            arg_s   = strtok(NULL, ";");
            switch (arg) {
              case 0:
                x11->sgr_fg_col = x11->col_fg;
                x11->sgr_bg_col = x11->col_bg;
                x11->sgr_bold = false;
                x11->sgr_italic = false;
                break;
              case 1:
                x11->sgr_bold = true;
                break;
              case 3:
                x11->sgr_italic = true;
                break;
              case 30:
              case 31:
              case 32:
              case 33:
              case 34:
              case 35:
              case 36:
              case 37:
                x11->sgr_fg_col = x11->col_os[arg - 30];
                break;
              case 38:
                arg             = atoi(arg_s);
                arg_s           = strtok(NULL, ";");
                if (arg == 5) {
                    arg             = atoi(arg_s);
                    arg_s           = strtok(NULL, ";");
                    x11->sgr_fg_col = x11->col_256[arg];
                }
                else {
                    eexit(1);
                }
                break;
              case 40:
              case 41:
              case 42:
              case 43:
              case 44:
              case 45:
              case 46:
              case 47:
                x11->sgr_bg_col = x11->col_os[arg - 40];
                break;
              case 48:
                arg             = atoi(arg_s);
                arg_s           = strtok(NULL, ";");
                if (arg == 5) {
                    arg             = atoi(arg_s);
                    arg_s           = strtok(NULL, ";");
                    x11->sgr_bg_col = x11->col_256[arg];
                }
                else {
                    eexit(1);
                }
                break;
              case 91:
              case 92:
              case 93:
              case 94:
              case 95:
              case 96:
              case 97:
                x11->sgr_fg_col = x11->col_os[arg - 90 + 8];
                break;
              case 101:
              case 102:
              case 103:
              case 104:
              case 105:
              case 106:
              case 107:
                x11->sgr_bg_col = x11->col_os[arg - 100 + 8];
                break;
            }
        }
      } break;
      case 'J': {
        int arg1 = 0;
        sscanf(buf, "%dJ", &arg1);
        if (arg1 == 2 || arg1 == 3) {
            for (struct cell *a = x11->buf;
                 a != x11->buf + x11->buf_w * x11->buf_h;
                 ++a) {
                clear(x11, a);
            }
            x11->buf_x = 0;
            x11->buf_y = 0;
        }
        else {
            eexit(1);
        }
      } break;
      case 'c': {
        if (buf[0] == '>') {
            const char* reply = "\e[>77;20805;0c";
            int  num = strlen(reply);
            int         ignore = write(pty->master, reply, num);
            (void)ignore;
        }
        else {
            eexit(1);
        }
      } break;
      case 'C': {
        int arg1 = 1;
        sscanf(buf, "%dC", &arg1);
        x11->buf_x += arg1;
        x11->buf_x = x11->buf_x < x11->buf_w - 1 ? x11->buf_x : x11->buf_w - 1;
      } break;
      case 'H': {
        int r = 1; int c = 1;
        if (len > 1) {
            sscanf(buf, "%d;%dH", &r, &c);
        }
        x11->buf_x = c - 1;
        x11->buf_y = r - 1;
        x11->buf_x = x11->buf_x < x11->buf_w ? x11->buf_x : x11->buf_w - 1;
        x11->buf_y = x11->buf_y < x11->buf_h ? x11->buf_y : x11->buf_h - 1;
      } break;
      case 'K': {
        int arg1 = 0;
        sscanf(buf, "%dK", &arg1);
        switch (arg1) {
          case 0: {
            for (struct cell *a = cursor; a != lend + 1; ++a) {
                clear(x11, a);
            }
          } break;
          default:
            eexit(1);
        }
      } break;
      case 'r': {
        if (len >= 1) {
            int start, end;
            sscanf(buf, "%d;%dr", &start, &end);
            x11->scr_begin = start - 1;
            x11->scr_end   = end - 1;
        }
        else {
            x11->scr_begin = 0;
            x11->scr_end   = x11->buf_h - 1;
        }
        printf("Scroll region set to %d %d\n", x11->scr_begin, x11->scr_end);
      } break;
      case 'l': {
        int arg1;
        sscanf(buf, "?%dl", &arg1);
        // CSI ? P m l   DEC Private Mode Reset (DECRST)
        if (arg1 == 25) {
            //        P s = 2 5 → Hide Cursor (DECTCEM)
            x11->cur = false;
            printf("Hiding Cursor\n");
        }
        else if (arg1 == 12) {
            // stop cursor blinking
        }
        //else {
        //    eexit(1);
        //}
      } break;
      case 's':  
        buf[len] = 'h';
        __attribute__((fallthrough));
      case 'h': {
        //  CSI ? P m h   DEC Private Mode Set (DECSET)
        if (buf[0] != '?')
            eexit(1);

        char *arg_s = strtok(buf+1, ";");
        while (arg_s) {
            int arg1 = atoi(arg_s);
            arg_s    = strtok(NULL, ";");
            switch (arg1) {
              case 1:
              case 12:
              case 1006:
              case 1002:
              case 5: 
              case 2004: {
                //  P s = 1 → Application Cursor Keys (DECCKM)
                //  P s = 1 2 → Start Blinking Cursor (att610) 
                // 1006,1002 mouse mode shenannigans
                // 5 reverse video?
		// 2004 bracketed paste mode
              } break;
              case 25: {
                //        P s = 2 5 → Show Cursor (DECTCEM)
                x11->cur = true;
                printf("Unhiding Cursor\n");
              } break;
              case 1049: {
                //        P s = 1 0 4 7 → Use Alternate Screen Buffer (unless
                //        disabled by the titeInhibit resource)
                //        P s = 1 0 4 8 → Save cursor as in DECSC (unless
                //        disabled by the titeInhibit resource)
                //        P s = 1 0 4 9 → Save cursor as in DECSC and use
                //        Alternate Screen Buffer,
                //                        clearing it first (unless disabled by
                //                        the titeInhibit resource).
                //                        This combines the effects of the 1047
                //                        and 1048 modes.
                //                        Use this with terminfo-based
                //                        applications rather than the 47 mode.
                switch_buffers(x11);
                clear_all_cells(x11);
                dirty_all_cells(x11);
              } break;
              default:
                eexit(1);
            }
        }
      } break;
      case 'M': {
        int arg1 = 1;
        sscanf(buf, "%dM", &arg1);
        // delete arg1 lines

        for (struct cell *dest   = lstart,
                         *source = dest + x11->buf_w * arg1;
             source < x11->buf + x11->buf_w * (1+x11->scr_end);
             ++source, ++dest)
            copy(dest, source);

        for (struct cell *dest = x11->buf + x11->buf_w * (1+x11->scr_end - arg1);
             dest < x11->buf + x11->buf_w * (1+x11->scr_end);
             ++dest)
            clear(x11, dest);
      } break;
      case 'L': {
        int arg1 = 1;
        sscanf(buf, "%dL", &arg1);
        // insert arg1 lines
        printf("Insert %d lines\n", arg1);
        struct cell *scroll_end = x11->buf + (x11->scr_end + 1) * x11->buf_w;

        for (struct cell *dest   = scroll_end,
                         *source = scroll_end - x11->buf_w * arg1;
             source >= lstart;
             --source, --dest)
            copy(dest, source);

        for (struct cell *dest = lstart, *end = lstart + x11->buf_w * arg1;
             dest < end;
             ++dest) 
            clear(x11, dest);

      } break;
      case 'n': {
        // Device Status Report
        int arg = atoi(buf);
        if (arg == 6) {
          char command[20];
          size_t len;

          len = snprintf(command,
                         sizeof(command),
                         "\e[%d;%dR",
                         x11->buf_x + 1,
                         x11->buf_y + 1);

          int writ = write(pty->master, command, len);
          (void) writ;
        }
        else if (arg == 5) {
          char command[20];
          size_t len;

          len = snprintf(command,
                         sizeof(command),
                         "\e[0n");

          int writ = write(pty->master, command, len);
          (void) writ;
        }
        else {
            eexit(1);
        }
      } break;
      case 't': {
        // IGNORE
        // Window manipulation (from dtterm, as well as extensions). These controls 
        //      may be disabled using the allowWindowOps resource. Valid values for 
        //      the first (and any additional parameters) are:
      } break;      	
      default: {
        eexit(1);
      } break;
    }
}

void process_osi(char *buf, size_t len, struct X11 *x11, struct PTY *pty)
{
    (void)len;
    (void)x11;
    (void)pty;

    for(char*a = buf; *a!= 0;++a) if(!isprint(*a)) *a = '?';

    printf("Osi received '%s'\n", buf);
}

void scroll_up(struct X11 *x11)
{
    size_t w = x11->buf_w;
    for (struct cell *dest   = x11->buf + (w * x11->scr_begin),
                     *source = dest + w;
         source < x11->buf + w * (x11->scr_end + 1);
         ++source, ++dest)
        copy(dest, source);

    for (struct cell *dest = x11->buf + w * (x11->scr_end);
         dest < x11->buf + w * (x11->scr_end + 1);
         ++dest)
        clear(x11, dest);
}

int run(struct PTY *pty, struct X11 *x11)
{
    int    maxfd;
    fd_set active;
    fd_set readable;
    XEvent ev;
    char   _buf[4096];
    char   buf[1];
    bool   just_wrapped     = false;
    bool   add_newline      = false;
    bool   read_escape_mode = false;
    bool   read_csi         = false;
    bool   read_osi         = false;
    bool   read_charset     = false;
    bool   read_utf8        = false;

    char   csi_buf[20];
    size_t csi_buf_i = 0;

    char   osi_buf[200];
    size_t osi_buf_i = 0;

    size_t utf8_size = 0; 
    char utf8_buf[4];
    size_t utf8_idx = 0;

    struct timeval timeout;

    bool draw = false;

    maxfd = pty->master > x11->fd ? pty->master : x11->fd;

    FD_ZERO(&active);
    FD_SET(pty->master, &active);
    FD_SET(x11->fd, &active);
    FD_SET(0, &active);

    for (;;) {
        readable = active;

        timeout.tv_sec  = 0;
        timeout.tv_usec = 1000000;

        int num = select(maxfd + 1, &readable, NULL, NULL, &timeout);
        if (num == 0) {
            x11->blink = !x11->blink;
            x11_redraw(x11);
                // static int col_n = 0; x11->col_bg = x11->col_os[++col_n %
                // col_os_length]; printf("Timeout %lu %d\n", x11->col_bg,
                // col_n);
            continue;
        }
        else if (num == -1) {
            perror("select");
            return 1;
        }

        if (FD_ISSET(pty->master, &readable)) {
            ssize_t num = read(pty->master, _buf, sizeof(_buf));

            if (num == -1) {
                break;
            }

            for (size_t i = 0; i < (size_t)num; i++) {
                buf[0] = _buf[i];

                if (print_child) { 
                    char printbuf[2];

                    if (buf[0] >= 32 && buf[0] <= 126)
                        printbuf[0] = buf[0];
                    else
                        printbuf[0] = '?';

                    printbuf[1] = '\0';
                    printf("Child sent '%s' (%d) (0x%x)\n",
                           printbuf,
                           (int)buf[0],
                           (unsigned char)buf[0]);
                }

                if (read_escape_mode) {
                    read_escape_mode = false;
                    switch (buf[0]) {
                      case '[':
                        read_csi  = true;
                        csi_buf_i = 0;
                        break;
                      case '=':
                        // Application Keypad
                        x11->application_keypad = true;
                        break;
                      case ']':
                        printf("OSI Start\n");
                        read_osi  = true;
                        osi_buf_i = 0;
                        break;
                      case '\\':
                        if (read_osi) {
                            osi_buf[osi_buf_i] = '\0';
                            process_osi(osi_buf, osi_buf_i, x11, pty);
                            draw = true;
                            read_osi = false;
                        }
                        break;
                      case '>': {  //  Normal Keypad (DECPNM)
                        x11->application_keypad = false;
                      } break;
                      case '(': {
                        //  ESC ( C   Designate G0 Character Set (ISO 2022)
                        read_charset = true;
                      } break;
                      case '7': {  // save cursor
                      } break;
                      case 'M': {
                        // move cursor up, if cursor at top, scroll screen
                        if (x11->buf_y==0) {
                            // scroll window content down one row
                            process_csi("L", 0, x11, pty);
                        } else {
                            // move cursor up
                            --x11->buf_y;
                        }
                        draw = true;
                      } break;
                      default:
                        printf("Escape code unknown '%c' (%x)\n",
                               (int)buf[0],
                               (int)0xFF & buf[0]);
                        eexit(1);
                    }
                }
                else if (read_charset) {
                    read_charset = false;
                    switch (buf[0]) {
                      case '0':
                      case 'A':
                      case 'B':
                      case '4':
                      case 'C':
                      case '5':
                      case 'R':
                      case 'Q':
                      case 'K':
                      case 'Y':
                      case 'E':
                      case '6':
                      case 'Z':
                      case 'H':
                      case '7':
                      case '=': {
                      } break;
                    }
                }
                else if (read_csi) {
                    csi_buf[csi_buf_i] = buf[0];
                    csi_buf_i++;
                    if (is_final_csi_byte(buf[0])) {
                        csi_buf[csi_buf_i] = '\0';
                        process_csi(csi_buf, csi_buf_i - 1, x11, pty);
                        read_csi = false;
                        draw = true;
                        just_wrapped = false; 
                    }
                }
                else if (read_osi) {
                    osi_buf[osi_buf_i] = buf[0];
                    osi_buf_i++;
                    if (is_final_osi_byte(buf[0])) {
                        osi_buf[osi_buf_i-1] = '\0';
                        process_osi(osi_buf, osi_buf_i - 1, x11, pty);
                        read_osi = false;
                        draw = true;
                    }
                }
                else if (buf[0] == '\t') {
                    x11->buf_x += 8 - (x11->buf_x&7);
                    draw = true;
                }
                else if (buf[0] == '\r') {
                    /* "Carriage returns" are probably the most simple
                 * "terminal command": They just make the cursor jump
                 * back to the very first column. */
                    x11->buf_x = 0;
                    draw = true;
                }
                else if (buf[0] == (char)0x08) {
                    printf("Backspace\n");
                    draw = true;
                    if (x11->buf_x != 0)
                        x11->buf_x -= 1;
                }
                else if (buf[0] == (char)0x07) {
                    printf("Bell\n");
                }
                else if (buf[0] == (char)27) {
                    read_escape_mode = true;
                }
                else if (buf[0] == '\n') {
                    if (!just_wrapped) { 
                        add_newline = true; 
                        draw = true; 
                    } else {
                        printf("Supressed double newline\n");
                    }
                } else if ( !read_utf8 && (buf[0] & 0x80) != 0) {
                    utf8_idx = 0;
                    read_utf8 = true;
                    utf8_size = 0;

                    // utf8 character
                    if ((buf[0] & 0xE0) == 0xC0)
                        utf8_size = 2;
                    else if ((buf[0] & 0xF0) == 0xE0)
                        utf8_size = 3;
                    else if ((buf[0] & 0xF8) == 0xF0)
                        utf8_size = 4;
                    else { eexit(1); }

                    utf8_buf[utf8_idx++] = buf[0];
                } else if (read_utf8 && utf8_idx < utf8_size-1) {
                    utf8_buf[utf8_idx++] = buf[0];
                } else {
                    wchar_t glyph = buf[0];

                    if (read_utf8) {
                        utf8_buf[utf8_idx++] = buf[0];
                        read_utf8 = false;
                        glyph = utf8_to_utf32(utf8_buf, utf8_size);
                    }

                    if (just_wrapped) {
                        just_wrapped = false;
                        x11->buf_x   = 0;
                        if (x11->buf_y > x11->scr_end) {
                            scroll_up(x11);
                            x11->buf_y = x11->scr_end;
                        } else {
                            ++x11->buf_y;
                        }
                    }

                    read_utf8 = false;
                    putch(x11, glyph);
                    draw = true;
                    x11->buf_x++;

                    if (x11->buf_x >= x11->buf_w) {
                        just_wrapped = true;
                        x11->buf_x = x11->buf_w-1;
                    }
                }

                if (add_newline) {
                    add_newline = false;
                    draw = true;
                    printf("Adding newline\n");
                    x11->buf_x = 0;
                    x11->buf_y++;

                    if (x11->buf_y > x11->scr_end) {
                        scroll_up(x11);
                        x11->buf_y = x11->scr_end;
                    }
                }
            }
            
            if(draw){
                x11->blink = true;
                x11_redraw(x11);
            }
        }

        if (FD_ISSET(x11->fd, &readable)) {
            while (XPending(x11->dpy)) {
                XNextEvent(x11->dpy, &ev);
                switch (ev.type) {
                  case Expose:
                    dirty_all_cells(x11);
                    x11_redraw(x11);
                    break;
                  case KeyPress:
                    x11_key(&ev.xkey, pty, x11);
                    break;
                }
            }
        }

        if (FD_ISSET(0, &readable)) {
            printf("Stdin became readable\n");
            char   buf[1024];
            size_t n;

            n = read(0, buf, sizeof(buf));

            if (n != 0) {
                printf("Stdin read %zu chars\n", n);
                for (size_t i = 0; i < n; i++) {
                    int ignore = write(pty->master, &buf[i], 1);
                    (void)ignore;
                }
            }
            else {
                printf("Stdin closed\n");
                FD_CLR(0, &active);
            }
        }
    }

    return 0;
}
const char *argp_program_version =
  "eduterm 1.0";
const char *argp_program_bug_address =
  "<james_peach01@hotmail.co.uk>";

static char doc[] =
  "Eduterm -- James' extention to the eduterm source";

/* The options we understand. */
static struct argp_option options[] = {
  {"exit-on-unknown",  'e', 0, 0, "Exit on unknown operations", 0},
  {"print-child",  'p', 0, 0, "Print child output", 0},
  { 0 }
};

static error_t
parse_opt(int key, char* arg, struct argp_state *state)
{
  (void) arg;
  (void) state;

  switch(key) {
    case 'e': {
      exit_mode = true;
    } break;
    case 'p': {
      print_child = true;
    } break;
    default:
      return ARGP_ERR_UNKNOWN;
  }
  
  return 0;
}

static struct argp argp = { options, parse_opt, 0, doc, 0, 0, 0 };

int main(int argc, char* argv[])
{
    argp_parse(&argp, argc, argv, 0, 0, 0);

    struct PTY pty;
    struct X11 x11;

    if (!x11_setup(&x11))
        return 1;

    if (!pt_pair(&pty))
        return 1;

    if (!term_set_size(&pty, &x11))
        return 1;

    if (!spawn(&pty))
        return 1;

    return run(&pty, &x11);
}
