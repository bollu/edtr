#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <iostream>
#include <iterator>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ttydefaults.h>
#include <termios.h>
#include <unistd.h>

static const int NSPACES_PER_TAB = 2;
/*** defines ***/

const char *VERSION = "0.0.1";

enum editorKey {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  PAGE_UP,
  PAGE_DOWN,
};

// https://vt100.net/docs/vt100-ug/chapter3.html#CPR

// control key: maps to 1...26,
// 00011111
// #define CTRL_KEY(k) ((k) & ((1 << 6) - 1))
#define CTRL_KEY(k) ((k)&0x1f)

int clamp(int lo, int val, int hi) {
  return std::min<int>(std::max<int>(lo, val), hi);
}

/*** data ***/

struct erow {
  int size = 0;
  char *chars = nullptr;
  int rsize = 0;
  char *render = nullptr;

  int cxToRx(int cx) const {
    int rx = 0;
    for (int j = 0; j < cx; ++j) {
      if (chars[j] == '\t') {
        rx += NSPACES_PER_TAB - (rx % NSPACES_PER_TAB);
      } else {
        rx++;
      }
    }
    return rx;
  }
  void update() {

    int ntabs = 0;
    for (int j = 0; j < size; ++j) {
      ntabs += chars[j] == '\t';
    }

    free(render);
    render = (char *)malloc(size + ntabs * NSPACES_PER_TAB + 1);
    int ix = 0;
    for (int j = 0; j < size; ++j) {
      if (chars[j] == '\t') {
        render[ix++] = ' ';
        while (ix % NSPACES_PER_TAB != 0) {
          render[ix++] = ' ';
        }
      } else {
        render[ix++] = chars[j];
      }
    }
    render[ix] = '\0';
    rsize = ix;
  }
};

struct editorConfig {
  int cx = 0, cy = 0;
  struct termios orig_termios;
  int screenrows;
  int screencols;
  int rx = 0;
  erow *row;
  int rowoff = 0;
  int coloff = 0;
  int numrows = 0;
  char *filepath = nullptr;
} E;

/*** terminal ***/

void die(const char *s) {

  // clear screen
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  // explain errno before dying with mesasge s.
  perror(s);
  exit(1);
}

void disableRawMode() {

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
    die("tcgetattr");
  };
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  int lflags = 0;
  lflags |= ECHO;   // don't echo back.
  lflags |= ICANON; // read byte at a time.
  lflags |= ISIG;   // no C-c, C-z for me!
  lflags |= IEXTEN; // Disables C-v for waiting for another character to be sent
                    // literally.
  raw.c_lflag &= ~(lflags);

  // | disable flow control. C-s/C-q to stop sending and start sending data.
  int iflags = 0;
  iflags |= (IXON);
  // disable translating C-m (\r) into \n. (Input-Carriage-Return-NewLine)
  iflags |= ICRNL;
  // INPCK enables parity checking, which doesn’t seem to apply to modern
  // terminal emulators. ISTRIP causes the 8th bit of each input byte to be
  // stripped, meaning it will set it to 0. This is probably already turned off.
  // When BRKINT is turned on, a break condition will cause a SIGINT signal to
  // be sent to the program, like pressing Ctrl-C.
  iflags |= BRKINT | INPCK | ISTRIP;
  raw.c_iflag &= ~iflags;

  // disable all output processing of output.
  raw.c_oflag &= ~(OPOST);

  raw.c_cflag |= (CS8); // This specifies eight bits per byte.

  // | min # of bytes to be read before read() return
  raw.c_cc[VMIN] = 0;
  // | max amount of time to wait before read() return.
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  // if we see an escape char, then read.
  if (c == '\x1b') {
    char seq[3];

    // user pressed escape key only!
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    // user pressed escape sequence!
    // ... Oh wow, that's why it's called an ESCAPE sequence.
    if (seq[0] == '[') {

      // [<digit>
      if (seq[1] >= '0' && seq[1] <= '9') {
        // read digit.
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        // [<digit>~
        if (seq[2] == '~') {
          switch (seq[1]) {
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          }
        }
      }
      // translate arrow keys to vim :)
      switch (seq[1]) {
      case 'A':
        return ARROW_UP;
      case 'B':
        return ARROW_DOWN;
      case 'C':
        return ARROW_RIGHT;
      case 'D':
        return ARROW_LEFT;
      }
    };

    return '\x1b';
  } else if (c == CTRL_KEY('d')) {
    return PAGE_DOWN;
  } else if (c == CTRL('u')) {
    return PAGE_UP;
  } else if (c == 'h') {
    return ARROW_LEFT;
  } else if (c == 'j') {
    return ARROW_DOWN;
  } else if (c == 'k') {
    return ARROW_UP;
  } else if (c == 'l') {
    return ARROW_RIGHT;
  } else {
    return c;
  }
}

void getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;
  // n: terminal status | 6: cursor position
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    die(" | unable to read terminal status");
  }

  // Then we can read the reply from the standard input.
  // The reply is an escape sequence!
  // It’s an escape character ( 27 ), followed by a [
  // character, and then the actual response: 24;80R , or similar.
  //(This escape sequence is documented as Cursor Position Report.)
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  buf[i] = '\0';

  assert(buf[0] == '\x1b');
  assert(buf[1] == '[');

  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    die("unable to parse cursor string");
  };

  printf("\r\n&buf[1]: '%s'\r\n", &buf[1]);
}

int getWindowSize(int *rows, int *cols) {

  getCursorPosition(rows, cols);
  printf("\r\nrows: %d | cols: %d\r\n", *rows, *cols);

  struct winsize ws;
  // TIOCGWINSZ: terminal IOctl get window size.
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    return -1;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
  }
  return 0;
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len) {

  E.row = (erow *)realloc(E.row, sizeof(erow) * (E.numrows + 1));
  const int at = E.numrows;
  // TODO: placement-new.
  E.row[at].size = len;
  E.row[at].chars = (char *)malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].update();
  E.numrows++;
}

/*** file i/o ***/
void editorOpen(const char *filename) {
  free(E.filepath);
  E.filepath = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) {
    die("fopen");
  }

  char *line = nullptr;
  size_t linecap = 0; // allocate memory for line read.
  ssize_t linelen = -1;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 &&
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
      linelen--;
    }
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
}

/*** append buffer ***/
struct abuf {
  char *b = nullptr;
  int len = 0;
  abuf() {}

  void appendbuf(const char *s, int slen) {
    this->b = (char *)realloc(b, len + slen);
    assert(this->b && "unable to append string");
    memcpy(this->b + len, s, slen);
    this->len += slen;
  }

  void appendstr(const char *s) { appendbuf(s, strlen(s)); }

  ~abuf() { free(b); }
};

/*** output ***/

void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = E.row[E.cy].cxToRx(E.cx);
  }
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

void editorDrawRows(abuf &ab) {

  // When we print the nal tilde, we then print a
  // "\r\n" like on any other line, but this causes the terminal to scroll in
  // order to make room for a new, blank line. Let’s make the last line an
  // exception when we print our
  // "\r\n" ’s.

  for (int y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;

    if (filerow < E.numrows) {
      int len = clamp(0, E.row[filerow].rsize - E.coloff, E.screencols);

      // int len = E.row[filerow].size;
      // if (len > E.screencols)
      //   len = E.screencols;
      ab.appendbuf(E.row[filerow].render + E.coloff, len);

    } else {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = sprintf(welcome, "Kilo editor -- version %s", VERSION);
        welcomelen = std::min<int>(welcomelen, E.screencols);

        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          ab.appendstr("~");
          padding--;
        }
        while (padding--) {
          ab.appendstr(" ");
        };

        ab.appendbuf(welcome, welcomelen);
      } else {
        ab.appendstr("~");
      }
    }

    // The K command (Erase In Line) erases part of the current line.
    // by default, arg is 0, which erases everything to the right of the
    // cursor.
    ab.appendstr("\x1b[K");
    // always append a space, since we decrement a row from screen rows
    // to make space for status bar.
    // if (y < E.screenrows - 1) {
    ab.appendstr("\r\n");
    // }
  }
}

void editorDrawStatusBar(abuf &ab) {
  // m: select graphic rendition
  // 1: bold
  // 4: underscore
  // 5: bliks
  // 7: inverted colors
  // can select all with [1;4;5;7m
  // 0: clear, default arg.
  ab.appendstr("\x1b[7m");

  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
    E.filepath ? E.filepath : "[No Name]", E.numrows);

  len = std::min<int>(len, E.screencols);
  ab.appendstr(status);

  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    E.cy + 1, E.numrows);


  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      ab.appendstr(rstatus);
      break;
    } else {
       ab.appendstr(" ");
      len++;
    }
  }
  ab.appendstr("\x1b[m");
}

void editorRefreshScreen() {

  editorScroll();
  abuf ab;

  // It’s possible that the cursor might be displayed in the middle of the
  // screen somewhere for a split second while the terminal is drawing to the
  // screen. To make sure that doesn’t happen, let’s hide the cursor before
  // refreshing the screen, and show it again immediately after the refresh
  // finishes.
  ab.appendstr("\x1b[?25l"); // hide cursor

  // EDIT: no need to refresh screen, screen is cleared
  // line by line @ editorDrawRows.
  //
  // EDIT: I am not sure if this extra complexity is worth it!
  //
  // VT100 escapes.
  // \x1b: escape.
  // J: erase in display.
  // [2J: clear entire screen
  // trivia: [0J: clear screen from top to cuursor, [1J: clear screen from
  // cursor to bottom
  //          0 is default arg, so [J: clear screen from cursor to bottom
  // ab.appendstr("\x1b[2J");

  // H: cursor position
  // [<row>;<col>H   (args separated by ;).
  // Default arguments for H is 1, so it's as if we had sent [1;1H
  ab.appendstr("\x1b[H");

  editorDrawRows(ab);
  editorDrawStatusBar(ab);

  // move cursor to correct row;col.
  char buf[32];
  sprintf(buf, "\x1b[%d;%dH", E.cy - E.rowoff + 1, E.rx - E.coloff + 1);
  ab.appendstr(buf);
  // ab.appendstr("\x1b[H"); < now place cursor at right location!

  // show hidden cursor
  ab.appendstr("\x1b[?25h");

  write(STDOUT_FILENO, ab.b, ab.len);
}

/*** input ***/

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
  case ARROW_LEFT:
    if (E.cx != 0) {
      E.cx--;
    } else if (E.cy > 0) {
      // move back line.
      assert(E.cx == 0);
      E.cy--;
      E.cx = E.row[E.cy].size;
    }

    break;
  case ARROW_RIGHT:
    if (row && E.cx < row->size) {
      E.cx++;
    } else if (row && E.cx == row->size) {
      assert(E.cx == row->size);
      E.cy++;
      E.cx = 0;
    }

    break;
  case ARROW_UP:
    if (E.cy != 0) {
      E.cy--;
    }
    break;
  case ARROW_DOWN:
    if (E.cy < E.numrows) {
      E.cy++;
    }
    break;
  case PAGE_DOWN:
    E.cy = std::min<int>(E.cy + E.screenrows / 2, E.numrows);
    break;
  case PAGE_UP:
    E.cy = std::max<int>(E.cy - E.screenrows / 2, 0);
    break;
  }

  // snap to next line.
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorProcessKeypress() {
  int c = editorReadKey();
  switch (c) {

  case CTRL_KEY('q'):
    exit(0);
    break;
  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_RIGHT:
  case ARROW_LEFT:
  case PAGE_DOWN:
  case PAGE_UP:
    editorMoveCursor(c);
    break;
  }
}

/*** init ***/

void initEditor() {
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
    die("getWindowSize");
  };
  static const int STATUS_BAR_HEIGHT = 1;
  E.screenrows -= STATUS_BAR_HEIGHT;
}

int main(int argc, char **argv) {
  enableRawMode();
  initEditor();

  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  };

  return 0;
}
