
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#define CASH_VERSION "0.1.0"
#define CASH_MAX_ARGS 4096

/* =========================================================
 * Tokenizer
 * ========================================================= */

typedef enum {
    TOK_WORD,
    TOK_SEMI,
    TOK_NEWLINE,
    TOK_PIPE,
    TOK_AND,
    TOK_OR,
    TOK_BACKGROUND,
    TOK_INPUT,
    TOK_OUTPUT,
    TOK_APPEND,
    TOK_ERROR_OUTPUT,
    TOK_ERROR_APPEND,
    TOK_ERROR_TO_OUTPUT,
    TOK_END
} TokenType;

typedef struct {
    TokenType type;
    char *text;
} Token;

typedef struct {
    Token *items;
    size_t count;
    size_t capacity;
} TokenList;

static int cash_status = 0;
static bool cash_running = true;

/* =========================================================
 * Memory management
 * ========================================================= */

static void *cash_malloc(size_t size)
{
    void *ptr = malloc(size ? size : 1);

    if (!ptr) {
        perror("cash: malloc");
        exit(EXIT_FAILURE);
    }

    return ptr;
}

static void *cash_realloc(void *ptr, size_t size)
{
    void *result = realloc(ptr, size ? size : 1);

    if (!result) {
        perror("cash: realloc");
        exit(EXIT_FAILURE);
    }

    return result;
}

static char *cash_strdup(const char *str)
{
    char *result = strdup(str);

    if (!result) {
        perror("cash: strdup");
        exit(EXIT_FAILURE);
    }

    return result;
}

static void buffer_append(char **buffer, size_t *length,
                          size_t *capacity, char c)
{
    if (*length + 2 > *capacity) {
        *capacity = *capacity ? *capacity * 2 : 64;
        *buffer = cash_realloc(*buffer, *capacity);
    }

    (*buffer)[(*length)++] = c;
    (*buffer)[*length] = '\0';
}

static void buffer_append_string(char **buffer, size_t *length,
                                 size_t *capacity, const char *str)
{
    while (*str)
        buffer_append(buffer, length, capacity, *str++);
}

/* =========================================================
 * Variable expansion
 * ========================================================= */

static char *expand_variable(const char *name)
{
    if (strcmp(name, "?") == 0) {
        char value[32];
        snprintf(value, sizeof(value), "%d", cash_status);
        return cash_strdup(value);
    }

    if (strcmp(name, "$") == 0) {
        char value[32];
        snprintf(value, sizeof(value), "%ld", (long)getpid());
        return cash_strdup(value);
    }

    const char *value = getenv(name);
    return cash_strdup(value ? value : "");
}

static void expand_dollar(const char *input, size_t *position,
                          char **buffer, size_t *length,
                          size_t *capacity)
{
    size_t i = *position + 1;

    if (input[i] == '{') {
        size_t start = ++i;

        while (input[i] && input[i] != '}')
            i++;

        if (!input[i]) {
            buffer_append(buffer, length, capacity, '$');
            return;
        }

        char *name = strndup(input + start, i - start);

        if (!name) {
            perror("cash: strndup");
            exit(EXIT_FAILURE);
        }

        char *value = expand_variable(name);

        buffer_append_string(buffer, length, capacity, value);

        free(name);
        free(value);

        *position = i;
        return;
    }

    if (input[i] == '?' || input[i] == '$') {
        char name[2] = { input[i], '\0' };
        char *value = expand_variable(name);

        buffer_append_string(buffer, length, capacity, value);

        free(value);
        *position = i;
        return;
    }

    if (!isalpha((unsigned char)input[i]) &&
        input[i] != '_' &&
        !isdigit((unsigned char)input[i])) {
        buffer_append(buffer, length, capacity, '$');
        return;
    }

    size_t start = i;

    while (isalnum((unsigned char)input[i]) || input[i] == '_')
        i++;

    char *name = strndup(input + start, i - start);

    if (!name) {
        perror("cash: strndup");
        exit(EXIT_FAILURE);
    }

    char *value = expand_variable(name);

    buffer_append_string(buffer, length, capacity, value);

    free(name);
    free(value);

    *position = i - 1;
}

/* =========================================================
 * Tokenizer
 * ========================================================= */

static void token_add(TokenList *list, TokenType type,
                      const char *text)
{
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 64;

        list->items = cash_realloc(
            list->items,
            list->capacity * sizeof(Token)
        );
    }

    list->items[list->count].type = type;
    list->items[list->count].text = cash_strdup(text);
    list->count++;
}

static bool is_operator(char c)
{
    return c == ';' || c == '\n' || c == '|' ||
           c == '&' || c == '<' || c == '>';
}

static int tokenize(const char *input, TokenList *list)
{
    size_t i = 0;

    while (input[i]) {
        char c = input[i];

        if (c == ' ' || c == '\t' || c == '\r') {
            i++;
            continue;
        }

        if (c == '#') {
            while (input[i] && input[i] != '\n')
                i++;

            continue;
        }

        if (c == '\n') {
            token_add(list, TOK_NEWLINE, "\n");
            i++;
            continue;
        }

        if (c == ';') {
            token_add(list, TOK_SEMI, ";");
            i++;
            continue;
        }

        if (c == '|' && input[i + 1] == '|') {
            token_add(list, TOK_OR, "||");
            i += 2;
            continue;
        }

        if (c == '|' ) {
            token_add(list, TOK_PIPE, "|");
            i++;
            continue;
        }

        if (c == '&' && input[i + 1] == '&') {
            token_add(list, TOK_AND, "&&");
            i += 2;
            continue;
        }

        if (c == '&') {
            token_add(list, TOK_BACKGROUND, "&");
            i++;
            continue;
        }

        if (c == '2' && input[i + 1] == '>' &&
            input[i + 2] == '&' && input[i + 3] == '1') {
            token_add(list, TOK_ERROR_TO_OUTPUT, "2>&1");
            i += 4;
            continue;
        }

        if (c == '2' && input[i + 1] == '>') {
            if (input[i + 2] == '>') {
                token_add(list, TOK_ERROR_APPEND, "2>>");
                i += 3;
            } else {
                token_add(list, TOK_ERROR_OUTPUT, "2>");
                i += 2;
            }

            continue;
        }

        if (c == '<') {
            token_add(list, TOK_INPUT, "<");
            i++;
            continue;
        }

        if (c == '>') {
            if (input[i + 1] == '>') {
                token_add(list, TOK_APPEND, ">>");
                i += 2;
            } else {
                token_add(list, TOK_OUTPUT, ">");
                i++;
            }

            continue;
        }

        char *word = NULL;
        size_t length = 0;
        size_t capacity = 0;

        bool started = false;
        char quote = 0;

        while (input[i]) {
            c = input[i];

            if (!quote && (isspace((unsigned char)c) ||
                           is_operator(c)))
                break;

            if (!quote && (c == '\'' || c == '"')) {
                quote = c;
                started = true;
                i++;
                continue;
            }

            if (quote && c == quote) {
                quote = 0;
                i++;
                continue;
            }

            if (c == '\\' && quote != '\'') {
                started = true;
                i++;

                if (input[i]) {
                    buffer_append(&word, &length, &capacity, input[i]);
                    i++;
                }

                continue;
            }

            if (c == '$' && quote != '\'') {
                started = true;
                expand_dollar(input, &i, &word, &length, &capacity);
                i++;
                continue;
            }

            started = true;
            buffer_append(&word, &length, &capacity, c);
            i++;
        }

        if (quote) {
            fprintf(stderr, "cash: unmatched quote\n");
            free(word);
            return -1;
        }

        if (started) {
            if (!word)
                word = cash_strdup("");

            token_add(list, TOK_WORD, word);
        }

        free(word);
    }

    token_add(list, TOK_END, "");
    return 0;
}

static void token_list_free(TokenList *list)
{
    for (size_t i = 0; i < list->count; i++)
        free(list->items[i].text);

    free(list->items);
    memset(list, 0, sizeof(*list));
}

/* =========================================================
 * Abstract syntax structures
 * ========================================================= */

typedef struct {
    char **argv;
    size_t argc;
    size_t capacity;

    char **redirs;
    int *redir_types;
    size_t redir_count;
    size_t redir_capacity;
} Command;

typedef struct {
    Command **commands;
    size_t count;
} Pipeline;

typedef struct {
    Pipeline **pipelines;
    TokenType *operators;
    size_t count;
} AndOr;

typedef struct {
    AndOr **items;
    bool *background;
    size_t count;
} Program;

typedef struct {
    TokenList *tokens;
    size_t position;
    bool error;
} Parser;

/* =========================================================
 * Parser helpers
 * ========================================================= */

static Token *peek(Parser *parser)
{
    return &parser->tokens->items[parser->position];
}

static Token *consume(Parser *parser)
{
    return &parser->tokens->items[parser->position++];
}

static bool accept(Parser *parser, TokenType type)
{
    if (peek(parser)->type != type)
        return false;

    consume(parser);
    return true;
}

static void parser_error(Parser *parser, const char *message)
{
    fprintf(stderr, "cash: syntax error: %s\n", message);
    parser->error = true;
}

/* =========================================================
 * Command parser
 * ========================================================= */

static Command *command_new(void)
{
    Command *cmd = cash_malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    return cmd;
}

static void command_add_arg(Command *cmd, const char *arg)
{
    if (cmd->argc >= CASH_MAX_ARGS) {
        fprintf(stderr, "cash: too many arguments\n");
        return;
    }

    if (cmd->argc + 1 >= cmd->capacity) {
        cmd->capacity = cmd->capacity ? cmd->capacity * 2 : 8;

        cmd->argv = cash_realloc(
            cmd->argv,
            cmd->capacity * sizeof(char *)
        );
    }

    cmd->argv[cmd->argc++] = cash_strdup(arg);
    cmd->argv[cmd->argc] = NULL;
}

static void command_add_redir(Command *cmd, int type,
                              const char *path)
{
    if (cmd->redir_count == cmd->redir_capacity) {
        cmd->redir_capacity = cmd->redir_capacity ?
                              cmd->redir_capacity * 2 : 4;

        cmd->redirs = cash_realloc(
            cmd->redirs,
            cmd->redir_capacity * sizeof(char *)
        );

        cmd->redir_types = cash_realloc(
            cmd->redir_types,
            cmd->redir_capacity * sizeof(int)
        );
    }

    cmd->redirs[cmd->redir_count] = cash_strdup(path);
    cmd->redir_types[cmd->redir_count] = type;
    cmd->redir_count++;
}

static void command_free(Command *cmd)
{
    if (!cmd)
        return;

    for (size_t i = 0; i < cmd->argc; i++)
        free(cmd->argv[i]);

    for (size_t i = 0; i < cmd->redir_count; i++)
        free(cmd->redirs[i]);

    free(cmd->argv);
    free(cmd->redirs);
    free(cmd->redir_types);
    free(cmd);
}

static Pipeline *pipeline_new(void)
{
    Pipeline *pipeline = cash_malloc(sizeof(*pipeline));
    memset(pipeline, 0, sizeof(*pipeline));
    return pipeline;
}

static void pipeline_add(Pipeline *pipeline, Command *cmd)
{
    pipeline->commands = cash_realloc(
        pipeline->commands,
        (pipeline->count + 1) * sizeof(Command *)
    );

    pipeline->commands[pipeline->count++] = cmd;
}

static void pipeline_free(Pipeline *pipeline)
{
    if (!pipeline)
        return;

    for (size_t i = 0; i < pipeline->count; i++)
        command_free(pipeline->commands[i]);

    free(pipeline->commands);
    free(pipeline);
}

static bool is_redirection(TokenType type)
{
    return type == TOK_INPUT ||
           type == TOK_OUTPUT ||
           type == TOK_APPEND ||
           type == TOK_ERROR_OUTPUT ||
           type == TOK_ERROR_APPEND ||
           type == TOK_ERROR_TO_OUTPUT;
}

static Pipeline *parse_pipeline(Parser *parser)
{
    Pipeline *pipeline = pipeline_new();

    for (;;) {
        Command *cmd = command_new();

        while (peek(parser)->type == TOK_WORD ||
               is_redirection(peek(parser)->type)) {

            TokenType type = peek(parser)->type;

            if (type == TOK_WORD) {
                command_add_arg(cmd, consume(parser)->text);
                continue;
            }

            consume(parser);

            if (type == TOK_ERROR_TO_OUTPUT) {
                command_add_redir(cmd, 5, "");
                continue;
            }

            if (peek(parser)->type != TOK_WORD) {
                parser_error(parser, "expected filename after redirection");
                command_free(cmd);
                pipeline_free(pipeline);
                return NULL;
            }

            char *path = consume(parser)->text;
            int redir_type = 0;

            switch (type) {
                case TOK_INPUT:         redir_type = 0; break;
                case TOK_OUTPUT:        redir_type = 1; break;
                case TOK_APPEND:        redir_type = 2; break;
                case TOK_ERROR_OUTPUT:  redir_type = 3; break;
                case TOK_ERROR_APPEND:  redir_type = 4; break;
                default: break;
            }

            command_add_redir(cmd, redir_type, path);
        }

        if (!cmd->argc && !cmd->redir_count) {
            command_free(cmd);

            if (!pipeline->count) {
                pipeline_free(pipeline);
                return NULL;
            }

            break;
        }

        pipeline_add(pipeline, cmd);

        if (!accept(parser, TOK_PIPE))
            break;

        if (peek(parser)->type == TOK_END ||
            peek(parser)->type == TOK_SEMI ||
            peek(parser)->type == TOK_NEWLINE ||
            peek(parser)->type == TOK_AND ||
            peek(parser)->type == TOK_OR) {
            parser_error(parser, "expected command after pipe");
            pipeline_free(pipeline);
            return NULL;
        }
    }

    return pipeline;
}

/* =========================================================
 * Logical expression parser
 * ========================================================= */

static AndOr *andor_new(void)
{
    AndOr *node = cash_malloc(sizeof(*node));
    memset(node, 0, sizeof(*node));
    return node;
}

static void andor_add(AndOr *node, Pipeline *pipeline,
                      TokenType op)
{
    node->pipelines = cash_realloc(
        node->pipelines,
        (node->count + 1) * sizeof(Pipeline *)
    );

    node->operators = cash_realloc(
        node->operators,
        (node->count + 1) * sizeof(TokenType)
    );

    node->pipelines[node->count] = pipeline;
    node->operators[node->count] = op;
    node->count++;
}

static void andor_free(AndOr *node)
{
    if (!node)
        return;

    for (size_t i = 0; i < node->count; i++)
        pipeline_free(node->pipelines[i]);

    free(node->pipelines);
    free(node->operators);
    free(node);
}

static AndOr *parse_andor(Parser *parser)
{
    AndOr *node = andor_new();

    Pipeline *first = parse_pipeline(parser);

    if (!first) {
        andor_free(node);
        return NULL;
    }

    andor_add(node, first, TOK_END);

    while (peek(parser)->type == TOK_AND ||
           peek(parser)->type == TOK_OR) {

        TokenType op = consume(parser)->type;
        Pipeline *next = parse_pipeline(parser);

        if (!next) {
            parser_error(parser, "expected command after logical operator");
            andor_free(node);
            return NULL;
        }

        andor_add(node, next, op);
    }

    return node;
}

/* =========================================================
 * Program parser
 * ========================================================= */

static Program *program_new(void)
{
    Program *program = cash_malloc(sizeof(*program));
    memset(program, 0, sizeof(*program));
    return program;
}

static void program_add(Program *program, AndOr *node,
                        bool background)
{
    program->items = cash_realloc(
        program->items,
        (program->count + 1) * sizeof(AndOr *)
    );

    program->background = cash_realloc(
        program->background,
        (program->count + 1) * sizeof(bool)
    );

    program->items[program->count] = node;
    program->background[program->count] = background;
    program->count++;
}

static void program_free(Program *program)
{
    if (!program)
        return;

    for (size_t i = 0; i < program->count; i++)
        andor_free(program->items[i]);

    free(program->items);
    free(program->background);
    free(program);
}

static Program *parse_program(Parser *parser)
{
    Program *program = program_new();

    while (peek(parser)->type != TOK_END && !parser->error) {
        while (accept(parser, TOK_SEMI) ||
               accept(parser, TOK_NEWLINE))
            ;

        if (peek(parser)->type == TOK_END)
            break;

        AndOr *node = parse_andor(parser);

        if (!node) {
            if (!parser->error)
                parser_error(parser, "expected command");

            break;
        }

        bool background = accept(parser, TOK_BACKGROUND);
        program_add(program, node, background);

        if (peek(parser)->type == TOK_SEMI ||
            peek(parser)->type == TOK_NEWLINE) {

            while (accept(parser, TOK_SEMI) ||
                   accept(parser, TOK_NEWLINE))
                ;
        } else if (peek(parser)->type != TOK_END) {
            parser_error(parser, "unexpected token");
            break;
        }
    }

    if (parser->error) {
        program_free(program);
        return NULL;
    }

    return program;
}

/* =========================================================
 * Redirection execution
 * ========================================================= */

static int apply_redirections(Command *cmd)
{
    for (size_t i = 0; i < cmd->redir_count; i++) {
        int type = cmd->redir_types[i];

        if (type == 5) {
            if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
                perror("cash: dup2");
                return -1;
            }

            continue;
        }

        const char *path = cmd->redirs[i];
        int fd = -1;
        int target = STDIN_FILENO;

        switch (type) {
            case 0:
                fd = open(path, O_RDONLY);
                target = STDIN_FILENO;
                break;

            case 1:
                fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                target = STDOUT_FILENO;
                break;

            case 2:
                fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
                target = STDOUT_FILENO;
                break;

            case 3:
                fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                target = STDERR_FILENO;
                break;

            case 4:
                fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
                target = STDERR_FILENO;
                break;
        }

        if (fd < 0) {
            fprintf(stderr, "cash: %s: %s\n",
                    path, strerror(errno));
            return -1;
        }

        if (dup2(fd, target) < 0) {
            perror("cash: dup2");
            close(fd);
            return -1;
        }

        close(fd);
    }

    return 0;
}

/* =========================================================
 * Shell commands
 *
 * These are the ONLY built-in commands:
 *     cd
 *     exit
 * ========================================================= */

static bool is_shell_command(const char *name)
{
    return name &&
           (strcmp(name, "cd") == 0 ||
            strcmp(name, "exit") == 0);
}

static int run_shell_command(Command *cmd)
{
    if (!cmd->argc)
        return 0;

    if (strcmp(cmd->argv[0], "cd") == 0) {
        const char *path;

        if (cmd->argc > 2) {
            fprintf(stderr, "cash: cd: too many arguments\n");
            return 1;
        }

        path = cmd->argc == 2 ? cmd->argv[1] : getenv("HOME");

        if (!path) {
            fprintf(stderr, "cash: cd: HOME is not set\n");
            return 1;
        }

        if (chdir(path) < 0) {
            fprintf(stderr, "cash: cd: %s: %s\n",
                    path, strerror(errno));
            return 1;
        }

        return 0;
    }

    if (strcmp(cmd->argv[0], "exit") == 0) {
        if (cmd->argc > 2) {
            fprintf(stderr, "cash: exit: too many arguments\n");
            return 1;
        }

        if (cmd->argc == 2) {
            char *end = NULL;
            errno = 0;

            long value = strtol(cmd->argv[1], &end, 10);

            if (errno || !end || *end) {
                fprintf(stderr, "cash: exit: numeric argument required\n");
                cash_running = false;
                return 2;
            }

            cash_status = (unsigned char)value;
        }

        cash_running = false;
        return cash_status;
    }

    return 127;
}

/* =========================================================
 * Process execution
 * ========================================================= */

static int wait_for(pid_t pid)
{
    int status;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;

        perror("cash: waitpid");
        return 1;
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return 1;
}

static int execute_external(Command *cmd)
{
    if (!cmd->argc)
        return 0;

    execvp(cmd->argv[0], cmd->argv);

    fprintf(stderr, "cash: %s: %s\n",
            cmd->argv[0], strerror(errno));

    return errno == ENOENT ? 127 : 126;
}

static int execute_child_command(Command *cmd)
{
    if (apply_redirections(cmd) < 0)
        return 1;

    if (!cmd->argc)
        return 0;

    if (is_shell_command(cmd->argv[0]))
        return run_shell_command(cmd);

    return execute_external(cmd);
}

static int execute_single(Command *cmd)
{
    if (cmd->argc && is_shell_command(cmd->argv[0])) {
        int saved_stdin = dup(STDIN_FILENO);
        int saved_stdout = dup(STDOUT_FILENO);
        int saved_stderr = dup(STDERR_FILENO);

        if (saved_stdin < 0 || saved_stdout < 0 || saved_stderr < 0) {
            perror("cash: dup");
            return 1;
        }

        if (apply_redirections(cmd) < 0) {
            dup2(saved_stdin, STDIN_FILENO);
            dup2(saved_stdout, STDOUT_FILENO);
            dup2(saved_stderr, STDERR_FILENO);

            close(saved_stdin);
            close(saved_stdout);
            close(saved_stderr);
            return 1;
        }

        int result = run_shell_command(cmd);

        fflush(NULL);

        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);

        close(saved_stdin);
        close(saved_stdout);
        close(saved_stderr);

        return result;
    }

    if (!cmd->argc && cmd->redir_count) {
        int saved_stdin = dup(STDIN_FILENO);
        int saved_stdout = dup(STDOUT_FILENO);
        int saved_stderr = dup(STDERR_FILENO);

        if (saved_stdin < 0 || saved_stdout < 0 || saved_stderr < 0) {
            perror("cash: dup");
            return 1;
        }

        int result = apply_redirections(cmd) < 0 ? 1 : 0;

        fflush(NULL);

        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);

        close(saved_stdin);
        close(saved_stdout);
        close(saved_stderr);

        return result;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("cash: fork");
        return 1;
    }

    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        _exit(execute_child_command(cmd));
    }

    return wait_for(pid);
}

/* =========================================================
 * Pipeline execution
 * ========================================================= */

static int execute_pipeline(Pipeline *pipeline)
{
    if (!pipeline->count)
        return 0;

    if (pipeline->count == 1)
        return execute_single(pipeline->commands[0]);

    size_t count = pipeline->count;

    int (*pipes)[2] = cash_malloc((count - 1) * sizeof(*pipes));
    pid_t *pids = cash_malloc(count * sizeof(pid_t));

    for (size_t i = 0; i + 1 < count; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("cash: pipe");

            for (size_t j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            free(pipes);
            free(pids);
            return 1;
        }
    }

    size_t started = 0;

    for (size_t i = 0; i < count; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("cash: fork");
            break;
        }

        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);

            if (i + 1 < count)
                dup2(pipes[i][1], STDOUT_FILENO);

            for (size_t j = 0; j + 1 < count; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            _exit(execute_child_command(pipeline->commands[i]));
        }

        pids[started++] = pid;
    }

    for (size_t i = 0; i + 1 < count; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    int result = 1;

    for (size_t i = 0; i < started; i++) {
        int status = wait_for(pids[i]);

        if (i + 1 == count)
            result = status;
    }

    free(pipes);
    free(pids);

    return result;
}

/* =========================================================
 * Logical execution
 * ========================================================= */

static int execute_andor(AndOr *node)
{
    if (!node || !node->count)
        return 0;

    int status = execute_pipeline(node->pipelines[0]);

    for (size_t i = 1; i < node->count; i++) {
        TokenType op = node->operators[i];

        if (op == TOK_AND && status != 0)
            continue;

        if (op == TOK_OR && status == 0)
            continue;

        status = execute_pipeline(node->pipelines[i]);
    }

    return status;
}

/* =========================================================
 * Program execution
 * ========================================================= */

static int execute_program(Program *program)
{
    if (!program)
        return 1;

    for (size_t i = 0; i < program->count && cash_running; i++) {
        if (program->background[i]) {
            pid_t pid = fork();

            if (pid < 0) {
                perror("cash: fork");
                cash_status = 1;
                continue;
            }

            if (pid == 0) {
                signal(SIGINT, SIG_DFL);
                _exit(execute_andor(program->items[i]));
            }

            printf("[%ld]\n", (long)pid);
            cash_status = 0;
        } else {
            cash_status = execute_andor(program->items[i]);
        }
    }

    return cash_status;
}

/* =========================================================
 * Evaluation
 * ========================================================= */

static int cash_eval(const char *source)
{
    TokenList tokens = {0};

    if (tokenize(source, &tokens) < 0) {
        token_list_free(&tokens);
        return 2;
    }

    Parser parser = {
        .tokens = &tokens,
        .position = 0,
        .error = false
    };

    Program *program = parse_program(&parser);

    int result = 2;

    if (program) {
        result = execute_program(program);
        program_free(program);
    }

    token_list_free(&tokens);
    return result;
}

/* =========================================================
 * Input handling
 * ========================================================= */

static char *read_stream(FILE *stream)
{
    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;

    int c;

    while ((c = fgetc(stream)) != EOF)
        buffer_append(&buffer, &length, &capacity, (char)c);

    if (!buffer)
        buffer = cash_strdup("");

    return buffer;
}

static void interactive_shell(void)
{
    char *line = NULL;
    size_t capacity = 0;

    while (cash_running) {
        char cwd[1024];

        if (getcwd(cwd, sizeof(cwd)))
            printf("cash:%s$ ", cwd);
        else
            printf("cash$ ");

        fflush(stdout);

        ssize_t length = getline(&line, &capacity, stdin);

        if (length < 0)
            break;

        cash_status = cash_eval(line);
    }

    free(line);
}

/* =========================================================
 * Script execution
 * ========================================================= */

static int run_script(const char *path)
{
    FILE *file = fopen(path, "r");

    if (!file) {
        fprintf(stderr, "cash: %s: %s\n",
                path, strerror(errno));
        return 1;
    }

    char *source = read_stream(file);
    fclose(file);

    int result = cash_eval(source);

    free(source);
    return result;
}

/* =========================================================
 * Entry point
 * ========================================================= */

int main(int argc, char **argv)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    if (argc > 1) {
        if (strcmp(argv[1], "-c") == 0) {
            if (argc < 3) {
                fprintf(stderr, "usage: cash -c COMMAND\n");
                return 2;
            }

            return cash_eval(argv[2]);
        }

        return run_script(argv[1]);
    }

    if (isatty(STDIN_FILENO)) {
        interactive_shell();
        return cash_status;
    }

    char *source = read_stream(stdin);
    int result = cash_eval(source);

    free(source);
    return result;
}
