// 😀
/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

// ctrl-q to quit
// this macro mimics what ctrl does in terminal by setting top3 msbs to 0
#define CTRL_KEY(k) ((k)&0x1f)

// change editorKey vals to large ints to avoid collision with other characters
enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000, // <ESQ>[D
  ARROW_RIGHT,       // [C
  ARROW_UP,          // [A
  ARROW_DOWN,        // [B
  DEL_KEY,           // [3~
  HOME_KEY,          // [1~ [7~ [H OH
  END_KEY,           // [4~ [8~ [F OF
  PAGE_UP,           // [5~
  PAGE_DOWN          // [6~
};

// editor modes
typedef enum emode {
  NORMAL_MODE = 0,
  INSERT_MODE,
  VISUAL_MODE,
  COMMAND_MODE,
  SEARCH_MODE
} emode;

const char *stringFromEmode(emode mode) {
  const char *strings[] = {"NORMAL", "INSERT", "VISUAL", "COMMAND", "SEARCH"};
  return strings[mode];
}

/*** data ***/

// stores a line of text
typedef struct erow {
  int size;
  int rsize; // render size
  char *chars;
  char *render; // render char array
} erow;

struct editorConfig {
  int cx, cy;            // cursor x,y for char array
  int rx;                // render x position
  int rowoff;            // tracks offset for row scroll position
  int coloff;            // tracks offset for column scroll position
  int screenrows;        // number of rows on screen
  int screencols;        // number of columns on screen
  int numrows;           // number of rows in file
  erow *row;             // array of edtiro rows to hold rows of chars
  int dirty;             // set if buffer has been modified since last save
  char *filename;        // name of file opened in buffer
  char statusmsg[80];    // hold status bar message
  time_t statusmsg_time; // hold status bar message displayed time
  emode mode;            // hold current mode
  struct termios orig_termios;
};

struct editorConfig E;

// print error and exit with error
void die(const char *s) {
  // clear screen and reposition cursor on exit
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);

/*** terminal ***/

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode);

  // characters won't bew echoed back in the terminal
  // &= ~ is to unset specified bits in(A | B | C ...)
  // ECHO
  // ICANON canonical mode
  // ISIG for sigint and sigstp (terminate and suspedn)
  // IXON for ctrl-s and ctrl-q (stop and resume transmission of data to the
  // IEXTEN for ctrl-v (terminal sends next character literally)
  // terminal) lflag is for etc flags iflag is for input flags
  // ICRNL for ctrl-n (terminal replaces 13 '\r' for 10 '\n'
  // meaning carriage return to newline)
  // this now causes enter key to read as 13 as well
  // terminal also transitions "\n" to "\r\n" for outputs
  // so move to start and then down (typewrtier days i guess)
  // OPOST for output post processing
  // finally bunch more flags
  // these are probably already turned off (old not used)
  // ISTRIP strips 8th bit (sets it to 0) asci only needs 7
  // CS8 sets character size to 8 bits per byte (so our byte is an octet)
  // use cc flags to tell read() to return after certain time
  // without it it waits
  // cc is an array of bytes that holds varoius control characters
  // VMIN sets minimum number of bytes of input needed before read() can return
  // VTIME sets max time (in tenth of second) to wait before read() returns
  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  // TCSAFLUSH discards any unread input before applying changes
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
          case '1':
            return HOME_KEY;
          case '3':
            return DEL_KEY;
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }
    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  // make it a null terminated string
  buf[i] = '\0';

  // parse cursor position
  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // get rows by moving cursor to bottom right
    // 999C -> moves cursor to right by 999
    // 999B -> move cursor down by 999
    // C, B stop cursor from going past the edge of the screen
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
  // how many tabs up row[cx]
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t') {
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    }
    rx++;
  }
  return rx;
}

// fill in render array from char
// substitute for how tabs and control chars should be rendered
void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t')
      tabs++;
  }

  free(row->render);
  row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0)
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

// append row with given string and size
// sizeof(E.row[i].chars will be E.row[i].size + 1
// since we append null at the end of it
void editorAppendRow(char *s, size_t len) {
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

  int at = E.numrows;

  E.row[at].size = len;
  // reading each line calls malloc and out whole file is not in a contigious
  // chunk of memory. but we use abBuffer for editorDrawRows so it will
  // end up in the same place
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  // init render vals
  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows)
    return;

  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

// insert character c at row[at]
void editorRowInsertChar(erow *row, int at, int c) {
  // if index invalid set to end of line
  if (at < 0 || at > row->size)
    at = row->size;
  // 1 for char and 1 because size doesn't include null at end of row->chars
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  // len won't include NULL neither does row size
  // but row->chars has it so add + 1
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

// how would backspace behave on an empty row?
void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size)
    return;
  // we copy the '\0' as well
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

// holds operations that will be called from editorProcessKeypress
/*** editor operations ***/

void editorInsertChar(int c) {
  // insert row if on last line
  if (E.cy == E.numrows) {
    editorAppendRow("", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorDelChar() {
  // passed end of file
  if (E.cy == E.numrows)
    return;
  // start of file
  if (E.cx == 0 && E.cy == 0)
    return;

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    // cursor will be placed at current end of line above
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 &&
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
  // open calls editorAppendRow which dirties buffer
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL)
    return;

  int len;
  char *buf = editorRowsToString(&len);
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) != -1) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** append buffer ***/

// we do this to avoid doing so many writes
// just append to this buffer and write once
// dynamic string buffer with append method
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT                                                              \
  { NULL, 0 }

void abAppend(struct abuf *ab, const char *s, int len) {
  // allocate en
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

/*** output ***/

// scroll if row[cy] is outside viewport
void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
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

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    // get absolute row wrt file start
    // is filerow name misleading?
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      // welcome message, third of way down, only displayed if no input is given
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
                                  "Kilo editor -- version %s", KILO_VERSION);
        if (welcomelen > E.screencols)
          welcomelen = E.screencols;
        // center it
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--)
          abAppend(ab, " ", 1);

        abAppend(ab, welcome, welcomelen);
      } else {
        // append '~' to empty lines below editable area
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0)
        len = 0;
      if (len > E.screencols)
        len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }
    // clear from cursor to end of line
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

// appends status bar chars to abuf
void editorDrawStatusBar(struct abuf *ab) {
  // [7m switches to inverted colors
  abAppend(ab, "\x1b[31;7m", 7);
  char status[80], rstatus[80];
  // show first 20 chars of filename fllowed to number of lines
  // use [No Name] if no file is given
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s %s",
                     E.filename ? E.filename : "[No Name]", E.numrows,
                     E.dirty ? "(modified)" : "", stringFromEmode(E.mode));
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
  if (len > E.screencols)
    len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    // append rstatus once thre is only rlen left in rows
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;

    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  // switch to normal formatting
  abAppend(ab, "\x1b[m", 3);
  // status bar is not the last line, messge line comes after it now
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  // clear from cursor to end of line
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols)
    msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

// uses VT1oo escape sequances
// https://vt100.net/docs/vt100-ug/chapter3.html
// we hide the cursor before paiting and show it again after to avoid a flicker
// (where curosr migh appear in the middle of screen for a split second)
void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;
  // hide cursor
  abAppend(&ab, "\x1b[?25l", 6);

  // clear entire screen replaced with clearing each line as we draw them
  // abAppend(&ab, "\x1b[2J]", 4);

  // reposition cursor to top-left
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  // move cursor on refresh
  char buf[32];
  // add one to x,y since terminal uses 1 indexed values
  // E.cy shows cursor position within file not screen
  // E.cx shows cursor position relative to file line start absolute
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
           (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  // show cursor
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/*** input ***/

void editorMoveCursor(int key) {
  // NULL for last line(since cy can go past file last line)
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
  // Moving left at begening of line moves cursor to end of previous line
  case ARROW_LEFT:
    if (E.cx != 0) {
      E.cx--;
    } else if (E.cy > 0) {
      E.cy--;
      E.cx = E.row[E.cy].size;
    }
    break;

  // moving right at the end of the line moves cursor to start of next line
  case ARROW_RIGHT:
    if (row && E.cx < row->size) {
      // cx can go one past last char horizntally, to insert new char
      E.cx++;
    } else if (row && E.cx == row->size) {
      E.cy++;
      E.cx = 0;
    }
    break;
  case ARROW_UP:
    if (E.cy != 0) {
      E.cy--; // don't go past start of file
    }
    break;
  case ARROW_DOWN:
    // go past bottom of screen but not bottom of file
    // if (E.cy != E.screenrows - 1)
    if (E.cy < E.numrows) {
      // cy can go one past last line, to insert text in new line
      E.cy++;
    }
    break;
  }
  // check if our E.cx is past the eol of new cy
  // shouldnt we first check if cy even changed?
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorProcessKeypress() {
  /* TODO */
  // rewrite without using static
  static int quit_times = KILO_QUIT_TIMES;

  int c = editorReadKey();

  switch (E.mode) {
  case NORMAL_MODE:
    switch (c) {
    case 'i':
      E.mode = INSERT_MODE;
      break;
    case ':':
      E.mode = COMMAND_MODE;
      break;
    case '/':
      E.mode = SEARCH_MODE;
      break;
    case HOME_KEY:
    case '0':
      E.cx = 0;
      break;

    case END_KEY:
    case '$':
      if (E.cy < E.numrows) {
        E.cx = E.row[E.cy].size;
      }
      break;

    case DEL_KEY:
    case 'x':
      if (c == DEL_KEY)
        editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
    case CTRL_KEY('u'):
    case CTRL_KEY('d'): {
      // move crusor to top or bottom of screen
      // this gos past last line
      if (c == PAGE_UP || c == CTRL_KEY('u')) {
        // move to top of screen
        E.cy = E.rowoff;
      } else if (c == PAGE_DOWN || c == CTRL_KEY('d')) {
        E.cy = E.rowoff + E.screenrows - 1;
      }

      // simulate a screen worth of ARROW_UP or ARROW_DOWN
      int times = E.screenrows;
      while (times--) {
        editorMoveCursor(c == (PAGE_UP || CTRL_KEY('u')) ? ARROW_UP : ARROW_DOWN);
      }
    } break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;

    default:
      break;
    }
    break;
  case INSERT_MODE:
    switch (c) {
    case '\r':
      /* TODO */
      break;
    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                               "Press Ctrl-Q %d more times to quit.",
                               quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      E.cx = 0;
      break;

    case END_KEY:
      if (E.cy < E.numrows) {
        E.cx = E.row[E.cy].size;
      }
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY)
        editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN: {
      // move crusor to top or bottom of screen
      // this gos past last line
      if (c == PAGE_UP) {
        // move to top of screen
        E.cy = E.rowoff;
      } else if (c == PAGE_DOWN) {
        E.cy = E.rowoff + E.screenrows - 1;
      }

      // simulate a screen worth of ARROW_UP or ARROW_DOWN
      int times = E.screenrows;
      while (times--) {
        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
    } break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      E.mode = NORMAL_MODE;
      break;

    default:
      editorInsertChar(c);
      break;
    }
    break;
  case VISUAL_MODE:
    break;

  case COMMAND_MODE:
    switch (c) {
    case CTRL_KEY('l'):
    case '\x1b':
      E.mode = NORMAL_MODE;
      break;
    }
    break;

  case SEARCH_MODE:
    switch (c) {
    case CTRL_KEY('l'):
    case '\x1b':
      E.mode = NORMAL_MODE;
      break;
    }
    break;

  default:
    break;
  }

  // reset if any key but CTRL_Q is passed
  quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.mode = NORMAL_MODE;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
  // reserve last two rows for status bar and messge line
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

  // refresh after each key processing
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
