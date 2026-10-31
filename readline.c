#include "common.h"
#include "memory.h"
#include "readline.h"
#include "utils.h"
#include <poll.h>
#include <sys/ioctl.h>

//история
static char** hist = NULL; // массив сохранённых строк
static size_t hist_n = 0; // сколько строк в истории
static size_t hist_cap = 0; // ёмкость массива
static void history_add(const char* s){
    if(*s == '\0') return; // пустые строки не запоминаем
    if(hist_n > 0 && strcmp(hist[hist_n - 1], s) == 0) return; // подряд одинаковые - только одна копия
    if(hist_n == hist_cap){ // место кончилось - расширяем вдвое
        hist_cap = hist_cap ? hist_cap * 2 : 64;
        hist = er_realloc(hist, hist_cap * sizeof(char*));
    }
    hist[hist_n++] = er_strdup((char*)s); // копия строки
}
void rl_history_free(void){
    for(size_t i = 0; i < hist_n; i++) free(hist[i]);
    free(hist);
    hist = NULL; hist_n = hist_cap = 0;
}
static bool is_cont(unsigned char c){ return (c & 0xC0) == 0x80; }

static size_t prev_char(const char* s, size_t pos){ // индекс начала предыдущего символа
    if(pos == 0) return 0;
    pos--;
    while(pos > 0 && is_cont((unsigned char)s[pos])) pos--;
    return pos;
}
static size_t next_char(const char* s, size_t len, size_t pos){ // индекс начала следующего символа
    if(pos >= len) return len;
    pos++;
    while(pos < len && is_cont((unsigned char)s[pos])) pos++;
    return pos;
}
static size_t width_of(const char* s, size_t n){ // ширина в колонках (1 символ = 1 колонка)
    size_t w = 0;
    for(size_t i = 0; i < n; i++) if(!is_cont((unsigned char)s[i])) w++;
    return w;
}
static size_t term_cols(void){ // ширина терминала
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}
static void out(const char* s, size_t n){ // write без потери частично записанных данных
    while(n > 0){
        ssize_t w = write(STDOUT_FILENO, s, n);
        if(w < 0){ if(errno == EINTR) continue; return; }
        s += w; n -= (size_t)w;
    }
}

// ---------------- редактируемая строка ----------------
typedef struct {
    char* buf; // содержимое (всегда завершено '\0')
    size_t len; // длина в байтах
    size_t pos; // позиция курсора в байтах
    size_t cap; // ёмкость buf
} Line;

static void line_set(Line* l, const char* s){ // заменить всё содержимое
    size_t n = strlen(s);
    if(n + 1 > l->cap){ l->cap = n + 64; l->buf = er_realloc(l->buf, l->cap); }
    memcpy(l->buf, s, n + 1);
    l->len = l->pos = n;
}

static void line_insert(Line* l, const char* s, size_t n){ // вставить n байт в позицию курсора
    if(l->len + n + 1 > l->cap){
        while(l->len + n + 1 > l->cap) l->cap *= 2;
        l->buf = er_realloc(l->buf, l->cap);
    }
    memmove(l->buf + l->pos + n, l->buf + l->pos, l->len - l->pos + 1); // +1 - сдвигаем и '\0'
    memcpy(l->buf + l->pos, s, n);
    l->len += n; l->pos += n;
}
static void line_erase(Line* l, size_t from, size_t to){ // удалить байты [from, to)
    memmove(l->buf + from, l->buf + to, l->len - to + 1);
    l->len -= to - from;
    if(l->pos > to) l->pos -= to - from;
    else if(l->pos > from) l->pos = from;
}
//перерисовка
// Перерисовывает приглашение и строку целиком с учётом переноса на несколько экранных строк.
// *cur_row - на сколько строк ниже начала приглашения стоит курсор (обновляется).
// final=true: курсор ставится в конец строки (перед Enter), без служебного переноса.
static void redraw(const char* prompt, const Line* l, size_t* cur_row, bool final){
    size_t cols = term_cols();
    char tmp[32];
    if(*cur_row > 0){ // поднимаемся к первой строке приглашения
        int n = snprintf(tmp, sizeof tmp, "\x1b[%zuA", *cur_row);
        out(tmp, (size_t)n);
    }
    out("\r\x1b[J", 4); // в начало строки и стереть всё до конца экрана
    out(prompt, strlen(prompt));
    out(l->buf, l->len);

    size_t pw = width_of(prompt, strlen(prompt));
    size_t total = pw + width_of(l->buf, l->len); // позиция конца строки (в колонках от начала приглашения)
    if(final){ *cur_row = total / cols; return; } // курсор уже в конце
    if(total > 0 && total % cols == 0) out("\n", 1); // строка ровно по ширине: явно переходим на новую строку
    size_t end_row = total / cols;
    size_t target = pw + width_of(l->buf, l->pos); // где должен стоять курсор
    size_t tr = target / cols, tc = target % cols;
    if(end_row > tr){ int n = snprintf(tmp, sizeof tmp, "\x1b[%zuA", end_row - tr); out(tmp, (size_t)n); }
    out("\r", 1);
    if(tc > 0){ int n = snprintf(tmp, sizeof tmp, "\x1b[%zuC", tc); out(tmp, (size_t)n); }
    *cur_row = tr;
}

//чтение клавиш 
static int read_byte(void){ // один байт из stdin; -1 при EOF/ошибке
    unsigned char c;
    for(;;){
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if(r == 1) return c;
        if(r < 0 && errno == EINTR) continue; // прервал сигнал - читаем снова
        return -1;
    }
}

static int read_byte_timeout(int ms){ // то же, но ждём не дольше ms миллисекунд; -2 = ничего не пришло
    struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
    int r;
    do { r = poll(&p, 1, ms); } while(r < 0 && errno == EINTR);
    if(r <= 0) return -2;
    return read_byte();
}

typedef enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DELETE } Key;

// разбор escape-последовательности после ESC: ESC [ A, ESC [ 3 ~, ESC O H и т.д.
static Key read_escape(void){
    int c = read_byte_timeout(50); // одиночный ESC (без продолжения) - игнорируем
    if(c != '[' && c != 'O') return K_NONE;
    int kind = c;
    int num = 0; // первое число параметра, например 3 в ESC[3~
    c = read_byte_timeout(50);
    while(c >= 0 && ((c >= '0' && c <= '9') || c == ';')){ // читаем параметры
        if(c != ';') num = num * 10 + (c - '0');
        c = read_byte_timeout(50);
    }
    (void)kind;
    switch(c){
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case '~':
            if(num == 1 || num == 7) return K_HOME;
            if(num == 4 || num == 8) return K_END;
            if(num == 3) return K_DELETE;
            return K_NONE;
        default: return K_NONE;
    }
}

//основная функция
char* rl_readline(const char* prompt){
    struct termios saved, raw;
    if(tcgetattr(STDIN_FILENO, &saved) < 0){ // не получилось - работаем как обычный getline
        out(prompt, strlen(prompt));
        char* s = NULL; size_t cap = 0;
        if(getline(&s, &cap, stdin) < 0){ free(s); return NULL; }
        cutter(s);
        history_add(s);
        return s;
    }
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG); // без построчного режима, без эха; Ctrl+C/Z приходят как обычные байты
    raw.c_cc[VMIN] = 1; // read ждёт минимум 1 байт
    raw.c_cc[VTIME] = 0; // без таймера
    tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);

    Line l = { .cap = 128 };
    l.buf = er_malloc(l.cap);
    l.buf[0] = '\0';
    char* draft = NULL; // то, что печатали до входа в историю (возвращаем при ↓ до конца)
    size_t hidx = hist_n; // текущая позиция в истории; hist_n = "новая строка"
    size_t cur_row = 0;
    bool eof = false;

    out(prompt, strlen(prompt)); // первое отображение приглашения
    for(;;){
        int c = read_byte();
        if(c < 0){ eof = true; break; } // EOF/ошибка
        if(c == '\n' || c == '\r'){ // Enter
            redraw(prompt, &l, &cur_row, true);
            out("\n", 1);
            break;
        }
        if(c == 4){ // Ctrl+D
            if(l.len == 0){ eof = true; break; } // пустая строка - конец ввода
            if(l.pos < l.len) line_erase(&l, l.pos, next_char(l.buf, l.len, l.pos)); // иначе удаляем символ под курсором
        } else if(c == 3){ // Ctrl+C - бросаем текущую строку
            redraw(prompt, &l, &cur_row, true);
            out("^C\n", 3);
            l.buf[0] = '\0'; l.len = l.pos = 0;
            break;
        } else if(c == 127 || c == 8){ // Backspace
            if(l.pos > 0) line_erase(&l, prev_char(l.buf, l.pos), l.pos);
        } else if(c == 1){ l.pos = 0; // Ctrl+A - в начало
        } else if(c == 5){ l.pos = l.len; // Ctrl+E - в конец
        } else if(c == 21){ line_erase(&l, 0, l.pos); // Ctrl+U - удалить всё до курсора
        } else if(c == 11){ line_erase(&l, l.pos, l.len); // Ctrl+K - удалить всё после курсора
        } else if(c == 23){ // Ctrl+W - удалить слово слева
            size_t p = l.pos;
            while(p > 0 && l.buf[p - 1] == ' ') p--;
            while(p > 0 && l.buf[p - 1] != ' ') p--;
            line_erase(&l, p, l.pos);
        } else if(c == 27){ // ESC - начало escape-последовательности (стрелки и др.)
            Key k = read_escape();
            if(k == K_LEFT) l.pos = prev_char(l.buf, l.pos);
            else if(k == K_RIGHT) l.pos = next_char(l.buf, l.len, l.pos);
            else if(k == K_HOME) l.pos = 0;
            else if(k == K_END) l.pos = l.len;
            else if(k == K_DELETE){ if(l.pos < l.len) line_erase(&l, l.pos, next_char(l.buf, l.len, l.pos)); }
            else if(k == K_UP){
                if(hidx > 0){
                    if(hidx == hist_n){ free(draft); draft = strdup(l.buf); } // запоминаем набранное
                    hidx--;
                    line_set(&l, hist[hidx]);
                }
            } else if(k == K_DOWN){
                if(hidx < hist_n){
                    hidx++;
                    line_set(&l, hidx == hist_n ? (draft ? draft : "") : hist[hidx]);
                }
            }
        } else if(c >= 32){ // обычный символ (в т.ч. байты UTF-8 >= 0x80)
            char ch = (char)c;
            line_insert(&l, &ch, 1);
        } // остальные управляющие символы (Tab, Ctrl+Z и т.д.) игнорируем
        redraw(prompt, &l, &cur_row, false);
    }

    tcsetattr(STDIN_FILENO, TCSADRAIN, &saved); // возвращаем прежние настройки терминала
    free(draft);
    if(eof){ free(l.buf); return NULL; }
    history_add(l.buf);
    return l.buf;
}
