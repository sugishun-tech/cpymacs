#include "editor.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Plugins supply lexical policy; scanning and cache invalidation stay in C.
 * Each cached entry is the lexical state at the beginning of a physical line. */
enum { NORMAL, SINGLE, DOUBLE, TRIPLE_SINGLE, TRIPLE_DOUBLE, BLOCK_COMMENT,
       HTML_COMMENT, JINJA_COMMENT, BACKTICK };
static bool has_word(const char *list,const char *word,size_t n) {
    if(!list)return false;const char *p=list;
    while(*p){while(*p==' ')p++;const char *a=p;while(*p && *p!=' ')p++;if((size_t)(p-a)==n && !memcmp(a,word,n))return true;}return false;
}
static void span(json_object *a,size_t start,size_t end,int style) {
    if(!a || end<=start)return;json_object *s=json_object_new_array();
    json_object_array_add(s,json_object_new_int64((int64_t)start));json_object_array_add(s,json_object_new_int64((int64_t)end));json_object_array_add(s,json_object_new_int(style));json_object_array_add(a,s);
}
static unsigned scan(Buffer *b,const char *s,size_t n,unsigned state,json_object *out) {
    int kind=b->syntax.kind;if(!kind)return NORMAL;
    size_t i=0;
    while(i<n) {
        size_t start=i;
        if(state!=NORMAL) {
            const char *close=state==TRIPLE_SINGLE?"'''":state==TRIPLE_DOUBLE?"\"\"\"":state==BLOCK_COMMENT?"*/":state==HTML_COMMENT?"-->":state==JINJA_COMMENT?"#}":state==SINGLE?"'":state==DOUBLE?"\"":"`";
            size_t len=strlen(close);bool comment=state==BLOCK_COMMENT || state==HTML_COMMENT || state==JINJA_COMMENT;
            while(i<n) {
                if(i+len<=n && !memcmp(s+i,close,len)){i+=len;state=NORMAL;break;}
                if(!comment && s[i]=='\\' && i+1<n)i+=2;else i++;
            }
            span(out,start,i,comment?2:4);continue;
        }
        if((kind==1 || kind==5 || (kind==2 && !strcmp(b->syntax.name,"php"))) && s[i]=='#') {span(out,i,n,2);return NORMAL;}
        if(kind!=1 && i+2<=n && !memcmp(s+i,"//",2)){span(out,i,n,2);return NORMAL;}
        if(kind!=1 && i+2<=n && !memcmp(s+i,"/*",2)){state=BLOCK_COMMENT;span(out,i,i+2,2);i+=2;continue;}
        if((kind==3 || kind==5) && i+4<=n && !memcmp(s+i,"<!--",4)){state=HTML_COMMENT;span(out,i,i+4,2);i+=4;continue;}
        if((kind==3 || kind==5) && i+2<=n && !memcmp(s+i,"{#",2)){state=JINJA_COMMENT;span(out,i,i+2,2);i+=2;continue;}
        if(s[i]=='\'' || s[i]=='"' || (kind!=1 && s[i]=='`')) {
            char q=s[i];size_t len=1;
            if(kind==1 && i+3<=n && s[i+1]==q && s[i+2]==q){state=q=='\''?TRIPLE_SINGLE:TRIPLE_DOUBLE;len=3;}
            else state=q=='\''?SINGLE:q=='"'?DOUBLE:BACKTICK;
            span(out,i,i+len,4);i+=len;continue;
        }
        if(isdigit((unsigned char)s[i]) && (i==0 || (!isalnum((unsigned char)s[i-1]) && s[i-1]!='_'))) {
            i++;while(i<n && (isalnum((unsigned char)s[i]) || s[i]=='_' || s[i]=='.'))i++;span(out,start,i,7);continue;
        }
        if((kind==3 || kind==5) && s[i]=='<') {
            i++;if(i<n && s[i]=='/')i++;while(i<n && (isalnum((unsigned char)s[i])||s[i]=='-'||s[i]==':'||s[i]=='!'))i++;span(out,start,i,6);continue;
        }
        if((kind==3 || kind==5) && i+2<=n && ((!memcmp(s+i,"{{",2))||(!memcmp(s+i,"}}",2))||(!memcmp(s+i,"{%",2))||(!memcmp(s+i,"%}",2)))){span(out,i,i+2,5);i+=2;continue;}
        if(isalpha((unsigned char)s[i]) || s[i]=='_' || (unsigned char)s[i]>=128) {
            i++;while(i<n && (isalnum((unsigned char)s[i])||s[i]=='_'||(unsigned char)s[i]>=128))i++;
            int style=has_word(b->syntax.keywords,s+start,i-start)?5:has_word(b->syntax.builtins,s+start,i-start)?6:0;
            size_t after=i;while(after<n && (s[after]==' '||s[after]=='\t'))after++;
            if(!style && after<n && s[after]=='(')style=6;
            if(style)span(out,start,i,style);continue;
        }
        if(kind==1 && s[i]=='@') {i++;while(i<n && (isalnum((unsigned char)s[i])||s[i]=='_'||s[i]=='.'))i++;span(out,start,i,3);continue;}
        i++;
    }
    if((state==SINGLE || state==DOUBLE) && (n==0 || s[n-1]!='\\'))state=NORMAL;
    return state;
}
void editor_lex_invalidate(Buffer *b,size_t at) {
    if(at<b->column_cache.point)b->column_cache.valid=false;
    if(at<b->seek_cache.point)b->seek_cache.valid=false;
    size_t row=text_line_of(&b->text,at);
    if(b->lex_valid>row+1)b->lex_valid=row+1;
}
static unsigned state_at(Buffer *b,size_t row) {
    if(row+2>b->lex_capacity){size_t cap=b->lex_capacity?b->lex_capacity:256;while(cap<row+2)cap*=2;b->lex_states=xrealloc(b->lex_states,cap);b->lex_capacity=cap;}
    if(!b->lex_valid){b->lex_states[0]=NORMAL;b->lex_valid=1;}
    while(b->lex_valid<=row) {
        size_t line=b->lex_valid-1,a=text_line_start(&b->text,line),z=text_line_start(&b->text,line+1);
        if(z>a && text_byte(&b->text,z-1)=='\n')z--;
        char *s=text_slice(&b->text,a,z-a);b->lex_states[b->lex_valid]=(unsigned char)scan(b,s,z-a,b->lex_states[line],NULL);free(s);b->lex_valid++;
    }
    return b->lex_states[row];
}
json_object *editor_line_spans(Buffer *b,size_t row,const char *line,size_t n) {
    json_object *a=json_object_new_array();
    if(!b->syntax.kind)return a;
    unsigned state=state_at(b,row);unsigned end=scan(b,line,n,state,a);
    if(row+1<text_lines(&b->text) && row+1==b->lex_valid) {b->lex_states[row+1]=(unsigned char)end;b->lex_valid++;}
    return a;
}
