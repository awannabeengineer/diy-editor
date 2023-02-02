// 😀
/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

// ctrl-q to quit
// this macro mimics what ctrl does in terminal by setting top3 msbs to 0
#define CTRL_KEY(k) ((k)&0x1f)

// change editorKey vals to large ints to avoid collision with other characters
enum editorKey {
  ARROW_LEFT = 1000, // <ESQ>[D
  ARROW_RIGHT,       // [C
  ARROW_UP,          // [A
  ARROW_DOWN,        // [B
  DELETE_KEY,        // [3~
  HOME_KEY,          // [1~ [7~ [H OH
  END_KEY,           // [4~ [8~ [F OF
  PAGE_UP,           // [5~
  PAGE_DOWN          // [6~
};

/*** data ***/

// stores a line of text
typedef struct erow {
  int size;
  int rsize; // render size
  char *chars;
  char *render; // render char array
} erow;

struct editorConfig {
  int cx, cy;     // cursor x,y for char array
  int rx;         // render x position
  int rowoff;     // tracks offset for row scroll position
  int coloff;     // tracks offset for column scroll position
  int screenrows; // number of rows on screen
  int screencols; // number of columns on screen
  int numrows;    // number of rows in file
  erow *row;      // array of edtiro rows to hold rows of chars
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
            return DELETE_KEY;
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
}

/*** file i/o ***/

void editorOpen(char *filename) {
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
    // no new line after last line ~
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
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
  int c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;

  case HOME_KEY:
    E.cx = 0;
    break;

  case END_KEY:
    if (E.cy < E.numrows) {
      E.cx = E.row[E.cy].size;
    }
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
  }
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

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  char c;
  // read each character, quit on  q
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}