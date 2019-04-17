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
    bool          dirty;
};

bool equals(struct cell* a, struct cell* b)
{
    if (a == b)
        return true;
    if (a == NULL)
        return false;
    if (a->g != b->g)
        return false;
    if (a->fg != b->fg)
        return false;
    if (a->bg != b->bg)
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

    // oldscool 3/4 bit colors, normal and bright versions
    unsigned long col_os[col_os_length];
    unsigned long col_256[256 /* duh */];
};

void clear(struct X11 *x11, struct cell *c)
{
    struct cell backup = *c;

    c->g     = ' ';
    c->fg    = x11->col_fg;
    c->bg    = x11->col_bg;

    c->dirty = !equals(&backup, c);
}

void dirty(struct X11 *x11, struct cell *c)
{
    c->dirty = true;
}

// does not handle moving cursor or wrapping
void putch(struct X11 *x11, wchar_t g)
{
    struct cell *c = x11->buf + x11->buf_y * x11->buf_w + x11->buf_x;

    struct cell backup = *c;

    c->g     = g;
    c->fg    = x11->sgr_fg_col;
    c->bg    = x11->sgr_bg_col;

    c->dirty = !equals(&backup, c);
}

void clear_cells(struct X11* x11, struct cell* begin, struct cell* end)
{
    for (; begin != end; ++begin)
        clear(x11, begin);
}

void dirty_cells(struct X11* x11, struct cell* begin, struct cell* end)
{
    for (; begin != end; ++begin)
        dirty(x11, begin);
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
        exit(1);
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
      case 3:
        *--target = (char)((ch | byteMark) & byteMask);
        ch >>= 6;
      case 2:
        *--target = (char)((ch | byteMark) & byteMask);
        ch >>= 6;
      case 1:
        *--target = (char)(ch | firstByteMark[len]);
    }

    buf[len] = '\0';

    printf("%s", buf);
}

void x11_key(XKeyEvent *ev, struct PTY *pty)
{
    char   buf[32];
    int    i, num;
    KeySym ksym;

    num      = XLookupString(ev, buf, sizeof(buf) - 1, &ksym, 0);
    buf[num] = 0;

    if (IsTtyFunctionOrSpaceKey(ksym)) {
        // printf("XKeyEvent non character\n");
    }
    else if (IsKeypad(ksym) != '\0') {
        printf("XKeyEvent arrow key\n");
        num    = 3;
        buf[0] = (char)27;
        buf[1] = '[';
        buf[2] = IsKeypad(ksym);
    }
    else {
        // printf("XKeyEvent string = '%s'\n", buf);
        //        fflush(stdout);
        //        write(1, buf, num);
        //        fflush(stdout);
        //        printf("'\n");
    }
    int ignore;
    for (i = 0; i < num; i++)
        ignore = write(pty->master, &buf[i], 1);
    (void)ignore;
}

void x11_redraw(struct X11 *x11)
{
    if (!x11->cur)
        return;

    int     x, y;
    wchar_t buf[1];

//    XSetForeground(x11->dpy, x11->termgc, x11->col_bg);
//    XFillRectangle(x11->dpy, x11->termwin, x11->termgc, 0, 0, x11->w, x11->h);

    for (y = 0; y < x11->buf_h; y++) {
        for (x = 0; x < x11->buf_w; x++) {
            struct cell *c = x11->buf + (y * x11->buf_w + x);

            if (!c->dirty)
                continue;

            buf[0]         = c->g;

            XSetForeground(x11->dpy, x11->termgc, c->bg);

            XFillRectangle(x11->dpy,
                           x11->termwin,
                           x11->termgc,
                           x * x11->font_width,
                           y * x11->font_height,
                           x11->font_width,
                           x11->font_height);

            if (!iscntrl(buf[0])) {
                XSetForeground(x11->dpy, x11->termgc, c->fg);

                XwcDrawString(x11->dpy,
                              x11->termwin,
                              x11->xfontset,
                              x11->termgc,
                              x * x11->font_width,
                              y * x11->font_height + x11->font_yadg,
                              buf,
                              1);
            }

            if (x != x11->buf_x || y != x11->buf_y)
                c->dirty = false;
        }
    }

    if (x11->blink) {
        XSetForeground(x11->dpy, x11->termgc, x11->col_fg);
    }
    else {
        XSetForeground(x11->dpy, x11->termgc, x11->col_bk);
    }

    XFillRectangle(x11->dpy,
                   x11->termwin,
                   x11->termgc,
                   x11->buf_x * x11->font_width,
                   x11->buf_y * x11->font_height,
                   x11->font_width,
                   x11->font_height);

    XFlush(x11->dpy);
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

    XFontSetExtents* ext = XExtentsOfFontSet(x11->xfontset);

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
    x11->buf_h = 25;
    x11->buf_x = x11->buf_alt_x = 0;
    x11->buf_y = x11->buf_alt_y = 0;
    x11->buf   = calloc(x11->buf_w * x11->buf_h * sizeof(x11->buf[0]), 1);
    clear_cells(x11, x11->buf, x11->buf + x11->buf_w * x11->buf_h);
    dirty_cells(x11, x11->buf, x11->buf + x11->buf_w * x11->buf_h);

    if (x11->buf == NULL) {
        perror("calloc");
        return false;
    }
    x11->buf_alt = calloc(x11->buf_w * x11->buf_h * sizeof(x11->buf[0]), 1);
    clear_cells(x11, x11->buf_alt, x11->buf_alt + x11->buf_w * x11->buf_h);
    dirty_cells(x11, x11->buf_alt, x11->buf_alt + x11->buf_w * x11->buf_h);
    if (x11->buf_alt == NULL) {
        perror("calloc");
        return false;
    }

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
    return b >= 0x40 && b <= 0x73;
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
        //break;
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
      case 'P': {
        // Delete characters
        int num = atoi_range(buf, len - 1, 1);
        for (struct cell *source = cursor + num, *dest = cursor;
             source != lend + 1;
             ++source, ++dest)
            copy(dest, source);

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
                    exit(1);
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
                    exit(1);
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
        int arg1;
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
            exit(1);
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
            exit(1);
        }
      } break;
      case 'C': {
        int arg1;
        sscanf(buf, "%dC", &arg1);
        x11->buf_x += arg1;
        x11->buf_x = x11->buf_x < x11->buf_w - 1 ? x11->buf_x : x11->buf_w - 1;
      } break;
      case 'H': {
        int r, c;
        if (len > 1) {
            sscanf(buf, "%d;%dH", &r, &c);
        }
        else {
            r = c = 1;
        }
        x11->buf_x = c - 1;
        x11->buf_y = r - 1;
        x11->buf_x = x11->buf_x < x11->buf_w ? x11->buf_x : x11->buf_w - 1;
        x11->buf_y = x11->buf_y < x11->buf_h ? x11->buf_y : x11->buf_h - 1;
      } break;
      case 'K': {
        int arg1 = atoi_range(buf, len - 1, 0);
        switch (arg1) {
          case 0: {
            for (struct cell *a = cursor; a != lend + 1; ++a) {
                clear(x11, a);
            }
          } break;
          default:
            exit(1);
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
      } break;
      case 'h': {
        //  CSI ? P m h   DEC Private Mode Set (DECSET)
        if (buf[0] != '?')
            exit(1);

        char *arg_s = strtok(buf+1, ";");
        while (arg_s) {
            int arg1 = atoi(arg_s);
            printf("h arg1 %d", arg1);
            arg_s    = strtok(NULL, ";");
            switch (arg1) {
              case 1: case 12: case 1006: case 1002: case 5:{
                //  P s = 1 → Application Cursor Keys (DECCKM)
                //  P s = 1 2 → Start Blinking Cursor (att610) 
                // 1006,1002 mouse mode shenannigans
                // 5 reverse video?
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
                /*
                for (struct cell *cur = x11->buf;
                     cur < x11->buf + x11->buf_w * x11->buf_h;
                     ++cur) {
                    clear(x11, cur);
                }
                */

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
              } break;
              default:
                exit(1);
            }
        }
      } break;
      case 'M': {
        int arg1;
        sscanf(buf, "%dM", &arg1);
        // delete arg1 lines

        for (struct cell *dest   = lstart,
                         *source = dest + x11->buf_w * (arg1 - 1);
             source < x11->buf + x11->buf_w * x11->buf_h;
             ++source, ++dest)
            copy(dest, source);

        for (struct cell *dest = x11->buf + x11->buf_w * (x11->buf_h - arg1);
             dest < x11->buf + x11->buf_w * x11->buf_h;
             ++dest)
            clear(x11, dest);
      } break;
      case 'L': {
        int arg1;
        sscanf(buf, "%dM", &arg1);
        // insert arg1 lines
        struct cell *scroll_end = x11->buf + (x11->scr_end + 1) * x11->buf_w;

        for (struct cell *dest   = scroll_end,
                         *source = dest - x11->buf_w * arg1;
             dest >= lstart;
             --source, --dest)
            copy(dest, source);

        for (struct cell *dest = lstart, *end = lstart + x11->buf_w * arg1;
             dest < end;
             ++dest) {
            clear(x11, dest);
        }
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
            exit(1);
        }
      } break;
      default: {
        exit(1);
      } break;
    }
}

void process_osi(char *buf, size_t len, struct X11 *x11, struct PTY *pty)
{
    (void)len;
    (void)x11;
    (void)pty;

    printf("Osi received '%s'\n", buf);
}

int run(struct PTY *pty, struct X11 *x11)
{
    int    maxfd;
    fd_set active;
    fd_set readable;
    XEvent ev;
    char   buf[1];
    bool   just_wrapped     = false;
    bool   read_escape_mode = false;
    bool   read_csi         = false;
    bool   read_osi         = false;
    bool   read_charset     = false;

    char   csi_buf[20];
    size_t csi_buf_i = 0;

    char   osi_buf[200];
    size_t osi_buf_i = 0;

    struct timeval timeout;

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
            if (read(pty->master, buf, 1) <= 0) {
                /* This is not necessarily an error but also happens
                 * when the child exits normally. */
                fprintf(stderr, "Nothing to read from child: ");
                return 0;
            }
#if 0
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
#endif
            if (read_escape_mode) {
                read_escape_mode = false;
                switch (buf[0]) {
                  case '[':
                    read_csi  = true;
                    csi_buf_i = 0;
                    break;
                  case '=':
                    // Application Keypad
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
                        read_osi = false;
                    }
                    break;
                  case '>': {  //  Normal Keypad (DECPNM)
                  } break;
                  case '(': { 
                    //  ESC ( C   Designate G0 Character Set (ISO 2022)
                    read_charset = true;
                  } break;
                  case '7': { // save cursor
                  } break;
                  default:
                    printf("Escape code unknown '%c' (%x)\n",
                           (int)buf[0],
                           (int)0xFF & buf[0]);
                    exit(1);
                }
            }
            else if (read_charset){
              read_charset = false;
              switch(buf[0]){
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
                }
            }
            else if (read_osi) {
                osi_buf[osi_buf_i] = buf[0];
                osi_buf_i++;
                if (is_final_osi_byte(buf[0])) {
                    osi_buf[osi_buf_i] = '\0';
                    process_osi(osi_buf, osi_buf_i - 1, x11, pty);
                    read_osi = false;
                }
            }
            else if (buf[0] == '\r') {
                /* "Carriage returns" are probably the most simple
                 * "terminal command": They just make the cursor jump
                 * back to the very first column. */
                x11->buf_x = 0;
            }
            else if (buf[0] == (char)0x08) {
                printf("Backspace\n");
                if (x11->buf_x != 0)
                    x11->buf_x -= 1;
            }
            else if (buf[0] == (char)0x07) {
                printf("Bell\n");
            }
            else {
                if (buf[0] == (char)27) {
                    read_escape_mode = true;
                }
                else if (buf[0] != '\n') {
                    wchar_t glyph = buf[0];
                    if ((buf[0] & 0x80) != 0) {
                        // utf8 character
                        size_t n = 1;
                        if ((buf[0] & 0xE0) == 0xC0)
                            n = 1;
                        else if ((buf[0] & 0xF0) == 0xE0)
                            n = 2;
                        else if ((buf[0] & 0xF8) == 0xF0)
                            n = 3;

                        char utf8[n + 1];
                        utf8[0] = buf[0];
                        if (read(pty->master, utf8 + 1, n) <= 0) {
                            /* This is not necessarily an error but also happens
                             * when the child exits normally. */
                            fprintf(stderr, "Nothing to read from child: ");
                            perror(NULL);
                            return 1;
                        }

                        glyph = utf8_to_utf32(utf8, n + 1);
                    }

                    // printf("Glyph received '"); print_utf32(glyph);
                    // printf("'\n");

                    /* If this is a regular byte, store it and advance
                     * the cursor one cell "to the right". This might
                     * actually wrap to the next line, see below. */
                    putch(x11, glyph);
                    x11->buf_x++;

                    if (x11->buf_x >= x11->buf_w) {
                        x11->buf_x = 0;
                        x11->buf_y++;
                        just_wrapped = true;
                    }
                    else
                        just_wrapped = false;
                }
                else if (!just_wrapped) {
                    /* We read a newline and we did *not* implicitly
                     * wrap to the next line with the last byte we read.
                     * This means we must *now* advance to the next
                     * line.
                     *
                     * This is the same behaviour that most other
                     * terminals have: If you print a full line and then
                     * a newline, they "ignore" that newline. (Just
                     * think about it: A full line of text could always
                     * wrap to the next line implicitly, so that
                     * additional newline could cause the cursor to jump
                     * to the next line *again*.) */
                    x11->buf_y++;
                    just_wrapped = false;
                }

                /* We now check if "the next line" is actually outside
                 * of the buffer. If it is, we shift the entire content
                 * one line up and then stay in the very last line.
                 *
                 * After the memmove(), the last line still has the old
                 * content. We must clear it. */
                if (x11->buf_y > x11->scr_end) {
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

                    x11->buf_y = x11->scr_end;
                }
            }

            x11_redraw(x11);
        }

        if (FD_ISSET(x11->fd, &readable)) {
            while (XPending(x11->dpy)) {
                XNextEvent(x11->dpy, &ev);
                switch (ev.type) {
                  case Expose:
                    x11_redraw(x11);
                    break;
                  case KeyPress:
                    x11_key(&ev.xkey, pty);
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

int main()
{
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
