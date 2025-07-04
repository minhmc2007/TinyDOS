/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

/*** defines ***/

#define MINIEDIT_VERSION "0.0.2"
#define TAB_STOP 8
#define QUIT_TIMES 2

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/*** data ***/

typedef struct erow {
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx, cy;
  int rx;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  int dirty;
  struct termios orig_termios;
  int show_linenums;
  char *last_search;
  int search_direction;
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY; case '3': return DEL_KEY; case '4': return END_KEY; case '5': return PAGE_UP;
            case '6': return PAGE_DOWN; case '7': return HOME_KEY; case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP; case 'B': return ARROW_DOWN; case 'C': return ARROW_RIGHT; case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY; case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) { case 'H': return HOME_KEY; case 'F': return END_KEY; }
    }
    return '\x1b';
  } else {
    return c;
  }
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    return -1;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (TAB_STOP - 1) - (rx % TAB_STOP);
    rx++;
  }
  return rx;
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) { free(row->chars); }

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
  if (E.cy == E.numrows) { editorInsertRow(E.numrows, "", 0); }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline() {
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
  }
  E.cy++;
  E.cx = 0;
}

void editorDelChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;
  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    erow *prev_row = &E.row[E.cy - 1];
    prev_row->chars = realloc(prev_row->chars, prev_row->size + row->size + 1);
    memcpy(&prev_row->chars[prev_row->size], row->chars, row->size);
    prev_row->size += row->size;
    prev_row->chars[prev_row->size] = '\0';
    editorDelRow(E.cy);
    E.cy--;
  }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++) totlen += E.row[j].size + 1;
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
  if (!fp) return;
  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save As: %s (ESC to cancel)", NULL);
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }
  int len;
  char *buf = editorRowsToString(&len);
  FILE *fp = fopen(E.filename, "w");
  if (fp) {
    if (fwrite(buf, 1, len, fp) == len) {
      fclose(fp);
      free(buf);
      E.dirty = 0;
      editorSetStatusMessage("%d bytes written to disk", len);
      return;
    }
    fclose(fp);
  }
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** append buffer ***/

struct abuf { char *b; int len; };
#define ABUF_INIT {NULL, 0}
void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}
void abFree(struct abuf *ab) { free(ab->b); }

/*** output ***/

void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }
  if (E.cy < E.rowoff) E.rowoff = E.cy;
  if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
  if (E.rx < E.coloff) E.coloff = E.rx;
  if (E.rx >= E.coloff + E.screencols) E.coloff = E.rx - E.screencols + 1;
}

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome), "MiniEdit editor -- version %s", MINIEDIT_VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) { abAppend(ab, "~", 1); padding--; }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int line_num_width = 0;
      if (E.show_linenums) {
          char linenum_buf[16];
          line_num_width = snprintf(linenum_buf, sizeof(linenum_buf), "%4d ", filerow + 1);
          abAppend(ab, linenum_buf, line_num_width);
      }
      int len = E.row[filerow].size - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols - line_num_width) len = E.screencols - line_num_width;
      abAppend(ab, &E.row[filerow].chars[E.coloff], len);
    }
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 3);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
  editorScroll();
  struct abuf ab = ABUF_INIT;
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);
  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);
  int line_num_width = E.show_linenums ? 5 : 0;
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1 + line_num_width);
  abAppend(&ab, buf, strlen(buf));
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

// *** THE MISSING FUNCTION IS HERE ***
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';
  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();
    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback) callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback) callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
    if (callback) callback(buf, c);
  }
}

void findCallback(char *query, int key) {
  if (key == '\r' || key == '\x1b') {
    E.search_direction = 1;
    return;
  }
}

void editorFind() {
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;

  char *query = editorPrompt("Search: %s (ESC to cancel)", findCallback);
  if (query == NULL) return;

  if(E.last_search) free(E.last_search);
  E.last_search = strdup(query);

  int current = E.cy;
  int i;
  for (i = 0; i < E.numrows; i++) {
    current += E.search_direction;
    if (current == -1) current = E.numrows - 1;
    else if (current == E.numrows) current = 0;

    erow *row = &E.row[current];
    char *match = strstr(row->chars, query);
    if (match) {
      E.cy = current;
      E.cx = match - row->chars;
      E.rowoff = E.numrows;
      editorSetStatusMessage("Found: '%s'", query);
      free(query);
      return;
    }
  }

  editorSetStatusMessage("Not found: '%s'", query);
  free(query);
  E.cx = saved_cx; E.cy = saved_cy; E.coloff = saved_coloff; E.rowoff = saved_rowoff;
}

void showScreenMessage(const char *title, const char *body[], int num_lines) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "--- %s ---\r\n\r\n", title);
    write(STDOUT_FILENO, buf, len);
    for (int i = 0; i < num_lines; i++) {
        len = snprintf(buf, sizeof(buf), "%s\r\n", body[i]);
        write(STDOUT_FILENO, buf, len);
    }
    len = snprintf(buf, sizeof(buf), "\r\n(Press any key to continue)");
    write(STDOUT_FILENO, buf, len);
    editorReadKey();
    editorRefreshScreen();
}

void executeExternalCommand(char *cmd_line) {
    disableRawMode();
    printf("\x1b[2J\x1b[H");
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
    } else if (pid == 0) {
        char *args[32];
        char *token = strtok(cmd_line, " ");
        int i = 0;
        while(token != NULL && i < 31) {
            args[i++] = token;
            token = strtok(NULL, " ");
        }
        args[i] = NULL;
        execvp(args[0], args);
        fprintf(stderr, "execvp failed: %s\n", strerror(errno));
        exit(127);
    } else {
        int status;
        waitpid(pid, &status, 0);
    }
    printf("\r\n--- Press ENTER to return to editor ---");
    while(getchar() != '\n');
    enableRawMode();
    editorRefreshScreen();
    editorSetStatusMessage("Command finished");
}

void editorCommandPrompt() {
    char *cmd = editorPrompt(":%s", NULL);
    if (!cmd) return;

    if (strcmp(cmd, "help") == 0) {
        const char *help_body[] = {
            "Ctrl-S: Save file", "Ctrl-Q: Quit (without saving)", "Ctrl-F: Find text", "",
            ":help          - Show this help screen", ":about         - Show editor information",
            ":lines=on/off  - Toggle line numbers", ":exec <cmd>    - Execute an external shell command"
        };
        showScreenMessage("MiniEdit Help", help_body, 8);
    } else if (strcmp(cmd, "about") == 0) {
        const char *about_body[] = {
            "MiniEdit (A Minimal Editor for TinyDOS)", "", "Version: " MINIEDIT_VERSION, "Author: minhmc2007"
        };
        showScreenMessage("About MiniEdit", about_body, 4);
    } else if (strcmp(cmd, "lines=on") == 0) {
        E.show_linenums = 1;
        editorSetStatusMessage("Line numbers ON");
    } else if (strcmp(cmd, "lines=off") == 0) {
        E.show_linenums = 0;
        editorSetStatusMessage("Line numbers OFF");
    } else if (strncmp(cmd, "exec ", 5) == 0) {
        executeExternalCommand(cmd + 5);
    } else {
        editorSetStatusMessage("Unknown command: %s", cmd);
    }
    free(cmd);
}


void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  switch (key) {
    case ARROW_LEFT: if (E.cx != 0) { E.cx--; } else if (E.cy > 0) { E.cy--; E.cx = E.row[E.cy].size; } break;
    case ARROW_RIGHT: if (row && E.cx < row->size) { E.cx++; } else if (row && E.cx == row->size) { E.cy++; E.cx = 0; } break;
    case ARROW_UP: if (E.cy != 0) { E.cy--; } break;
    case ARROW_DOWN: if (E.cy < E.numrows) { E.cy++; } break;
  }
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) { E.cx = rowlen; }
}

void editorProcessKeypress() {
  static int quit_times = QUIT_TIMES;
  int c = editorReadKey();
  switch (c) {
    case '\r': editorInsertNewline(); break;
    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4); write(STDOUT_FILENO, "\x1b[H", 3); exit(0);
      break;
    case CTRL_KEY('s'): editorSave(); break;
    case CTRL_KEY('f'): editorFind(); break;
    case ':': editorCommandPrompt(); break;
    case HOME_KEY: E.cx = 0; break;
    case END_KEY: if (E.cy < E.numrows) E.cx = E.row[E.cy].size; break;
    case BACKSPACE: case CTRL_KEY('h'): case DEL_KEY:
      if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;
    case PAGE_UP: case PAGE_DOWN:
      {
        if (c == PAGE_UP) { E.cy = E.rowoff; }
        else if (c == PAGE_DOWN) { E.cy = E.rowoff + E.screenrows - 1; if (E.cy > E.numrows) E.cy = E.numrows; }
        int times = E.screenrows;
        while (times--) editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;
    case ARROW_UP: case ARROW_DOWN: case ARROW_LEFT: case ARROW_RIGHT: editorMoveCursor(c); break;
    case CTRL_KEY('l'): case '\x1b': break;
    default: editorInsertChar(c); break;
  }
  quit_times = QUIT_TIMES;
}

/*** init ***/

void initEditor() {
  E.cx = 0; E.cy = 0; E.rx = 0; E.rowoff = 0; E.coloff = 0;
  E.numrows = 0; E.row = NULL; E.filename = NULL;
  E.statusmsg[0] = '\0'; E.statusmsg_time = 0; E.dirty = 0;
  E.show_linenums = 0;
  E.last_search = NULL;
  E.search_direction = 1;
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) { editorOpen(argv[1]); }
  editorSetStatusMessage("HELP: :help | Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}