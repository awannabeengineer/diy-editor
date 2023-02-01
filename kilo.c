/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

// ctrl-q to quit
// this macro mimics what ctrl does in terminal by setting top3 msbs to 0
#define CTRL_KEY(k) ((k)&0x1f)

/*** data ***/

struct editorConfig {
  int screenrows;
  int screencols;
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

char editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }
  return c;
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

/*** append buffer ***/

// dynamic string buffer with append method
struct abuf {
  char *b;
  int len;
};

/*** output ***/

void editorDrawRows() {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    write(STDOUT_FILENO, "~", 1);

    // no new line after last line ~
    if (y < E.screenrows - 1) {
      write(STDOUT_FILENO, "\r\n", 2);
    }
  }
}

// uses VT1oo escape sequances
// https://vt100.net/docs/vt100-ug/chapter3.html
void editorRefreshScreen() {
  // clear entire screen
  write(STDOUT_FILENO, "\x1b[1J]", 4);
  // reposition cursor to top-left
  write(STDOUT_FILENO, "\x1b[H", 3);

  editorDrawRows();

  write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** input ***/

void editorProcessKeypress() {
  char c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  }
}

/*** init ***/

void initEditor() {
  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

int main() {
  enableRawMode();
  initEditor();

  char c;
  // read each character, quit on  q
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
