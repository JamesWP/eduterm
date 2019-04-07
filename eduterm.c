#define _XOPEN_SOURCE 600
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <ctype.h>
#include <wchar.h>
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

/* Launching /bin/sh may launch a GNU Bash and that can have nasty side
 * effects. On my system, it clobbers ~/.bash_history because it doesn't
 * respect $HISTSIZE from my ~/.bashrc. That's very annoying. So, launch
 * /bin/dash which does nothing of the sort. */
#define SHELL "/bin/bash"

struct PTY {
    int master, slave;
};

struct X11 {
    int      fd;
    Display *dpy;
    int      screen;
    Window   root;

    Window        termwin;
    GC            termgc;
    unsigned long col_fg, col_bg, col_bk;
    int           w, h;

    XFontStruct *xfont;
    XFontSet     xfontset;
    int          font_width, font_height;

    wchar_t *buf;
    int   buf_w, buf_h;
    int   buf_x, buf_y;
    bool  blink, cur;

    int scr_begin, scr_end;
};

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

wchar_t utf8_to_utf32(char* buf, size_t size)
{
    return L'é';
    if (size == 1)
        return (buf[0] & 0x7F);
    else if (size == 2)
        return (buf[0] & 0x1F) << 6 | (buf[1] & 0x3F);
    else if (size == 3) {
        printf("'%x' '%x' '%x'\n",
               (int)0xFF & buf[0],
               (int)0xFF & buf[1],
               (int)0xFF & buf[2]);
        return (buf[0] & 0x0F) << 12 | (buf[1] & 0x3F) << 6 | (buf[2] & 0x3F);
    }
    else if (size == 4)
        return (buf[0] & 0x07) << 18 | (buf[1] & 0x3F) << 12 |
               (buf[2] & 0x3F) << 6 | (buf[3] & 0x3F);
    else
        exit(1);
}

void x11_key(XKeyEvent *ev, struct PTY *pty)
{
    char   buf[32];
    int    i, num;
    KeySym ksym;

    num      = XLookupString(ev, buf, sizeof(buf) - 1, &ksym, 0);
    buf[num] = 0;

    if (IsTtyFunctionOrSpaceKey(ksym)) {
        printf("XKeyEvent non character\n");
    }
    else if (IsKeypad(ksym) != '\0') {
        printf("XKeyEvent arrow key\n");
        num    = 3;
        buf[0] = (char)27;
        buf[1] = '[';
        buf[2] = IsKeypad(ksym);
    }
    else {
        printf("XKeyEvent string = '%s'\n", buf);
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

    int  x, y;
    wchar_t buf[1];

    XSetForeground(x11->dpy, x11->termgc, x11->col_bg);
    XFillRectangle(x11->dpy, x11->termwin, x11->termgc, 0, 0, x11->w, x11->h);

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

    XSetForeground(x11->dpy, x11->termgc, x11->col_fg);
    for (y = 0; y < x11->buf_h; y++) {
        for (x = 0; x < x11->buf_w; x++) {
            buf[0] = x11->buf[y * x11->buf_w + x];
            if (!iscntrl(buf[0])) {
                XwcDrawString(x11->dpy,
                              x11->termwin,
                              x11->xfontset,
                              x11->termgc,
                              x * x11->font_width,
                              y * x11->font_height + x11->xfont->ascent,
                              buf,
                              1);
           }
        }
    }

    XSync(x11->dpy, False);
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
    x11->cur = true;

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

    char **missing_charsets;
    int    num_missing_charsets;
    char  *default_string;

    x11->xfontset = XCreateFontSet(x11->dpy,
                                   font,
                                   &missing_charsets,
                                   &num_missing_charsets,
                                   &default_string);

    font = "fixed";
    font = "12x24";
    font = "8x16";
    x11->xfont = XLoadQueryFont(x11->dpy, font);
    if (x11->xfont == NULL) {
        fprintf(stderr, "Could not load font\n");
        return false;
    }
    x11->font_width  = XTextWidth(x11->xfont, "m", 1);
    x11->font_height = x11->xfont->ascent + x11->xfont->descent;

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
    x11->col_fg = color.pixel;

    if (!XAllocNamedColor(x11->dpy, cmap, "#444444", &color, &color)) {
        fprintf(stderr, "Could not load blink color\n");
        return false;
    }
    x11->col_bk = color.pixel;

    /* The terminal will have a fixed size of 80x25 cells. This is an
     * arbitrary number. No resizing has been implemented and child
     * processes can't even ask us for the current size (for now).
     *
     * buf_x, buf_y will be the current cursor position. */
    x11->buf_w = 80;
    x11->buf_h = 25;
    x11->buf_x = 0;
    x11->buf_y = 0;
    x11->buf   = calloc(x11->buf_w * x11->buf_h * sizeof(x11->buf[0]), 1);
    if (x11->buf == NULL) {
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
    char  *env[] = {NULL};

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

        putenv("TERM=xterm-256color");

        execle(SHELL, "-" SHELL, NULL, env);
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

int atoi_range(char* buf, size_t len, int def)
{
    int  s = def;
    if (len >= 1) {
        s = atoi(buf);
    }
    return s;
}

void process_csi(char *buf, size_t len, struct X11 *x11, struct PTY *pty)
{
    char op = buf[len];

    printf("Processing CSI '%s' op %c\n", buf, op);

    wchar_t * const lstart = x11->buf + x11->buf_w * x11->buf_y;
    wchar_t * const cursor = lstart + x11->buf_x;
    wchar_t * const lend   = lstart + x11->buf_w - 1;

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
        for (wchar_t *source = lend - num, *dest = lend; source >= cursor;
             --dest, --source)
            *dest = *source;

        for (wchar_t *bend = cursor + num - 1; bend != cursor - 1; --bend)
            *bend = ' ';

      } break;
      case 'P': {
        // Delete characters
        int num = atoi_range(buf, len - 1, 1);
        for (wchar_t *source = cursor + num, *dest = cursor; source != lend + 1;
             ++source, ++dest)
            *dest = *source;

      } break;
      case 'm': {
        // SGR - Select Graphic Rendition
        //int arg1 = atoi_range(buf, len - 1, 0);
        //if (arg1 == 0) {
            // reset
        //}
      } break;
      case 'J': {
        int arg1;
        sscanf(buf, "%dJ", &arg1);
        if (arg1 == 2) {
            for (wchar_t *a = x11->buf; a != x11->buf + x11->buf_w * x11->buf_h;
                 ++a)
                *a = ' ';
            x11->buf_x = 0;
            x11->buf_y = 0;
        }
        else {
            exit(1);
        }
      } break;
      case 'c': {
        if (buf[0] == '>') {
            int  num = 10;
            char send[num];
            send[0] = (char)27;
            send[1] = '[';
            send[2] = '>';
            send[3] = '1';
            send[4] = ';';
            send[5] = '9';
            send[6] = '5';
            send[7] = ';';
            send[8] = '0';
            send[9] = 'c';
            int ignore;
            for (int i = 0; i < num; i++)
                ignore = write(pty->master, &send[i], 1);
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
            printf("H %d %d\n", r, c);
        }
        else {
            r = c = 1;
        }
        x11->buf_x = c - 1;
        x11->buf_y = r - 1;
      } break;
      case 'K': {
        int arg1 = atoi_range(buf, len - 1, 0);
        switch (arg1) {
          case 0: {
            for (wchar_t *a = cursor; a != lend + 1; ++a)
                *a = ' ';
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
        sscanf(buf,"?%dl", &arg1);
        // CSI ? P m l   DEC Private Mode Reset (DECRST)
        if (arg1 == 25) {
            //        P s = 2 5 → Hide Cursor (DECTCEM)
            x11->cur = false;
            printf("Hiding Cursor\n");
        }
      } break;
      case 'h': {
        // CSI ? P m h   DEC Private Mode Set (DECSET) 
        int arg1;
        sscanf(buf,"?%dh", &arg1);
        if (arg1 == 25) {
            //        P s = 2 5 → Show Cursor (DECTCEM)
            x11->cur = true;
            printf("Unhiding Cursor\n");
        }
      } break;
      default: {
        exit(1);
      } break;
    }
}

int run(struct PTY *pty, struct X11 *x11)
{
    int    i, maxfd;
    fd_set active;
    fd_set readable;
    XEvent ev;
    char   buf[1];
    bool   just_wrapped     = false;
    bool   read_escape_mode = false;
    bool   read_csi         = false;

    char   csi_buf[20];
    size_t csi_buf_i = 0;

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
            //printf("Timeout\n");
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
                perror(NULL);
                return 1;
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
                  default:
                    printf("Escape code unknown\n");
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
                    /* If this is a regular byte, store it and advance
                     * the cursor one cell "to the right". This might
                     * actually wrap to the next line, see below. */
                    x11->buf[x11->buf_y * x11->buf_w + x11->buf_x] = glyph;
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
                    memmove(x11->buf + x11->scr_begin * x11->buf_w,
                            x11->buf + (1 + x11->scr_begin) * x11->buf_w,
                            x11->buf_w * (x11->scr_end - x11->scr_begin));
                    x11->buf_y = x11->scr_end;

                    for (i = 0; i < x11->buf_w; i++)
                        x11->buf[x11->buf_y * x11->buf_w + i] = ' ';
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
