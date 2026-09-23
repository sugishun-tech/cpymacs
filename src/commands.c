#define _XOPEN_SOURCE 700
#include "editor.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wctype.h>
#include <wchar.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *builtin_names[]={
 "backward-char","backward-delete-char-untabify","backward-kill-word","backward-word",
 "beginning-of-buffer","beginning-of-line","copy-region-as-kill","delete-char",
 "delete-horizontal-space","delete-other-windows","delete-window","display-line-numbers-mode",
 "downcase-word","end-of-buffer","end-of-line","exchange-point-and-mark",
 "execute-extended-command","find-file","force-save-buffer","forward-char","forward-word",
 "goto-char","goto-line","indent-for-tab-command","isearch-backward","isearch-forward",
 "keyboard-quit","kill-buffer","kill-emacs","kill-line","kill-region","kill-word",
 "list-buffers","mark-whole-buffer","newline","newline-and-indent","next-buffer","next-line",
 "open-line","other-window","previous-buffer","previous-line","query-replace",
 "recenter-top-bottom","redo","replace-string","save-buffer","save-buffers-kill-terminal",
 "scroll-down-command","scroll-up-command","set-mark-command","split-window-below",
 "split-window-right","switch-to-buffer","text-scale-decrease","text-scale-increase",
 "toggle-read-only","transpose-chars","undo","undo-only","undo-redo","universal-argument",
 "upcase-word","write-file","yank","yank-pop","start-kbd-macro","end-kbd-macro",
 "call-last-kbd-macro","describe-bindings","describe-mode","detach-client",
 "capitalize-word",NULL
};
json_object *editor_command_names(Editor *e) {
    json_object *a=json_object_new_array(); for(int i=0;builtin_names[i];i++) json_object_array_add(a,json_object_new_string(builtin_names[i]));
    json_object *p=plugins_commands(e); if(p) { for(size_t i=0;i<json_object_array_length(p);i++) json_object_array_add(a,json_object_get(json_object_array_get_idx(p,i))); json_object_put(p); } return a;
}
void editor_bind(Editor *e,const char *key,const char *command,const char *mode) {
    if(strlen(key)>=100 || strlen(command)>=100 || (mode && strlen(mode)>=80)) return;
    for(size_t i=0;i<e->binding_count;i++) if(!strcmp(e->bindings[i].key,key) && !strcmp(e->bindings[i].mode,mode?mode:"")) { strcpy(e->bindings[i].command,command); return; }
    if(e->binding_count==e->binding_cap) { e->binding_cap=e->binding_cap?e->binding_cap*2:128; e->bindings=xrealloc(e->bindings,e->binding_cap*sizeof(Binding)); }
    Binding *b=&e->bindings[e->binding_count++]; strcpy(b->key,key); strcpy(b->command,command); strcpy(b->mode,mode?mode:"");
}
void editor_default_bindings(Editor *e) {
    static const char *pairs[][2]={
        {"C-f","forward-char"},{"C-b","backward-char"},{"C-p","previous-line"},{"C-n","next-line"},
        {"C-a","beginning-of-line"},{"C-e","end-of-line"},{"M-f","forward-word"},{"M-b","backward-word"},
        {"M-<","beginning-of-buffer"},{"M->","end-of-buffer"},{"C-d","delete-char"},
        {"DEL","backward-delete-char-untabify"},{"<delete>","delete-char"},{"M-d","kill-word"},
        {"M-DEL","backward-kill-word"},{"C-k","kill-line"},{"C-w","kill-region"},{"M-w","copy-region-as-kill"},
        {"C-y","yank"},{"M-y","yank-pop"},{"C-SPC","set-mark-command"},{"C-@","set-mark-command"},
        {"C-x h","mark-whole-buffer"},{"C-x C-x","exchange-point-and-mark"},
        {"RET","newline"},{"TAB","indent-for-tab-command"},{"C-o","open-line"},
        {"C-j","newline-and-indent"},{"C-t","transpose-chars"},{"M-\\","delete-horizontal-space"},
        {"C-/","undo"},{"C-_","undo"},{"C-x u","undo"},{"C-?","undo-redo"},
        {"C-s","isearch-forward"},{"C-r","isearch-backward"},{"M-%","query-replace"},
        {"M-g g","goto-line"},{"M-g M-g","goto-line"},{"M-x","execute-extended-command"},
        {"C-x C-f","find-file"},{"C-x C-s","save-buffer"},{"C-x C-w","write-file"},
        {"C-x C-c","save-buffers-kill-terminal"},{"C-x b","switch-to-buffer"},{"C-x k","kill-buffer"},
        {"C-x C-b","list-buffers"},{"C-x <right>","next-buffer"},{"C-x <left>","previous-buffer"},
        {"C-x 2","split-window-below"},{"C-x 3","split-window-right"},{"C-x 0","delete-window"},
        {"C-x 1","delete-other-windows"},{"C-x o","other-window"},{"C-x C-q","toggle-read-only"},
        {"C-v","scroll-up-command"},{"M-v","scroll-down-command"},{"C-l","recenter-top-bottom"},
        {"C-g","keyboard-quit"},{"C-u","universal-argument"},{"M-u","upcase-word"},
        {"M-l","downcase-word"},{"M-c","capitalize-word"},{"<left>","backward-char"},
        {"<right>","forward-char"},{"<up>","previous-line"},{"<down>","next-line"},
        {"<home>","beginning-of-line"},{"<end>","end-of-line"},{"<prior>","scroll-down-command"},
        {"<next>","scroll-up-command"},{"C-x (","start-kbd-macro"},{"C-x )","end-kbd-macro"},
        {"C-x e","call-last-kbd-macro"},{"C-h b","describe-bindings"},{"C-h m","describe-mode"},
        {"C-x +","text-scale-increase"},{"C-x -","text-scale-decrease"},{NULL,NULL}
    };
    for(int i=0;pairs[i][0];i++) editor_bind(e,pairs[i][0],pairs[i][1],NULL);
}
static size_t eol(Buffer *b,size_t at) {
    size_t start=text_line_start(&b->text,text_line_of(&b->text,at)+1);
    return start && text_byte(&b->text,start-1)=='\n' ? start-1 : start;
}
static bool word_at(Buffer *b,size_t at) {
    if(at>=text_size(&b->text)) return false;
    char s[5]={0}; size_t n=text_next(&b->text,at)-at; text_copy(&b->text,at,n,s); size_t used; uint32_t c=utf8_decode(s,n,&used);
    /* Programming modes treat '_' as a symbol constituent, not an Emacs word. */
    return iswalnum((wint_t)c)!=0;
}
static size_t move_word(Buffer *b,size_t at,long n) {
    if(n>=0) while(n--) {
        while(at<text_size(&b->text) && !word_at(b,at)) at=text_next(&b->text,at);
        while(at<text_size(&b->text) && word_at(b,at)) at=text_next(&b->text,at);
    } else while(n++) {
        while(at && !word_at(b,text_prev(&b->text,at))) at=text_prev(&b->text,at);
        while(at && word_at(b,text_prev(&b->text,at))) at=text_prev(&b->text,at);
    }
    return at;
}
static size_t move_char(Buffer *b,size_t at,long n) {
    if(n>0) while(n-- && at<text_size(&b->text)) at=text_next(&b->text,at);
    else while(n++<0 && at) at=text_prev(&b->text,at);
    return at;
}
static void push_kill(Editor *e,char *s,bool backwards,bool append) {
    if(append && e->kill_count) {
        size_t a=strlen(e->kill_ring[0]),b=strlen(s); char *out=xmalloc(a+b+1);
        if(backwards) { memcpy(out,s,b); memcpy(out+b,e->kill_ring[0],a+1); }
        else { memcpy(out,e->kill_ring[0],a); memcpy(out+a,s,b+1); }
        free(e->kill_ring[0]); free(s); e->kill_ring[0]=out;
    } else {
        if(e->kill_count==CP_KILL_RING) free(e->kill_ring[--e->kill_count]);
        memmove(e->kill_ring+1,e->kill_ring,(size_t)e->kill_count*sizeof(char*)); e->kill_count++; e->kill_ring[0]=s;
    }
    e->yank_index=0;e->kill_generation++;
}
static bool kill_range(Editor *e,size_t a,size_t z,bool backward,bool only_copy,bool append) {
    Buffer *b=editor_buffer(e); if(z<a) { size_t t=a;a=z;z=t; }
    if(a==z) return true;
    char *s=text_slice(&b->text,a,z-a);
    if(!only_copy && !editor_replace(e,a,z-a,"",0)) { free(s); return false; }
    push_kill(e,s,backward,append); b->mark_active=false;
    if(!only_copy) b->point=a; return true;
}
static void prompt(Editor *e,const char *label,const char *cmd,const char *initial) {
    snprintf(e->prompt,sizeof e->prompt,"%s",label); snprintf(e->prompt_command,sizeof e->prompt_command,"%s",cmd);
    snprintf(e->input,sizeof e->input,"%s",initial?initial:""); e->input_point=strlen(e->input);
}
static void prompt_clear(Editor *e) { e->prompt[0]=e->prompt_command[0]=e->input[0]=0; e->input_point=0; }
static bool undo(Editor *e,bool redo,long n) {
    Buffer *b=editor_buffer(e); if(b->readonly) { editor_message(e,"Buffer is read-only"); return false; }
    if(n<0) { redo=!redo;n=-n; }
    while(n--) {
        if((!redo && !b->history_pos)||(redo && b->history_pos==b->history_len)) { editor_message(e,redo?"No further redo information":"No further undo information"); return false; }
        uint64_t group=b->history[redo?b->history_pos:b->history_pos-1].group;
        do {
            Edit *d=&b->history[redo?b->history_pos:b->history_pos-1]; editor_lex_invalidate(b,d->at);
            text_delete(&b->text,d->at,redo?d->removed_len:d->added_len);
            text_insert(&b->text,d->at,redo?d->added:d->removed,redo?d->added_len:d->removed_len);
            b->point=redo?d->after_point:d->before_point; b->mark=redo?d->after_mark:d->before_mark;
            b->state=redo?d->after_state:d->before_state;
            if(redo) b->history_pos++; else b->history_pos--;
            b->revision++;
        } while((redo?b->history_pos<b->history_len:b->history_pos>0) && b->history[redo?b->history_pos:b->history_pos-1].group==group);
    }
    b->mark_active=false; plugins_event(e,"text_changed"); editor_message(e,redo?"Redo":"Undo"); return true;
}
static bool changed_buffers(Editor *e) { for(size_t i=0;i<e->buffer_count;i++) if(e->buffers[i]->state!=e->buffers[i]->saved_state) return true; return false; }
static void show_text(Editor *e,const char *name,const char *s) {
    Buffer *b=NULL;
    for(size_t i=0;i<e->buffer_count;i++) if(!strcmp(e->buffers[i]->name,name)) b=e->buffers[i];
    if(!b) b=editor_new_buffer(e,name); if(!b) return;
    editor_switch(e,b); b->readonly=false; editor_set_text(e,s,strlen(s)); b->point=0; b->saved_state=b->state; b->readonly=true;
}
static void query_next(Editor *e) {
    Buffer *b=editor_buffer(e); size_t at=editor_find(b,e->replace_from,b->point,false);
    if(at==SIZE_MAX) { prompt_clear(e); editor_message(e,"Query replace finished"); return; }
    e->replace_at=at; b->point=at+strlen(e->replace_from); b->mark=at; b->has_mark=b->mark_active=true;
    prompt(e,"Replace? y/n/!/q: ","query-step","");
}
static bool dispatch(Editor *e,const char *cmd,const char *arg,long count,bool explicit_count) {
    Buffer *b=editor_buffer(e); size_t size=text_size(&b->text); int previous_kind=e->last_kind;
    e->last_kind=0; bool line_motion=!strcmp(cmd,"next-line") || !strcmp(cmd,"previous-line");
    if(!line_motion) b->goal_valid=false;
#define CMD(name) (!strcmp(cmd,name))
    if(CMD("keyboard-quit")) {
        if(!strncmp(e->prompt_command,"isearch-",8)) b->point=e->search_origin;
        prompt_clear(e); e->key_prefix[0]=0; e->prefix_set=false; b->mark_active=false; e->macro_recording=false;
        editor_message(e,"Quit"); return true;
    }
    if(CMD("forward-char") || CMD("backward-char")) { b->point=move_char(b,b->point,CMD("forward-char")?count:-count); return true; }
    if(CMD("forward-word") || CMD("backward-word")) { b->point=move_word(b,b->point,CMD("forward-word")?count:-count); return true; }
    if(CMD("beginning-of-line") || CMD("end-of-line")) {
        long line=(long)text_line_of(&b->text,b->point)+count-1; if(line<0) line=0;
        b->point=text_line_start(&b->text,(size_t)line); if(CMD("end-of-line")) b->point=eol(b,b->point); return true;
    }
    if(CMD("beginning-of-buffer") || CMD("end-of-buffer")) {
        b->mark=b->point; b->has_mark=true;
        if(explicit_count) { long part=count<0?0:count>10?10:count; size_t ch=text_chars(&b->text)*(size_t)part/10; b->point=text_char_to_byte(&b->text,CMD("beginning-of-buffer")?ch:text_chars(&b->text)-ch); }
        else b->point=CMD("beginning-of-buffer")?0:size; return true;
    }
    if(line_motion) {
        if(!b->goal_valid) { b->goal=editor_column(b,b->point); b->goal_valid=true; }
        long row=(long)text_line_of(&b->text,b->point)+(CMD("next-line")?count:-count);
        if(row<0) row=0; if((size_t)row>=text_lines(&b->text)) row=(long)text_lines(&b->text)-1;
        b->point=editor_at_column(b,(size_t)row,b->goal); return true;
    }
    if(CMD("scroll-up-command") || CMD("scroll-down-command")) {
        long amount=explicit_count?count:(e->rows-4); if(amount<1 && !explicit_count) amount=1;
        if(CMD("scroll-down-command")) amount=-amount;
        long row=(long)text_line_of(&b->text,b->point)+amount; if(row<0) row=0; if((size_t)row>=text_lines(&b->text)) row=(long)text_lines(&b->text)-1;
        b->point=editor_at_column(b,(size_t)row,editor_column(b,b->point));
        long top=(long)e->panes[e->active_pane].top+amount; e->panes[e->active_pane].top=top<0?0:(size_t)top; return true;
    }
    if(CMD("recenter-top-bottom")) {
        size_t line=text_line_of(&b->text,b->point),half=(size_t)(e->rows>4?e->rows/2:1); e->panes[e->active_pane].top=line>half?line-half:0; return true;
    }
    if(CMD("newline") || CMD("newline-and-indent") || CMD("open-line")) {
        if(count<0 || count>100000) { editor_message(e,"Invalid newline count"); return false; }
        size_t old=b->point,indent=0;
        if(CMD("newline-and-indent") || (CMD("newline") && b->auto_indent)) {
            size_t start=text_line_start(&b->text,text_line_of(&b->text,b->point));
            while(start+indent<b->point && text_byte(&b->text,start+indent)==' ') indent++;
            size_t p=b->point; while(p>start && (text_byte(&b->text,p-1)==' ' || text_byte(&b->text,p-1)=='\t')) p--;
            if(b->syntax.kind==1 && p>start && text_byte(&b->text,p-1)==':') indent+=(size_t)b->indent_width;
        }
        for(long i=0;i<count;i++) { if(!editor_insert(e,"\n",1)) return false; if(indent) { char *sp=xmalloc(indent); memset(sp,' ',indent); bool ok=editor_insert(e,sp,indent); free(sp); if(!ok) return false; } }
        if(CMD("open-line")) b->point=old; return true;
    }
    if(CMD("indent-for-tab-command")) {
        if(count<0 || count>100000) { editor_message(e,"Invalid indentation count"); return false; }
        size_t n=(size_t)b->indent_width*(size_t)count; char *s=xmalloc(n); memset(s,' ',n); bool ok=editor_insert(e,s,n); free(s); return ok;
    }
    if(CMD("delete-char") || CMD("backward-delete-char-untabify")) {
        long n=CMD("delete-char")?count:-count; size_t at=move_char(b,b->point,n),a=at<b->point?at:b->point,z=at>b->point?at:b->point;
        if(explicit_count && CMD("delete-char")) { e->last_kind=1; return kill_range(e,a,z,n<0,false,previous_kind==1); }
        return editor_replace(e,a,z-a,"",0);
    }
    if(CMD("set-mark-command")) { if(!explicit_count && b->mark_active) { b->mark_active=false; editor_message(e,"Mark deactivated"); } else { b->mark=b->point; b->has_mark=b->mark_active=true; editor_message(e,"Mark set"); } return true; }
    if(CMD("mark-whole-buffer")) { b->mark=size; b->has_mark=b->mark_active=true; b->point=0; return true; }
    if(CMD("exchange-point-and-mark")) { if(!b->has_mark) { editor_message(e,"No mark set in this buffer"); return false; } size_t t=b->point;b->point=b->mark;b->mark=t;b->mark_active=true;return true; }
    if(CMD("kill-region") || CMD("copy-region-as-kill")) {
        if(!b->has_mark) { editor_message(e,"The mark is not set"); return false; }
        bool copy=CMD("copy-region-as-kill"); e->last_kind=copy?0:1;
        return kill_range(e,b->point,b->mark,b->point>b->mark,copy,!copy && previous_kind==1);
    }
    if(CMD("kill-line")) {
        size_t a=b->point,z;
        if(explicit_count) {
            long row=(long)text_line_of(&b->text,b->point);
            if(count>0) z=text_line_start(&b->text,(size_t)(row+count));
            else { long target=row+count; if(target<0) target=0; z=text_line_start(&b->text,(size_t)target); }
        } else {
            z=eol(b,b->point); bool whitespace=true;
            for(size_t i=b->point;i<z;i++) if(text_byte(&b->text,i)!=' ' && text_byte(&b->text,i)!='\t') { whitespace=false;break; }
            if(whitespace && z<size) z++;
        }
        e->last_kind=1; return kill_range(e,a,z,z<a,false,previous_kind==1);
    }
    if(CMD("kill-word") || CMD("backward-kill-word")) {
        size_t at=move_word(b,b->point,CMD("kill-word")?count:-count); e->last_kind=1; return kill_range(e,b->point,at,at<b->point,false,previous_kind==1);
    }
    if(CMD("yank") || CMD("yank-pop")) {
        if(!e->kill_count) { editor_message(e,"Kill ring is empty"); return false; }
        if(CMD("yank-pop")) {
            if(previous_kind!=2) { editor_message(e,"Previous command was not a yank"); return false; }
            e->yank_index=(int)(((long)e->yank_index+count)%e->kill_count); if(e->yank_index<0) e->yank_index+=e->kill_count;
            if(!editor_replace(e,e->yank_start,e->yank_end-e->yank_start,"",0)) return false; b->point=e->yank_start;
        } else { e->yank_index=explicit_count?(int)((count>0?count-1:0)%e->kill_count):0; e->yank_start=b->point; }
        b->mark=b->point;b->has_mark=true; const char *s=e->kill_ring[e->yank_index];
        if(!editor_insert(e,s,strlen(s))) return false;
        e->yank_end=b->point;e->last_kind=2;return true;
    }
    if(CMD("undo") || CMD("undo-only") || CMD("redo") || CMD("undo-redo")) { e->last_kind=3;return undo(e,CMD("redo")||CMD("undo-redo"),count); }
    if(CMD("transpose-chars")) {
        if(!count) { editor_message(e,"Zero-argument transposition at mark is not supported"); return false; }
        if(b->point==size && size) b->point=text_prev(&b->text,size);
        for(long i=0;i<labs(count);i++) {
            size_t p=b->point;if(!p || p>=text_size(&b->text)) return false;
            size_t a=text_prev(&b->text,p),z=text_next(&b->text,p),left=p-a,right=z-p;
            char *s=text_slice(&b->text,a,z-a),*out=xmalloc(z-a);memcpy(out,s+left,right);memcpy(out+right,s,left);
            bool ok=editor_replace(e,a,z-a,out,z-a);free(s);free(out);if(!ok)return false;
            b->point=count>0?z:a;
        }return true;
    }
    if(CMD("delete-horizontal-space")) {
        size_t a=b->point,z=a;
        while(a && (text_byte(&b->text,a-1)==' '||text_byte(&b->text,a-1)=='\t')) a--;
        while(z<size && (text_byte(&b->text,z)==' '||text_byte(&b->text,z)=='\t')) z++;
        return editor_replace(e,a,z-a,"",0);
    }
    if(CMD("upcase-word") || CMD("downcase-word") || CMD("capitalize-word")) {
        size_t old=b->point,z=move_word(b,old,count),a=old; if(z<a){size_t t=a;a=z;z=t;}
        char *s=text_slice(&b->text,a,z-a); size_t cap=(z-a)*4+1;char *out=xmalloc(cap);size_t used=0;bool first=true;
        for(size_t i=0;i<z-a;) { size_t k;uint32_t c=utf8_decode(s+i,z-a-i,&k);i+=k;
            if(iswalnum(c)) { c=(CMD("upcase-word")||(CMD("capitalize-word")&&first))?towupper(c):towlower(c);first=false; } else first=true;
            char tmp[MB_LEN_MAX];mbstate_t st={0};size_t n=wcrtomb(tmp,(wchar_t)c,&st);
            if(n==(size_t)-1) { n=k;memcpy(tmp,s+i-k,k); }memcpy(out+used,tmp,n);used+=n;
        }
        bool ok=editor_replace(e,a,z-a,out,used);free(s);free(out);b->point=count<0?old:a+used;return ok;
    }
    if(CMD("find-file")) { if(!arg){prompt(e,"Find file: ",cmd,"");return true;}return editor_open(e,arg); }
    if(CMD("save-buffer") || CMD("force-save-buffer") || CMD("write-file")) {
        if(CMD("write-file") && !arg) {prompt(e,"Write file: ",cmd,b->path?b->path:"");return true;}
        if(!arg && !b->path) {prompt(e,"File to save in: ","write-file","");return true;}
        return editor_save(e,arg,CMD("force-save-buffer"));
    }
    if(CMD("save-buffers-kill-terminal")) {
        if(changed_buffers(e)) {prompt(e,"Modified buffers exist. Save all (s), discard (y), cancel (n): ","quit-confirm","");return true;}
        e->quit=true;return true;
    }
    if(CMD("kill-emacs")) {e->quit=true;return true;}
    if(CMD("detach-client")) {e->detach=true;return true;}
    if(CMD("execute-extended-command")) {if(!arg){prompt(e,"M-x ",cmd,"");return true;}return editor_command(e,arg,NULL,count,explicit_count);}
    if(CMD("goto-line") || CMD("goto-char")) {
        if(!arg && !explicit_count) {prompt(e,CMD("goto-line")?"Goto line: ":"Goto char: ",cmd,"");return true;}
        long value=count; if(arg){char *end;errno=0;value=strtol(arg,&end,10);if(errno || *end || value<1){editor_message(e,"Expected a positive integer");return false;}}
        b->point=CMD("goto-line")?text_line_start(&b->text,(size_t)(value>0?value-1:0)):text_char_to_byte(&b->text,(size_t)(value>0?value-1:0));return true;
    }
    if(CMD("isearch-forward") || CMD("isearch-backward")) {
        e->search_origin=b->point;e->search_backward=CMD("isearch-backward");e->search_failed=e->search_wrapped=false;e->search_good[0]=0;
        prompt(e,e->search_backward?"I-search backward: ":"I-search: ",cmd,arg?arg:"");return true;
    }
    if(CMD("query-replace") || CMD("replace-string")) {
        if(!arg){prompt(e,CMD("query-replace")?"Query replace: ":"Replace string: ",CMD("query-replace")?"query-from":"replace-from","");return true;}
        editor_message(e,"Use interactive M-%% or the replace protocol operation");return false;
    }
    if(CMD("switch-to-buffer")) {
        if(!arg){prompt(e,"Switch to buffer: ",cmd,"");return true;}
        if(!*arg && e->buffer_count>1){editor_switch(e,e->buffers[(e->current+1)%e->buffer_count]);return true;}
        for(size_t i=0;i<e->buffer_count;i++) if(!strcmp(e->buffers[i]->name,arg)){editor_switch(e,e->buffers[i]);return true;}
        Buffer *newbuf=editor_new_buffer(e,*arg?arg:"*scratch*");if(newbuf){editor_switch(e,newbuf);plugins_event(e,"after_open");}return newbuf!=NULL;
    }
    if(CMD("next-buffer") || CMD("previous-buffer")) {long next=e->current+(CMD("next-buffer")?count:-count);next%=(long)e->buffer_count;if(next<0)next+=(long)e->buffer_count;editor_switch(e,e->buffers[next]);return true;}
    if(CMD("kill-buffer")) {
        if(!arg){prompt(e,"Kill buffer: ",cmd,b->name);return true;}
        Buffer *target=NULL;for(size_t i=0;i<e->buffer_count;i++) if(!strcmp(e->buffers[i]->name,arg))target=e->buffers[i];
        if(!target){editor_message(e,"No such buffer");return false;}
        if(target->state!=target->saved_state){e->pending_buffer=target->id;prompt(e,"Kill modified buffer? y/n: ","kill-confirm","");return true;}
        editor_drop_buffer(e,target);return true;
    }
    if(CMD("list-buffers")) {
        size_t cap=2048+e->buffer_count*512;char *out=xmalloc(cap);size_t n=(size_t)snprintf(out,cap,"Buffers\n\n  ID  Modified  Bytes  Mode  Name  File\n");
        for(size_t i=0;i<e->buffer_count;i++){Buffer *q=e->buffers[i];if(!*q->name)continue;n+=(size_t)snprintf(out+n,cap-n,"%4d  %c  %zu  %s  %.100s  %.200s\n",q->id,q->state!=q->saved_state?'*':' ',text_size(&q->text),q->mode,q->name,q->path?q->path:"");}
        show_text(e,"*Buffer List*",out);free(out);return true;
    }
    if(CMD("split-window-below") || CMD("split-window-right")) {
        if(e->pane_count>=CP_MAX_PANES){editor_message(e,"Window limit reached");return false;}
        e->panes[e->active_pane].point=b->point;e->panes[e->pane_count++]=e->panes[e->active_pane];e->split_vertical=CMD("split-window-right");return true;
    }
    if(CMD("other-window")) {
        e->panes[e->active_pane].point=b->point;long n=(e->active_pane+count)%e->pane_count;if(n<0)n+=e->pane_count;e->active_pane=(int)n;
        Buffer *q=editor_by_id(e,e->panes[n].buffer);if(q){editor_switch(e,q);q->point=e->panes[n].point>text_size(&q->text)?text_size(&q->text):e->panes[n].point;}return true;
    }
    if(CMD("delete-other-windows")) {e->panes[0]=e->panes[e->active_pane];e->active_pane=0;e->pane_count=1;return true;}
    if(CMD("delete-window")) {
        if(e->pane_count==1){editor_message(e,"Cannot delete the sole window");return false;}
        memmove(e->panes+e->active_pane,e->panes+e->active_pane+1,(size_t)(e->pane_count-e->active_pane-1)*sizeof(Pane));e->pane_count--;if(e->active_pane>=e->pane_count)e->active_pane=0;
        Buffer *q=editor_by_id(e,e->panes[e->active_pane].buffer);if(q)editor_switch(e,q);return true;
    }
    if(CMD("display-line-numbers-mode")){e->show_line_numbers=!e->show_line_numbers;return true;}
    if(CMD("toggle-read-only")){b->readonly=!b->readonly;return true;}
    if(CMD("text-scale-increase") || CMD("text-scale-decrease")){e->font_size+=(CMD("text-scale-increase")?1:-1)*(int)count;if(e->font_size<8)e->font_size=8;if(e->font_size>48)e->font_size=48;return true;}
    if(CMD("start-kbd-macro")) {
        for(size_t i=0;i<e->macro_len;i++)free(e->macro_keys[i]);e->macro_len=0;e->macro_recording=true;editor_message(e,"Defining keyboard macro");return true;
    }
    if(CMD("end-kbd-macro")){e->macro_recording=false;if(e->macro_len>=2){free(e->macro_keys[--e->macro_len]);free(e->macro_keys[--e->macro_len]);}editor_message(e,"Keyboard macro defined");return true;}
    if(CMD("call-last-kbd-macro")) {
        if(e->macro_playing || e->macro_recording){editor_message(e,"Recursive macro rejected");return false;}
        e->macro_playing=true;for(long k=0;k<count;k++)for(size_t i=0;i<e->macro_len;i++)editor_key(e,e->macro_keys[i]);e->macro_playing=false;return true;
    }
    if(CMD("describe-bindings")) {
        size_t cap=128+e->binding_count*220;char *out=xmalloc(cap);size_t n=(size_t)snprintf(out,cap,"Key bindings\n\n");
        for(size_t i=0;i<e->binding_count;i++){Binding *k=&e->bindings[i];n+=(size_t)snprintf(out+n,cap-n,"%-16s %-45s %s\n",k->key,k->command,k->mode);}show_text(e,"*Help*",out);free(out);return true;
    }
    if(CMD("describe-mode")){editor_message(e,"%s | TAB: %d spaces | auto-indent: %s | syntax: %s",b->mode,b->indent_width,b->auto_indent?"on":"off",b->syntax.name);return true;}
    if(CMD("auto-fill-mode")){editor_message(e,"Auto-fill is not implemented; no setting changed");return false;}
    if(plugins_command(e,cmd,arg,count))return true;
    if(!*e->message)editor_message(e,"Unknown command: %s",cmd);return false;
#undef CMD
}
bool editor_command(Editor *e,const char *cmd,const char *arg,long count,bool explicit_count) {
    if(e->command_depth>=32){editor_message(e,"Command recursion limit reached");return false;}
    if(count < -100000 || count > 100000){editor_message(e,"Numeric argument exceeds safety limit");return false;}
    bool outer=e->command_depth++==0;if(outer){e->group++;e->message[0]=0;}
    bool ok=dispatch(e,cmd,arg,count,explicit_count);e->command_depth--;
    if(outer)snprintf(e->last_command,sizeof e->last_command,"%s",cmd);return ok;
}
static size_t str_prev(const char *s,size_t i){if(i){do{i--;}while(i && ((unsigned char)s[i]&0xc0)==0x80);}return i;}
static size_t str_next(const char *s,size_t i){if(s[i]){do{i++;}while(s[i] && ((unsigned char)s[i]&0xc0)==0x80);}return i;}
static bool input_insert(Editor *e,const char *s) {
    size_t n=strlen(s),len=strlen(e->input);if(n>=sizeof e->input-len){editor_message(e,"Minibuffer input too long");return false;}
    memmove(e->input+e->input_point+n,e->input+e->input_point,len-e->input_point+1);memcpy(e->input+e->input_point,s,n);e->input_point+=n;return true;
}
static void search_update(Editor *e,bool repeat) {
    Buffer *b=editor_buffer(e);if(!*e->input)return;
    size_t start=repeat?b->point:e->search_origin;
    if(repeat && e->search_backward && start>=strlen(e->input))start-=strlen(e->input);
    size_t at=editor_find(b,e->input,start,e->search_backward);
    if(at==SIZE_MAX && repeat){at=editor_find(b,e->input,e->search_backward?text_size(&b->text):0,e->search_backward);e->search_wrapped=true;}
    e->search_failed=at==SIZE_MAX;
    if(!e->search_failed){b->point=e->search_backward?at:at+strlen(e->input);snprintf(e->search_good,sizeof e->search_good,"%s",e->input);}
    snprintf(e->prompt,sizeof e->prompt,"%sI-search%s: ",e->search_failed?"Failing ":e->search_wrapped?"Wrapped ":"",e->search_backward?" backward":"");
}
static void complete_input(Editor *e) {
    json_object *names=NULL;
    if(!strcmp(e->prompt_command,"execute-extended-command"))names=editor_command_names(e);
    else if(!strcmp(e->prompt_command,"switch-to-buffer") || !strcmp(e->prompt_command,"kill-buffer")){names=json_object_new_array();for(size_t i=0;i<e->buffer_count;i++)if(*e->buffers[i]->name)json_object_array_add(names,json_object_new_string(e->buffers[i]->name));}
    else if(!strcmp(e->prompt_command,"find-file") || !strcmp(e->prompt_command,"write-file")) {
        char directory[CP_PROMPT_MAX],prefix[CP_PROMPT_MAX],expanded[CP_PROMPT_MAX*2];
        const char *slash=strrchr(e->input,'/');size_t n=slash?(size_t)(slash-e->input+1):0;
        memcpy(prefix,e->input,n);prefix[n]=0;snprintf(directory,sizeof directory,"%s",n?prefix:".");
        if(directory[0]=='~' && (directory[1]=='/' || !directory[1]))
            snprintf(expanded,sizeof expanded,"%s%s",getenv("HOME")?getenv("HOME"):".",directory+1);
        else snprintf(expanded,sizeof expanded,"%s",directory);
        DIR *dir=opendir(expanded);names=json_object_new_array();
        if(dir){struct dirent *entry;while((entry=readdir(dir))!=NULL){
            if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
            char path[CP_PROMPT_MAX*3],candidate[CP_PROMPT_MAX*2];struct stat st;
            snprintf(path,sizeof path,"%s/%s",expanded,entry->d_name);
            bool isdir=stat(path,&st)==0 && S_ISDIR(st.st_mode);
            snprintf(candidate,sizeof candidate,"%s%s%s",prefix,entry->d_name,isdir?"/":"");
            json_object_array_add(names,json_object_new_string(candidate));
        }closedir(dir);}
    }
    if(!names){editor_message(e,"No completion source for this prompt");return;}
    size_t len=strlen(e->input),common=0;const char *first=NULL;int matches=0;char choices[1800]="";
    for(size_t i=0;i<json_object_array_length(names);i++) {const char *s=json_object_get_string(json_object_array_get_idx(names,i));if(strncmp(s,e->input,len))continue;
        if(!first){first=s;common=strlen(s);}else{size_t k=0;while(k<common && first[k]==s[k])k++;common=k;}matches++;
        if(strlen(choices)+strlen(s)+3<sizeof choices){strcat(choices,s);strcat(choices,"  ");}}
    if(first){if(common>=sizeof e->input)common=sizeof e->input-1;memcpy(e->input,first,common);e->input[common]=0;e->input_point=common;}
    editor_message(e,matches?"%s":"No completions",choices);json_object_put(names);
}
static bool prompt_key(Editor *e,const char *key) {
    bool search=!strncmp(e->prompt_command,"isearch-",8);
    if(!strcmp(e->prompt_command,"kill-confirm")) {
        if(!strcmp(key,"y")){Buffer *b=editor_by_id(e,e->pending_buffer);prompt_clear(e);if(b)editor_drop_buffer(e,b);return true;}
        if(!strcmp(key,"n")||!strcmp(key,"C-g")||!strcmp(key,"RET")){prompt_clear(e);editor_message(e,"Kill canceled");}return true;
    }
    if(!strcmp(e->prompt_command,"quit-confirm")) {
        if(!strcmp(key,"y")){prompt_clear(e);e->quit=true;return true;}
        if(!strcmp(key,"s")){int current=e->current;for(size_t i=0;i<e->buffer_count;i++){Buffer *b=e->buffers[i];if(b->state!=b->saved_state){editor_switch(e,b);if(!editor_save(e,NULL,false)){prompt_clear(e);return false;}}}editor_switch(e,e->buffers[current]);prompt_clear(e);e->quit=true;return true;}
        if(!strcmp(key,"n")||!strcmp(key,"C-g")||!strcmp(key,"RET")){prompt_clear(e);editor_message(e,"Quit canceled");}return true;
    }
    if(!strcmp(e->prompt_command,"query-step")) {
        if(!strcmp(key,"q")||!strcmp(key,"RET")||!strcmp(key,"C-g")){prompt_clear(e);editor_buffer(e)->mark_active=false;return true;}
        if(!strcmp(key,"y")||!strcmp(key,"!")){
            bool all=!strcmp(key,"!");
            do{e->group++;if(!editor_replace(e,e->replace_at,strlen(e->replace_from),e->replace_to,strlen(e->replace_to))){prompt_clear(e);return false;}editor_buffer(e)->point=e->replace_at+strlen(e->replace_to);query_next(e);}while(all && *e->prompt_command);return true;
        }
        if(!strcmp(key,"n")){query_next(e);return true;}return true;
    }
    if(!strcmp(key,"C-g")) {
        if(search && e->search_failed){snprintf(e->input,sizeof e->input,"%s",e->search_good);e->input_point=strlen(e->input);e->search_failed=false;search_update(e,false);return true;}
        return editor_command(e,"keyboard-quit",NULL,1,false);
    }
    if(search && (!strcmp(key,"C-s")||!strcmp(key,"C-r"))){e->search_backward=!strcmp(key,"C-r");if(!*e->input){snprintf(e->input,sizeof e->input,"%s",e->last_search);e->input_point=strlen(e->input);}search_update(e,true);return true;}
    if(!strcmp(key,"RET")) {
        char cmd[100],arg[CP_PROMPT_MAX];strcpy(cmd,e->prompt_command);strcpy(arg,e->input);prompt_clear(e);
        if(search){strcpy(e->last_search,arg);editor_message(e,"Mark saved where search started");Buffer *b=editor_buffer(e);b->mark=e->search_origin;b->has_mark=true;return true;}
        if(!strcmp(cmd,"query-from")||!strcmp(cmd,"replace-from")){if(!*arg){editor_message(e,"Empty search string");return false;}strcpy(e->replace_from,arg);prompt(e,"Replace with: ",!strcmp(cmd,"query-from")?"query-to":"replace-to","");return true;}
        if(!strcmp(cmd,"query-to")||!strcmp(cmd,"replace-to")){strcpy(e->replace_to,arg);query_next(e);if(!strcmp(cmd,"replace-to") && *e->prompt_command)return prompt_key(e,"!");return true;}
        return editor_command(e,cmd,arg,1,false);
    }
    if(!strcmp(key,"TAB")){complete_input(e);return true;}
    if(!strcmp(key,"C-a"))e->input_point=0;
    else if(!strcmp(key,"C-e"))e->input_point=strlen(e->input);
    else if(!strcmp(key,"C-b")||!strcmp(key,"<left>"))e->input_point=str_prev(e->input,e->input_point);
    else if(!strcmp(key,"C-f")||!strcmp(key,"<right>"))e->input_point=str_next(e->input,e->input_point);
    else if(!strcmp(key,"DEL")){size_t at=str_prev(e->input,e->input_point);memmove(e->input+at,e->input+e->input_point,strlen(e->input)-e->input_point+1);e->input_point=at;}
    else if(!strcmp(key,"C-d")||!strcmp(key,"<delete>")){size_t z=str_next(e->input,e->input_point);memmove(e->input+e->input_point,e->input+z,strlen(e->input)-z+1);}
    else if(!strcmp(key,"C-k"))e->input[e->input_point]=0;
    else if(!strcmp(key,"C-y")){if(e->kill_count)input_insert(e,e->kill_ring[0]);}
    else if(key[0] && (strlen(key)==1 || (unsigned char)key[0]>=128))input_insert(e,key);
    else if(search){strcpy(e->last_search,e->input);prompt_clear(e);return editor_key(e,key);}
    else {editor_message(e,"Key not available in minibuffer: %s",key);return false;}
    if(search){editor_buffer(e)->point=e->search_origin;search_update(e,false);}return true;
}
bool editor_key(Editor *e,const char *key) {
    if(!key || !*key || strlen(key)>80){editor_message(e,"Invalid key token");return false;}
    if(e->macro_recording && !e->macro_playing) {
        if(e->macro_len>=10000){e->macro_recording=false;editor_message(e,"Macro size limit reached");return false;}
        if(e->macro_len==e->macro_cap){e->macro_cap=e->macro_cap?e->macro_cap*2:64;e->macro_keys=xrealloc(e->macro_keys,e->macro_cap*sizeof(char*));}e->macro_keys[e->macro_len++]=xstrdup(key);
    }
    if(*e->prompt_command)return prompt_key(e,key);
    if(!strcmp(key,"C-g"))return editor_command(e,"keyboard-quit",NULL,1,false);
    if(!strcmp(key,"C-u") && !*e->key_prefix){
        if(!e->prefix_set){e->prefix_value=4;e->prefix_digits=false;e->prefix_negative=false;}else if(!e->prefix_digits && e->prefix_value<=25000)e->prefix_value*=4;
        e->prefix_set=true;editor_message(e,"Arg: %ld",e->prefix_value);return true;
    }
    bool meta_digit=strlen(key)==3 && key[0]=='M' && key[1]=='-' && (isdigit((unsigned char)key[2])||key[2]=='-');
    bool plain_digit=e->prefix_set && strlen(key)==1 && (isdigit((unsigned char)key[0])||key[0]=='-');
    if(!*e->key_prefix && (meta_digit||plain_digit)) {
        char c=meta_digit?key[2]:key[0];
        if(!e->prefix_set){e->prefix_value=0;e->prefix_digits=false;e->prefix_negative=false;}
        if(c=='-'){e->prefix_negative=true;e->prefix_value=e->prefix_digits?e->prefix_value:1;}
        else {if(!e->prefix_digits)e->prefix_value=0;if(e->prefix_value>10000){editor_message(e,"Argument limit reached");return false;}e->prefix_value=e->prefix_value*10+c-'0';e->prefix_digits=true;}
        e->prefix_set=true;editor_message(e,"Arg: %s%ld",e->prefix_negative?"-":"",e->prefix_value);return true;
    }
    char full[100];if(*e->key_prefix)snprintf(full,sizeof full,"%.70s %.25s",e->key_prefix,key);else snprintf(full,sizeof full,"%s",key);
    const char *command=NULL;bool is_prefix=false;size_t len=strlen(full);Buffer *b=editor_buffer(e);
    for(int pass=0;pass<2;pass++)for(size_t i=0;i<e->binding_count;i++) {
        Binding *k=&e->bindings[i];if(pass==0?*k->mode!=0:strcmp(k->mode,b->mode)!=0)continue;
        if(!strcmp(k->key,full))command=k->command;
        if(!strncmp(k->key,full,len) && k->key[len]==' ')is_prefix=true;
    }
    if(command){e->key_prefix[0]=0;long count=e->prefix_set?e->prefix_value:1;if(e->prefix_negative)count=-count;bool explicit_count=e->prefix_set;e->prefix_set=e->prefix_digits=e->prefix_negative=false;return editor_command(e,command,NULL,count,explicit_count);}
    if(is_prefix){strcpy(e->key_prefix,full);editor_message(e,"%s-",full);return true;}
    bool printable=!*e->key_prefix && (strlen(key)==1 || (unsigned char)key[0]>=128);
    e->key_prefix[0]=0;
    if(printable && utf8_valid(key,strlen(key))) {
        long n=e->prefix_set?e->prefix_value:1;if(e->prefix_negative)n=-n;e->prefix_set=e->prefix_digits=e->prefix_negative=false;
        if(n<0 || n>100000){editor_message(e,"Invalid insertion count");return false;}
        e->group++;e->last_kind=0;e->message[0]=0;
        size_t lenkey=strlen(key),lenall=lenkey*(size_t)n;char *s=xmalloc(lenall);for(long i=0;i<n;i++)memcpy(s+(size_t)i*lenkey,key,lenkey);
        bool ok=editor_insert(e,s,lenall);free(s);return ok;
    }
    e->prefix_set=false;editor_message(e,"%s is undefined",full);return false;
}

bool editor_external_yank(Editor *e,const char *s) {
    if(!utf8_valid(s,strlen(s)))return false;
    push_kill(e,xstrdup(s),false,false);
    return editor_command(e,"yank",NULL,1,false);
}

/* Bracketed paste and IME commits are literal input, even in the minibuffer. */
bool editor_paste(Editor *e,const char *s) {
    if(!utf8_valid(s,strlen(s)))return false;
    if(*e->prompt_command) {
        if(strstr(e->prompt_command,"confirm") || !strcmp(e->prompt_command,"query-step")){
            editor_message(e,"Answer confirmation prompts with individual keys");return false;
        }
        if(!input_insert(e,s))return false;
        if(!strncmp(e->prompt_command,"isearch-",8))search_update(e,false);
        return true;
    }
    return editor_insert(e,s,strlen(s));
}
