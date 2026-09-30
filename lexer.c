#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "lexer.h"
#include "memory.h"
#include "utils.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>  
#include <stdlib.h> 
#include <pwd.h>    // getpwnam
#include <unistd.h> // fork, pipe, dup2, execvp, read, close
#include <sys/wait.h> // waitpid
bool cur_eof(Cursor* c){//является ли текущий символ концом строки
    return (c->position >= c->length);
}
//посмотреть текущий символ
int cur_see(Cursor* c){
    if(cur_eof(c)){
        return EOF;
    } else {// иначе - текущий символ, без сдвига курсора
        return (unsigned char)c->s[c->position];
    }
}
//получить текущий символ
int cur_get(Cursor* c){
    if(cur_eof(c)){
        return EOF;
    } else {
        int symbol = (unsigned char)c->s[c->position];// читаем текущий символ
        c->position++;// сдвигаем курсор на 1 вперёд
        return symbol;// и только потом возвращаем прочитанный символ
    }
}
void skip_spaces(Cursor* c){//скипаю пробелы
    while(!cur_eof(c) && isspace((unsigned char)c->s[c->position])){
        c->position++;
    }
}
bool match_symbol(Cursor* c, char* z){//проверка строки с подстрокой
    size_t len = strlen(z);// длина искомого оператора (например, "&&" -> 2)
    if(c->position + len > c->length) return false;// если оставшейся части строки не хватает по длине - сразу false
    if(strncmp(c->s + c->position, z, len) == 0){// сравниваем len байт от текущей позиции со строкой z
        c->position += len;// совпало - сдвигаем курсор на всю длину найденного оператора
        return true;
    }
    return false;// не совпало - курсор не трогаем
}

//словесный парсер кавычек мы типа прописали, теперь наша задача прописать чтение цифр
//возвращает -1 если что-то не так
int read_number(Cursor* c){
    int value = 0;// накапливаемое числовое значение
    size_t n = 0;// счётчик прочитанных цифр
    while(!cur_eof(c) && isdigit(cur_see(c))){// пока не конец строки и следующий символ - цифра
        value = value*10 + (cur_get(c) - '0');// сдвигаем на разряд и добавляем новую цифру; cur_get съедает символ
        n++;
    }if(!n) return -1; // цифр не было вообще - здесь не было числа
    return value;
}
char* read_command(char* command){//Функция для чтения $()/`....` - команду внутри.
    //разбиваем command на argv, используя свои же примитивы лексера (без popen/sh)
    Cursor cc = { command, 0, strlen(command) };// новый курсор над текстом команды из $()/``
    size_t argc = 0, cap = 8;// счётчик аргументов и ёмкость массива argv
    char** argv = er_malloc(cap * sizeof(char*));// массив указателей на строки-аргументы
    while(1){
        skip_spaces(&cc);
        if(cur_eof(&cc)) break;
        char* w = read_words(&cc);// читаем очередное слово (со всеми кавычками/подстановками)
        if(!w) break;// read_words вернула NULL - слов больше нет
        if(argc + 1 >= cap){// нужно место под новый аргумент + будущий NULL
            cap *= 2;
            argv = er_realloc(argv, cap * sizeof(char*));
        }
        argv[argc++] = w;// кладём слово в массив, увеличиваем счётчик
    }
    argv[argc] = NULL;// execvp требует, чтобы argv заканчивался NULL
    if(argc == 0){// команда оказалась пустой
        free(argv);
        return er_strdup("");
    }
    int p[2];// p[0] - конец для чтения, p[1] - конец для записи
    if(pipe(p) < 0){// создаём канал между будущим потомком и нами
        for(size_t i = 0; i < argc; i++) free(argv[i]);// не удалось - освобождаем всё, что успели выделить
        free(argv);
        return er_strdup("");
    }
    pid_t pid = fork();// создаём процесс-копию
    if(pid < 0){// fork не удался (редкая системная ошибка)
        close(p[0]); close(p[1]);
        for(size_t i = 0; i < argc; i++) free(argv[i]);
        free(argv);
        return er_strdup("");
    }
    if(pid == 0){
        //потомок: перенаправляем свой stdout в записывающий конец пайпа и выполняем команду
        close(p[0]);
        if(dup2(p[1], STDOUT_FILENO) < 0) _exit(127);// подменяем свой stdout на записывающий конец пайпа
        close(p[1]);// исходный дескриптор больше не нужен (есть его копия как stdout)
        execvp(argv[0], argv);// заменяем себя на запущенную программу
        //если execvp вернулся - команда не найдена/не выполнена
        _exit(127);// аварийный выход потомка без сброса унаследованных буферов
    }
    //родитель: читаем вывод из читающего конца пайпа
    close(p[1]);// родителю пишущий конец не нужен (иначе read ниже не увидит EOF)
    for(size_t i = 0; i < argc; i++) free(argv[i]);// argv был нужен только для execvp в потомке
    free(argv);

    size_t capacity = 256, count = 0;
    char* out = er_malloc(capacity);
    char buf[256];// временный буфер для порций чтения
    ssize_t r;
    while((r = read(p[0], buf, sizeof(buf))) > 0){// читаем, пока read возвращает >0 байт
        while(count + (size_t)r + 1 >= capacity){// не хватает места под новую порцию + '\0'
            capacity *= 2;
        }
        out = er_realloc(out, capacity);
        memcpy(out + count, buf, (size_t)r);// дописываем прочитанное в конец out
        count += (size_t)r;
    }
    out[count] = '\0';// терминируем результат
    close(p[0]);// больше пайп не нужен

    int status;
    waitpid(pid, &status, 0); //дожидаемся завершения, чтобы не оставить зомби

    cutter(out); //как правило команды всегда заканчивают вывод переводом на следующую строку
    return out;
}

//функция для получения домашнего каталога(директории), полного пути к ней
char* tilda_koren(char* string){
    if(!string || string[0] != '~') return er_strdup(string);// не тильда - возвращаем копию как есть
    char* slash = strchr(string, '/');// ищем первый '/' после тильды
    char* userpart = NULL;
    size_t userlen = 0;

    if(!slash){// "/" не найден - вся строка после ~ это имя пользователя (или пусто)
        userpart = string + 1;
        userlen = strlen(userpart);
    } else {// "/" найден - имя пользователя между ~ и этим слэшем
        userpart = string + 1;
        userlen = (size_t)(slash - (string + 1));// расстояние между указателями = длина имени
    }

    char* home = NULL;
    if(userlen == 0){// имени нет - речь про текущего пользователя
        home = getenv("HOME");   //переменные окружения операционной системы
        if(!home) home = "";// на случай, если HOME не установлена
    } else {// указано конкретное имя пользователя
        char* copyname = strndup(userpart, userlen);// копия ровно userlen байт + свой '\0'
        struct passwd* pw = getpwnam(copyname);// поиск в системной базе пользователей
        free(copyname);
        if(pw && pw->pw_dir){
            home = pw->pw_dir;// нашли домашнюю директорию указанного пользователя
        } else {
            home = "";// пользователь не найден - заглушка
        }
    }
    if(!slash) return er_strdup(home); // если "/" не было после ~, возвращаем только домашнюю директорию
    // "/" был найден - нужно соединить домашнюю директорию с оставшейся частью пути
    char* r = path_join(home, slash+1);
    return r;
}

//функция для чтения
//'....' - делают все обычным текстом
//"..." - делают все обычным текстом, кроме $VAR, $(...), `...` // \ - экранирование(\"$`)
char* read_words(Cursor* c){
    size_t capacity = 64, count = 0;// растущий буфер под собираемое слово
    char* buffer = er_malloc(capacity);
    bool squote = false, dquote = false, start = true; //в начале слова

    while(!cur_eof(c)){
        int symbol = cur_see(c);// подсматриваем текущий символ, не съедая
        if(squote == false && dquote == false){// вне любых кавычек действуют разделители слова
            if(isspace(symbol)) break; //если не внутри кавычек, то пробел - разделитель слова
            if(strchr("|&;()<>", symbol)) break;// спецсимвол - конец слова
            if(symbol == '#') break;//коммент тоже 
        }
        cur_get(c);// символ проходит дальше - теперь съедаем его по-настоящему
        if(dquote == false && symbol == '\''){// одиночная кавычка (не внутри двойных)
            squote = !squote;// переключаем режим одиночных кавычек
            continue;
        }

        if(squote == false && symbol == '"'){// двойная кавычка (не внутри одиночных)
            dquote = !dquote;
            continue;
        }

        //экранирование
        if(squote == false && symbol == '\\'){// обратный слэш вне одиночных кавычек - экранирование
            if(!cur_eof(c)){
                symbol = cur_get(c);// подменяем symbol на СЛЕДУЮЩИЙ символ, съедая его
            } else break;// слэш в самом конце строки - обрываем слово
        }

        //условие $
        else if (squote == false && symbol == '$'){// раскрытие идёт везде, кроме одиночных кавычек
            //если получаем скобку ( --- $(...)
            if(!cur_eof(c) && cur_see(c) == '('){
                cur_get(c);// съедаем "("
                size_t depth = 1;// счётчик вложенности скобок
                size_t cap = 128, n = 0;
                char* massive = er_malloc(cap);// временный буфер под текст команды
                while(!cur_eof(c) && depth){
                    int x = cur_get(c);
                    if(x == '(') depth++;// новая вложенная "(" - глубже
                    else if(x == ')'){
                        depth--;// выход из одного уровня вложенности
                        if(depth == 0) break;// это была НАША закрывающая скобка
                    }
                    if((n+1) >= cap){
                        cap *=2;
                        massive = er_realloc(massive, cap);
                    }
                    massive[n++] = (char)x;// копим содержимое (включая вложенные скобки как текст)
                }
                massive[n] = '\0';
                if(depth != 0){// цикл прервался из-за конца строки, а не найденной ')'
                    fprintf(stderr, "Неверный синтаксис\n");
                    free(massive);
                    start = false;
                    continue;
                }
                char* read = read_command(massive);// реально запускаем команду и берём её вывод
                free(massive);
                size_t len = strlen(read);
                while(count + len + 1 >= capacity){// растим основной буфер слова при необходимости
                    capacity *= 2;
                    buffer = er_realloc(buffer, capacity);
                }
                memcpy(buffer + count, read, len);// дописываем результат подстановки в слово
                count += len;
                buffer[count] = '\0';
                free(read);
                start = false;
                continue;
            }

            //проверка на неверный синтаксис
            else if (!cur_eof(c) && cur_see(c) == ')'){// "$)" без открывающей "(" - явная ошибка
                fprintf(stderr, "Неверный синтаксис\n");
                cur_get(c);// съедаем лишнюю ")", чтобы не застрять
                start = false;
                continue;
            }

            //если получили условие { --- ${Var}
            else if(!cur_eof(c) && cur_see(c) == '{'){
                cur_get(c);// съедаем "{"
                size_t cap = 128, n = 0;
                char* massive = er_malloc(cap);// буфер под имя переменной
                while(!cur_eof(c) && cur_see(c) != '}'){//собираем всё до закрывающей "}"
                    int x = cur_get(c);
                    if((n+1) >= cap){
                        cap *=2;
                        massive = er_realloc(massive, cap);
                    }
                    massive[n++] = (char)x;
                }
                if (cur_eof(c) || cur_see(c) != '}'){//строка кончилась раньше "}"
                    fprintf(stderr, "Неверный синтаксис\n");
                    free(massive);
                    start = false;
                    continue;
                }
                cur_get(c);// съедаем закрывающую "}"
                massive[n] = '\0';
                char* value = getenv(massive);// получаем значение переменной окружения по имени massive
                free(massive);
                if(!value) value = "";// если переменной нет - подставляем пустую строку
                size_t len = strlen(value);
                while (count + len + 1 >= capacity){// растим буфер слова при необходимости
                    capacity *= 2;
                    buffer = er_realloc(buffer, capacity);// перевыделяем память под новую ёмкость
                }
                memcpy(buffer + count, value, len);// дописываем значение переменной в слово
                count += len;
                buffer[count]='\0';
                start = false;
                continue;
            }

            else if (!cur_eof(c) && cur_see(c) == '}') {// "${" не было, а "}" сразу после "$" - ошибка
                fprintf(stderr, "Неверный синтаксис\n");
                cur_get(c);  // считываем '}', чтобы не застрять
                start = false;
                continue;
            }
            else if(!cur_eof(c) && cur_see(c) == '?'){// $? - код возврата последней команды
                cur_get(c); //съедаем сам '?'
                char status_buf[16];
                snprintf(status_buf, sizeof(status_buf), "%d", last_exit_status);// преобразуем int в строку
                size_t len = strlen(status_buf);
                while(count + len + 1 >= capacity){
                    capacity *= 2;
                    buffer = er_realloc(buffer, capacity);
                }
                memcpy(buffer + count, status_buf, len);// дописываем значение переменной в слово
                count += len;
                buffer[count] = '\0';
                start = false;
                continue;
            }
            //остался случай, когда $var
            else{
                size_t cap = 128, n = 0;
                char* massive = er_malloc(cap);// буфер под имя переменной
                while(!cur_eof(c)){
                    int x = cur_see(c);// подсматриваем текущий символ, не съедая
                    if(isalnum(x) || x == '_'){// буквы, цифры и подчёркивания - допустимые символы в имени переменной
                        cur_get(c);// съедаем символ, так как он допустимый
                    } else break;// "чужой" символ - не трогаем, выходим
                    if((n+1) >= cap){
                        cap *= 2;
                        massive = er_realloc(massive, cap);
                    }
                    massive[n++] = (char)x;
                }
                massive[n] = '\0';
                char* value = getenv(massive);  //допустим getenv ждет нуль-терминированную строку
                free(massive);
                if(!value) value = "";
                size_t len = strlen(value);
                while(count + len + 1 >= capacity){
                    capacity *= 2;
                    buffer = er_realloc(buffer, capacity);
                }
                memcpy(buffer + count, value, len);
                count += len;
                buffer[count] = '\0'; //добавляем на всякий случай, так как все функции ждут "\0" в конце
                start = false;
                continue;
            }
        }
        //`......` - аналог $()
        else if(squote == false && symbol == '`'){// раскрытие идёт везде, кроме одиночных кавычек
            size_t cap = 128, n = 0;
            char* massive = er_malloc(cap);// буфер под текст команды между обратными кавычками
            while(!cur_eof(c) && cur_see(c) != '`'){// собираем всё до второй "`"
                int x = cur_get(c);
                if((n+1) >= cap){
                    cap *= 2;
                    massive = er_realloc(massive, cap);
                }
                massive[n++] = (char)x;
            }
            if (cur_eof(c) || cur_see(c) != '`'){//строка кончилась раньше закрывающей "`"
                fprintf(stderr, "Неверный синтаксис\n");
                free(massive);
                start = false;
                continue;
            }
            cur_get(c);// съедаем закрывающую "`"
            massive[n] = '\0'; // <- этой строки не хватало в оригинале: без неё massive не был
                                //    null-terminated, и read_command читал мусор из кучи
            char* value = read_command(massive);// запускаем команду, получаем её вывод
            free(massive);
            size_t len = strlen(value);// длина вывода команды
            while(count + len + 1 >= capacity){// растим буфер слова при необходимости
                capacity *= 2;
                buffer = er_realloc(buffer, capacity);
            }
            memcpy(buffer + count, value, len);// дописываем вывод команды в слово
            count += len;
            buffer[count] = '\0';
            free(value);
            start = false;
            continue;
        }
        //реализация нашей тильды
        else if(squote == false && dquote == false && start == true && symbol == '~'){// тильда только в начале слова, вне кавычек
            size_t cap = 64, n = 0;
            char* massive = er_malloc(cap);
            massive[n++] = '~'; // кладём саму тильду первым символом вручную
            while(!cur_eof(c)){
                int x = cur_see(c);
                if(isspace(x) || strchr("|&;()<>", x) || x == '#') break;
                cur_get(c);// символ относится к имени после тильды - съедаем
                if((n+2) >= cap){// +2 на всякий случай, чтобы хватило места под '\0'
                    cap *= 2;
                    massive = er_realloc(massive, cap);// перевыделяем память под новую ёмкость
                }
                massive[n++] = (char)x;// копируем символ в буфер
            }
            massive[n] = '\0';// терминируем строку, чтобы tilda_koren мог работать с ней
            char* value = tilda_koren(massive);// раскрываем ~/~user/~/path/~user/path в реальный путь
            free(massive);
            size_t len = strlen(value);
            while(count + len + 1 >= capacity){
                capacity *= 2;
                buffer = er_realloc(buffer, capacity);
            }
            memcpy(buffer + count, value, len);// дописываем раскрытый путь в слово
            count += len;
            buffer[count] = '\0';
            free(value);
            start = false;// тильда уже не в начале слова
            continue;
        }

        if((count + 2) >= capacity){
            capacity *= 2;
            buffer = er_realloc(buffer, capacity);
        }
        buffer[count++] = symbol;// записываем символ в буфер слова
        buffer[count] = '\0';// сразу терминируем на промежуточном шаге
        start = false;// это уже не первый символ слова
    }
    if(count == 0){// за весь проход не собрали ни одного символа
        free(buffer);
        return NULL;
    }
    return buffer;// возвращаем собранное слово (оно уже терминировано '\0')
}

//наконец прописываем сам лексер
void lexer(char* line, TokenVector* tv){
    Cursor cur = {line, 0, strlen(line)};// курсор над всей входной строкой
    while(!cur_eof(&cur)){
        skip_spaces(&cur);
        if(cur_eof(&cur)) break;
        if(cur_see(&cur) == '#') break;

        if(match_symbol(&cur, "&&")){   //если попадание match_symbol как и все другие функции сдвигает каретку(курсор)(указатель) нашей строки
            tv_push(tv, (Token){TOK_AND, NULL, -1});
            continue;
        }

        if(match_symbol(&cur, "||")){
            tv_push(tv, (Token){TOK_OR, NULL, -1});
            continue;
        }

        if(match_symbol(&cur, "<<<")){
            tv_push(tv, (Token){TOK_HERESTR, NULL, -1});  //передает строку на stdin команды echo "smth" | cat - доп pipe
            continue;
        }
        if(match_symbol(&cur, "<<")){// проверяется ПОСЛЕ "<<<" - иначе тройной оператор был бы разбит неверно
            tv_push(tv, (Token){TOK_HEREDOC, NULL, -1});
            continue;
        }
        if(match_symbol(&cur, ">>")){
            tv_push(tv, (Token){TOK_REDIR_OUT_APP, NULL, -1});  //дозапись в конец файла
            continue;
        }
        if(match_symbol(&cur, "&>")){
            tv_push(tv, (Token){TOK_ALL_TO_FILE, NULL, -1});  //И std_out и ошибки std_err в один файл
            continue;
        }
        if(match_symbol(&cur, ">&")){  //дублирует std_out в файловый дескриптор с нужным номером >&N
            int number = read_number(&cur);// пробуем прочитать номер fd сразу после оператора
            if(number >= 0){
                if(number > 2) {
                    fprintf(stderr, "Недопустимый формат дескриптора\n");
                }
                tv_push(tv, (Token){TOK_DUP_OUT, NULL, number});// передаёт std_out в другой файловый дескриптор
                continue;
            }
            tv_push(tv, (Token){TOK_WORD, er_strdup(">&"), -1});  //передаем как слово
            continue;
        }
        if(match_symbol(&cur, "<&")){  //Дублирует std_in // допустим <&3 читаем ввод из другого дескриптора
            int number = read_number(&cur);
            if(number >= 0){
                if(number > 2) {
                    fprintf(stderr, "Недопустимый формат дескриптора\n");
                }
                tv_push(tv, (Token){TOK_DUP_IN, NULL, number});
                continue;
            }
            tv_push(tv, (Token){TOK_WORD, er_strdup("<&"), -1});  //передаем как слово
            continue;
        }
        //однокомандные
        if(match_symbol(&cur, "|")){
            tv_push(tv, (Token){TOK_PIPE, NULL, -1});
            continue;}
        if(match_symbol(&cur, ";")){
            tv_push(tv, (Token){TOK_SEMI, NULL, -1});
            continue;
        }
        if(match_symbol(&cur, "&")){// проверяется ПОСЛЕ "&&" и "&>" - только одиночный "&" доходит сюда
            tv_push(tv, (Token){TOK_BG, NULL, -1});
            continue;
        }
        //отдельный дочерний бaш, со своей отдельной директорией и окружением
        if(match_symbol(&cur, "(")){
            tv_push(tv, (Token){TOK_LPAREN, NULL, -1});
            continue;
        }
        if(match_symbol(&cur, ")")){
            tv_push(tv, (Token){TOK_RPAREN, NULL, -1});
            continue;
        }
        if(match_symbol(&cur, ">")){// проверяется ПОСЛЕ ">>" и "&>" и ">&" - только одиночный ">"
            tv_push(tv, (Token){TOK_REDIR_OUT, NULL, -1});  //перенаправляет стандартный std_out echo smth > file.txt
            continue;
        }
        if(match_symbol(&cur, "<")){// проверяется ПОСЛЕ "<<<", "<<", "<&" - только одиночный "<"
            tv_push(tv, (Token){TOK_REDIR_IN, NULL, -1});  //перенаправляет std_in ws -l < file.txt
            continue;
        }
        size_t save_position = cur.position;//запоминаем позицию на случай отката
        int fd = read_number(&cur);//пробуем прочитать число (потенциальный номер fd)
        if(fd >= 0){
            if(match_symbol(&cur, ">>")){
                if(fd > 2) fprintf(stderr, "Недопустимый файловый дескриптор\n");
                tv_push(tv, (Token){TOK_FD_REDIR_OUT_APP, NULL, fd}); //тоже самое что и N> только дозапись
                continue;
            }
            if(match_symbol(&cur, ">")){
                if(fd > 2) fprintf(stderr, "Недопустимый файловый дескриптор\n");
                tv_push(tv, (Token){TOK_FD_REDIR_OUT, NULL, fd}); // echo privet 1> file.txt перезапишется в файл вместо вывода в консоль
                continue;
            }
            if(match_symbol(&cur, "<")){
                if(fd > 2) fprintf(stderr, "Недопустимый файловый дескриптор\n");
                tv_push(tv, (Token){TOK_FD_REDIR_IN, NULL, fd}); // Ввод с файла в дескриптор допустим sort (0< file.txt) - обычный ввод заменяется чтение из файла
                continue;
            }
            cur.position = save_position;// после числа не нашлось >>/>/< - это было не число-fd, откатываемся
        }
        char* word = read_words(&cur); // пробуем прочитать обычное слово (со всеми подстановками)
        if(word){
            tv_push(tv, (Token){TOK_WORD, word, -1});
            continue;
        }
        if(!cur_eof(&cur)){// слово не получилось прочитать, но строка ещё не кончилась - тупик
            char bad = (char)cur_see(&cur);
            char msg[64];
            snprintf(msg, sizeof(msg), "недопустимый символ '%c'", bad);
            report_syntax_error(msg);// сообщаем об ошибке синтаксиса, но продолжаем разбор строки дальше
            break; //строка целиком отвергается как ошибочная - дальше не токенизируем
        }
    }
    tv_push(tv, (Token){TOK_END, NULL, -1});// добавляем токен конца потока, чтобы парсер знал, что токены закончились
}
