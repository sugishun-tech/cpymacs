#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "editor.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <wchar.h>
#include <limits.h>
#include <sys/types.h>

Buffer *editor_buffer(Editor *e) { return e->buffers[e->current]; }
Buffer *editor_by_id(Editor *e,int id) { for(size_t i=0;i<e->buffer_count;i++) if(e->buffers[i]->id==id) return e->buffers[i]; return NULL; }
void editor_message(Editor *e,const char *fmt,...) { va_list ap; va_start(ap,fmt); vsnprintf(e->message,sizeof e->message,fmt,ap); va_end(ap); }
void editor_switch(Editor *e,Buffer *b) {
    for(size_t i=0;i<e->buffer_count;i++) if(e->buffers[i]==b) { e->current=(int)i; e->panes[e->active_pane].buffer=b->id; return; }
}
Buffer *editor_new_buffer(Editor *e,const char *name) {
    if(e->buffer_count>=CP_MAX_BUFFERS) { editor_message(e,"Buffer limit reached"); return NULL; }
    Buffer *b=xmalloc(sizeof(*b)); memset(b,0,sizeof(*b)); text_init(&b->text,"",0);
    b->name=xstrdup(name); b->id=++e->next_id; b->tab_width=b->indent_width=4;
    strcpy(b->mode,"fundamental-mode"); b->syntax.name=xstrdup("text");
    b->syntax.keywords=xstrdup(""); b->syntax.builtins=xstrdup("");
    e->buffers[e->buffer_count++]=b; return b;
}
void editor_init(Editor *e) {
    memset(e,0,sizeof(*e)); e->rows=24; e->cols=80; e->show_highlight=true;
    e->font_size=16; strcpy(e->font,"fixed"); strcpy(e->theme,"default");
    unsigned long c[]={0xd9e1eb,0x15191f,0x748292,0xffca85,0xb6dc86,0xe699e6,0x86c9ed,0xe89d8b,0x365070,0x222a35};
    memcpy(e->colors,c,sizeof c); e->pane_count=1;
    Buffer *b=editor_new_buffer(e,"*scratch*"); e->panes[0].buffer=b->id;
    editor_default_bindings(e);
    editor_message(e,"cpymacs | C-x C-f open | C-x C-s save | C-x C-c quit | M-x commands");
}
static void edit_free(Edit *d) { free(d->removed); free(d->added); }
static void free_buffer(Buffer *b) {
    text_free(&b->text);free(b->path);free(b->name);
    free(b->syntax.name);free(b->syntax.keywords);free(b->syntax.builtins);free(b->lex_states);
    for(size_t j=0;j<b->history_len;j++)edit_free(&b->history[j]);
    free(b->history);free(b);
}
void editor_drop_buffer(Editor *e,Buffer *b) {
    if(e->buffer_count==1)editor_new_buffer(e,"*scratch*");
    Buffer *fallback=NULL,*selected=editor_buffer(e);
    for(size_t i=0;i<e->buffer_count;i++)if(e->buffers[i]!=b){fallback=e->buffers[i];break;}
    if(!fallback)return;
    if(selected==b)selected=fallback;
    for(int i=0;i<e->pane_count;i++)if(e->panes[i].buffer==b->id){e->panes[i].buffer=fallback->id;e->panes[i].point=fallback->point;e->panes[i].top=e->panes[i].left=0;}
    for(size_t i=0;i<e->buffer_count;i++)if(e->buffers[i]==b){memmove(e->buffers+i,e->buffers+i+1,(e->buffer_count-i-1)*sizeof(Buffer*));e->buffer_count--;break;}
    free_buffer(b);editor_switch(e,selected);
}
void editor_destroy(Editor *e) {
    plugins_shutdown(e);
    for(size_t i=0;i<e->buffer_count;i++)free_buffer(e->buffers[i]);
    for(int i=0;i<e->kill_count;i++)free(e->kill_ring[i]);
    for(size_t i=0;i<e->macro_len;i++)free(e->macro_keys[i]);free(e->macro_keys);
    free(e->bindings);
}
static size_t relocate(size_t point,size_t at,size_t removed,size_t added,bool right) {
    if(point<at || (point==at && !right)) return point;
    if(point<=at+removed) return at+added;
    return point-removed+added;
}
bool editor_replace(Editor *e,size_t at,size_t count,const char *s,size_t n) {
    Buffer *b=editor_buffer(e); size_t size=text_size(&b->text);
    if(b->readonly) { editor_message(e,"Buffer is read-only"); return false; }
    if(at>size || count>size-at || !text_boundary(&b->text,at) || !text_boundary(&b->text,at+count) || !utf8_valid(s,n)) { editor_message(e,"Invalid UTF-8 edit or byte boundary"); return false; }
    if(!count && !n) return true;
    for(size_t j=b->history_pos;j<b->history_len;j++) edit_free(&b->history[j]);
    b->history_len=b->history_pos;
    if(b->history_len==b->history_cap) { b->history_cap=b->history_cap ? b->history_cap*2 : 64; b->history=xrealloc(b->history,b->history_cap*sizeof(Edit)); }
    Edit *d=&b->history[b->history_len++]; memset(d,0,sizeof(*d));
    d->at=at; d->removed_len=count; d->added_len=n; d->removed=text_slice(&b->text,at,count);
    d->added=xmalloc(n+1); memcpy(d->added,s,n); d->added[n]=0;
    d->before_point=b->point; d->before_mark=b->mark; d->group=e->group;
    d->before_state=b->state; d->after_state=++e->next_state;
    editor_lex_invalidate(b,at);
    text_delete(&b->text,at,count); text_insert(&b->text,at,s,n);
    b->point=relocate(b->point,at,count,n,true); b->mark=relocate(b->mark,at,count,n,false);
    d->after_point=b->point; d->after_mark=b->mark;
    b->state=d->after_state; b->revision++; b->history_pos=b->history_len;
    b->mark_active=false; b->goal_valid=false;
    plugins_event(e,"text_changed"); return true;
}
bool editor_insert(Editor *e,const char *s,size_t n) { return editor_replace(e,editor_buffer(e)->point,0,s,n); }
bool editor_set_text(Editor *e,const char *s,size_t n) {
    Buffer *b=editor_buffer(e); size_t size=text_size(&b->text),head=0,tail=0;
    if(!utf8_valid(s,n)) { editor_message(e,"Text must be UTF-8 without NUL bytes"); return false; }
    char *old=text_slice(&b->text,0,size);
    while(head<size && head<n && old[head]==s[head]) head++;
    while(head && head<n && ((unsigned char)s[head]&0xc0)==0x80) head--;
    while(tail<size-head && tail<n-head && old[size-tail-1]==s[n-tail-1]) tail++;
    while(tail && ((unsigned char)s[n-tail]&0xc0)==0x80) tail--;
    free(old); if(head==size && head==n) return true;
    return editor_replace(e,head,size-head-tail,s+head,n-head-tail);
}
static char *expand_path(const char *path) {
    if(path[0]=='~' && (path[1]=='/' || !path[1])) {
        const char *home=getenv("HOME"); if(!home) home=".";
        size_t n=strlen(home)+strlen(path)+2; char *s=xmalloc(n); snprintf(s,n,"%s%s",home,path+1); return s;
    }
    return xstrdup(path);
}
static char *absolute_path(const char *path) {
    char *s=expand_path(path); char *resolved=realpath(s,NULL);
    if(resolved) { free(s); return resolved; }
    if(s[0]=='/') return s;
    char cwd[PATH_MAX]; if(!getcwd(cwd,sizeof cwd)) return s;
    size_t n=strlen(s)+strlen(cwd)+2; char *out=xmalloc(n); snprintf(out,n,"%s/%s",cwd,s); free(s); return out;
}
bool editor_open(Editor *e,const char *path) {
    if(!path || !*path) { editor_message(e,"No file name supplied"); return false; }
    char *p=absolute_path(path);
    for(size_t i=0;i<e->buffer_count;i++) if(e->buffers[i]->path && !strcmp(p,e->buffers[i]->path)) { editor_switch(e,e->buffers[i]); free(p); return true; }
    FILE *f=fopen(p,"rb"); char *s=NULL; size_t n=0; struct stat st; bool known=false;
    if(!f && errno!=ENOENT) { editor_message(e,"Open failed: %s",strerror(errno)); free(p); return false; }
    if(f) {
        if(fstat(fileno(f),&st)!=0 || !S_ISREG(st.st_mode)) { editor_message(e,"Not a regular file"); fclose(f); free(p); return false; }
        if(st.st_size<0 || (uintmax_t)st.st_size>SIZE_MAX-1) { editor_message(e,"File is too large"); fclose(f); free(p); return false; }
        n=(size_t)st.st_size; s=xmalloc(n+1);
        if(fread(s,1,n,f)!=n) { editor_message(e,"Read failed: %s",strerror(errno)); fclose(f); free(s); free(p); return false; }
        fclose(f); s[n]=0; known=true;
    } else s=xstrdup("");
    bool bom=n>=3 && !memcmp(s,"\xef\xbb\xbf",3); if(bom) { memmove(s,s+3,n-3); n-=3; s[n]=0; }
    if(!utf8_valid(s,n)) { editor_message(e,"Unsupported encoding: use UTF-8 text without NUL bytes"); free(s); free(p); return false; }
    bool crlf=false, any_lf=false;
    for(size_t i=0;i<n;i++) if(s[i]=='\n') { if(i && s[i-1]=='\r') crlf=true; else any_lf=true; }
    if(crlf && !any_lf) { size_t j=0; for(size_t i=0;i<n;i++) { if(s[i]=='\r' && i+1<n && s[i+1]=='\n') continue; s[j++]=s[i]; } n=j; s[n]=0; }
    else crlf=false;
    const char *base=strrchr(p,'/'); Buffer *b=editor_new_buffer(e,base ? base+1 : p);
    if(!b) { free(s); free(p); return false; }
    text_free(&b->text); text_init(&b->text,s,n); free(s); b->path=p; b->bom=bom; b->crlf=crlf;
    b->disk_known=known; if(known) { b->disk_stat=st; b->readonly=access(p,W_OK)!=0; }
    editor_switch(e,b); e->panes[e->active_pane].top=e->panes[e->active_pane].left=0;
    plugins_event(e,"after_open"); editor_message(e,"%s%s",known ? "Opened " : "New file: ",p); return true;
}
static bool same_file_state(const struct stat *a,const struct stat *b) {
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_size==b->st_size && a->st_mtim.tv_sec==b->st_mtim.tv_sec && a->st_mtim.tv_nsec==b->st_mtim.tv_nsec;
}
bool editor_save(Editor *e,const char *path,bool force) {
    Buffer *b=editor_buffer(e);
    if(b->readonly && !force) { editor_message(e,"Buffer is read-only"); return false; }
    if(!path || !*path) path=b->path;
    if(!path || !*path) { editor_message(e,"Buffer has no file name; use C-x C-w"); return false; }
    char *p=absolute_path(path); struct stat now; bool exists=stat(p,&now)==0;
    bool same=b->path && !strcmp(b->path,p);
    if(!force && ((same && b->disk_known && (!exists || !same_file_state(&b->disk_stat,&now))) || (exists && (!same || !b->disk_known)))) {
        editor_message(e,"Refusing to overwrite an existing or externally changed file; use M-x force-save-buffer explicitly"); free(p); return false;
    }
    if(exists && !S_ISREG(now.st_mode)) { editor_message(e,"Not a regular file"); free(p); return false; }
    if(!plugins_before_save(e)) { free(p); return false; }
    size_t len=strlen(p)+24; char *temp=xmalloc(len); snprintf(temp,len,"%s.cpymacs-XXXXXX",p);
    int fd=mkstemp(temp);
    if(fd<0) { editor_message(e,"Save failed: %s",strerror(errno)); free(temp); free(p); return false; }
    if(exists) fchmod(fd,now.st_mode & 0777);
    else { mode_t m=umask(0); umask(m); fchmod(fd,0666 & ~m); }
    FILE *f=fdopen(fd,"wb"); bool ok=f!=NULL;
    if(f) {
        if(b->bom && fwrite("\xef\xbb\xbf",1,3,f)!=3) ok=false;
        if(ok) ok=text_write(&b->text,f,b->crlf);
        if(fflush(f)!=0 || fsync(fd)!=0) ok=false;
        if(fclose(f)!=0) ok=false;
    } else close(fd);
    if(ok && !force) {
        struct stat check; bool present=stat(p,&check)==0;
        if(present!=exists || (exists && !same_file_state(&now,&check))) { errno=EBUSY; ok=false; }
    }
    if(ok && rename(temp,p)!=0) ok=false;
    if(!ok) { int err=errno; unlink(temp); editor_message(e,"Save failed: %s",strerror(err)); free(temp); free(p); return false; }
    free(temp);
    char *directory=xstrdup(p), *slash=strrchr(directory,'/');
    if(slash) { if(slash==directory) slash[1]=0; else *slash=0; int dirfd=open(directory,O_RDONLY|O_DIRECTORY); if(dirfd>=0) { fsync(dirfd); close(dirfd); } }
    free(directory);
    bool changed=!b->path || strcmp(b->path,p);
    free(b->path); b->path=p; free(b->name); const char *base=strrchr(p,'/'); b->name=xstrdup(base?base+1:p);
    b->saved_state=b->state; b->disk_known=stat(p,&b->disk_stat)==0;
    if(changed) plugins_event(e,"after_open");
    plugins_event(e,"after_save"); editor_message(e,"Wrote %s",p); return true;
}
/* Width scans use bounded stack storage. Checkpoints make repeated typing and
 * horizontal scrolling in a very long line incremental instead of copying the
 * entire line on every frame. Edits before a checkpoint invalidate it. */
static size_t scan_columns(Buffer *b,size_t at,size_t end,size_t *column,
                           size_t target,bool limit) {
    char chunk[4096];
    while(at<end) {
        size_t n=end-at;if(n>sizeof chunk)n=sizeof chunk;
        while(n && !text_boundary(&b->text,at+n))n--;
        text_copy(&b->text,at,n,chunk);size_t i=0;
        while(i<n) {
            size_t k;uint32_t c=utf8_decode(chunk+i,n-i,&k);
            int width=wcwidth((wchar_t)c);
            if(width<0)width=c<32||c==127?2:1;
            size_t next=*column+(c=='\t'?(size_t)b->tab_width-*column%(size_t)b->tab_width:(size_t)width);
            if(limit && next>target)return at+i;
            *column=next;i+=k;
        }
        at+=n;
    }
    return at;
}
size_t editor_column(Buffer *b,size_t point) {
    size_t line=text_line_of(&b->text,point),start=text_line_start(&b->text,line),at=start,col=0;
    ColumnCache *cache=&b->column_cache;
    if(cache->valid && cache->line_start==start && cache->point<=point){at=cache->point;col=cache->column;}
    scan_columns(b,at,point,&col,0,false);
    if(point-start>4096)*cache=(ColumnCache){point,col,start,true};
    return col;
}
size_t editor_at_column_ex(Buffer *b,size_t line,size_t column,size_t *actual) {
    size_t start=text_line_start(&b->text,line),end=text_line_start(&b->text,line+1),at=start,col=0;
    if(end>start && text_byte(&b->text,end-1)=='\n')end--;
    ColumnCache *cache=&b->seek_cache;
    if(cache->valid && cache->line_start==start && cache->column<=column){at=cache->point;col=cache->column;}
    at=scan_columns(b,at,end,&col,column,true);
    if(at-start>4096)*cache=(ColumnCache){at,col,start,true};
    if(actual)*actual=col;return at;
}
size_t editor_at_column(Buffer *b,size_t line,size_t column) {
    return editor_at_column_ex(b,line,column,NULL);
}
/* Bounded-memory KMP over piece-table chunks: search never flattens the buffer. */
size_t editor_find(Buffer *b,const char *needle,size_t start,bool backward) {
    size_t m=strlen(needle),size=text_size(&b->text); if(!m) return start<=size ? start : size;
    if(start>size) start=size;
    size_t *pi=xmalloc(m*sizeof(size_t)); pi[0]=0;
    for(size_t i=1,j=0;i<m;i++) { while(j && needle[i]!=needle[j]) j=pi[j-1]; if(needle[i]==needle[j]) j++; pi[i]=j; }
    size_t off=backward?0:start,limit=backward?start:size,j=0,result=SIZE_MAX;
    char chunk[32768];
    while(off<limit) {
        size_t n=limit-off<sizeof chunk?limit-off:sizeof chunk; text_copy(&b->text,off,n,chunk);
        for(size_t k=0;k<n;k++) {
            while(j && chunk[k]!=needle[j]) j=pi[j-1]; if(chunk[k]==needle[j]) j++;
            if(j==m) { result=off+k+1-m; if(!backward) { free(pi); return result; } j=pi[j-1]; }
        }
        off+=n;
    }
    free(pi); return result;
}
