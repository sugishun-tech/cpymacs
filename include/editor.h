#ifndef CPYMACS_EDITOR_H
#define CPYMACS_EDITOR_H
#include "text.h"
#include <json-c/json.h>
#include <sys/stat.h>

#define CP_MAX_BUFFERS 256
#define CP_KILL_RING 60
#define CP_MAX_PANES 8
#define CP_PROMPT_MAX 4096

typedef struct {
    size_t at, removed_len, added_len, before_point, after_point;
    size_t before_mark, after_mark;
    char *removed, *added;
    uint64_t group, before_state, after_state;
} Edit;
typedef struct {
    char *name, *keywords, *builtins;
    int kind; /* 0=text, 1=python, 2=C-family, 3=markup, 4=CSS, 5=Jinja */
} Syntax;
typedef struct {
    size_t point, column, line_start;
    bool valid;
} ColumnCache;
typedef struct {
    int id;
    char *path, *name;
    Text text;
    size_t point, mark, goal;
    bool has_mark, mark_active, goal_valid, readonly, crlf, bom;
    uint64_t revision, state, saved_state;
    Edit *history;
    size_t history_len, history_cap, history_pos;
    int tab_width, indent_width;
    bool auto_indent;
    char mode[80];
    Syntax syntax;
    unsigned char *lex_states;
    size_t lex_capacity, lex_valid;
    ColumnCache column_cache, seek_cache;
    struct stat disk_stat;
    bool disk_known;
} Buffer;
typedef struct {
    int buffer;
    size_t top, left, point;
} Pane;
typedef struct {
    char key[100], command[100], mode[80];
} Binding;
typedef struct Editor {
    Buffer *buffers[CP_MAX_BUFFERS];
    size_t buffer_count;
    int current, next_id, pending_buffer;
    Pane panes[CP_MAX_PANES];
    int pane_count, active_pane;
    bool split_vertical;
    Binding *bindings;
    size_t binding_count, binding_cap;
    char *kill_ring[CP_KILL_RING];
    int kill_count, yank_index;
    uint64_t kill_generation;
    size_t yank_start, yank_end;
    int last_kind; /* 1=kill, 2=yank, 3=undo */
    char message[2048], last_command[100], key_prefix[100];
    char prompt[100], prompt_command[100], input[CP_PROMPT_MAX];
    size_t input_point, search_origin;
    bool search_backward, search_failed, search_wrapped;
    char search_good[CP_PROMPT_MAX];
    char last_search[CP_PROMPT_MAX], replace_from[CP_PROMPT_MAX], replace_to[CP_PROMPT_MAX];
    size_t replace_at;
    bool prefix_set, prefix_digits, prefix_negative;
    long prefix_value;
    uint64_t group, next_state;
    bool quit, detach, python_enabled, show_line_numbers;
    int rows, cols, font_size;
    char font[128], theme[32];
    bool show_highlight;
    unsigned long colors[10];
    void *python_commands, *python_hooks, *python_globals;
    char *config_paths[32];
    int config_count;
    unsigned command_depth;
    char **macro_keys;
    size_t macro_len, macro_cap;
    bool macro_recording, macro_playing;
} Editor;
Buffer *editor_buffer(Editor *e);
Buffer *editor_new_buffer(Editor *e,const char *name);
Buffer *editor_by_id(Editor *e,int id);
void editor_drop_buffer(Editor *e,Buffer *b);
void editor_init(Editor *e);
void editor_destroy(Editor *e);
void editor_message(Editor *e,const char *fmt,...);
void editor_switch(Editor *e,Buffer *b);
bool editor_open(Editor *e,const char *path);
bool editor_save(Editor *e,const char *path,bool force);
bool editor_replace(Editor *e,size_t at,size_t count,const char *text,size_t length);
bool editor_set_text(Editor *e,const char *text,size_t length);
bool editor_command(Editor *e,const char *command,const char *arg,long count,bool explicit_count);
bool editor_key(Editor *e,const char *key);
bool editor_insert(Editor *e,const char *s,size_t n);
bool editor_external_yank(Editor *e,const char *s);
void editor_bind(Editor *e,const char *key,const char *command,const char *mode);
void editor_default_bindings(Editor *e);
json_object *editor_command_names(Editor *e);
json_object *editor_snapshot(Editor *e,int rows,int cols);
json_object *editor_request(Editor *e,json_object *req);
size_t editor_column(Buffer *b,size_t point);
size_t editor_at_column(Buffer *b,size_t line,size_t column);
size_t editor_at_column_ex(Buffer *b,size_t line,size_t column,size_t *actual);
bool editor_paste(Editor *e,const char *s);
size_t editor_find(Buffer *b,const char *needle,size_t start,bool backward);
void editor_lex_invalidate(Buffer *b,size_t at);
json_object *editor_line_spans(Buffer *b,size_t row,const char *line,size_t n);

bool plugins_init(Editor *e,const char *python_dir,bool user_config);
void plugins_shutdown(Editor *e);
bool plugins_load(Editor *e,const char *path);
bool plugins_command(Editor *e,const char *name,const char *argument,long count);
void plugins_event(Editor *e,const char *event);
bool plugins_before_save(Editor *e);
json_object *plugins_commands(Editor *e);

#endif
