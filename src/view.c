#define _XOPEN_SOURCE 700
#include "editor.h"
#include "transport.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdint.h>

static void color_span(json_object *spans,size_t a,size_t z,int style) {
    if(a==z)return;size_t len=json_object_array_length(spans);
    if(len){json_object *last=json_object_array_get_idx(spans,len-1);if(json_object_get_int(json_object_array_get_idx(last,2))==style && (size_t)json_object_get_int64(json_object_array_get_idx(last,1))==a){json_object_array_put_idx(last,1,json_object_new_int64((int64_t)z));return;}}
    json_object *s=json_object_new_array();json_object_array_add(s,json_object_new_int64((int64_t)a));json_object_array_add(s,json_object_new_int64((int64_t)z));json_object_array_add(s,json_object_new_int(style));json_object_array_add(spans,s);
}
static json_object *visible_line(Editor *e,Buffer *b,size_t row,size_t left,int cols,size_t point) {
    json_object *result=json_object_new_object(),*styles=json_object_new_array();
    size_t line_start=text_line_start(&b->text,row),end=text_line_start(&b->text,row+1);
    if(end>line_start && text_byte(&b->text,end-1)=='\n')end--;
    size_t column=0,at=editor_at_column_ex(b,row,left,&column),rawlen=end-at;
    if(rawlen>(size_t)cols*8+8)rawlen=(size_t)cols*8+8;
    while(rawlen && !text_boundary(&b->text,at+rawlen))rawlen--;
    char *raw=text_slice(&b->text,at,rawlen),*display=xmalloc((size_t)cols*8+16);size_t out=0,i=0;
    json_object *lex=NULL;size_t spanindex=0;
    /* Large-file and pathological-line guards bound interactive lexing work. */
    if(e->show_highlight && text_size(&b->text)<=16u*1024u*1024u && end-line_start<=256u*1024u) {
        char *whole=text_slice(&b->text,line_start,end-line_start);lex=editor_line_spans(b,row,whole,end-line_start);free(whole);
    }
    size_t sel_a=b->mark<point?b->mark:point,sel_z=b->mark>point?b->mark:point;
    while(i<rawlen && column<left+(size_t)cols) {
        size_t k;uint32_t ch=utf8_decode(raw+i,rawlen-i,&k);int width=wcwidth((wchar_t)ch);if(width<0)width=ch<32||ch==127?2:1;
        if(ch=='\t')width=b->tab_width-(int)(column%(size_t)b->tab_width);
        int style=0;size_t rel=at+i-line_start;
        if(lex) {
            while(spanindex<json_object_array_length(lex) && (size_t)json_object_get_int64(json_object_array_get_idx(json_object_array_get_idx(lex,spanindex),1))<=rel)spanindex++;
            if(spanindex<json_object_array_length(lex)){json_object *s=json_object_array_get_idx(lex,spanindex);if((size_t)json_object_get_int64(json_object_array_get_idx(s,0))<=rel)style=json_object_get_int(json_object_array_get_idx(s,2));}
        }
        if(b->mark_active && at+i>=sel_a && at+i<sel_z)style=8;
        size_t old=out;
        if(ch=='\t') {for(int w=0;w<width;w++)if(column+(size_t)w>=left && column+(size_t)w<left+(size_t)cols)display[out++]=' ';}
        else if(column>=left && column+(size_t)width<=left+(size_t)cols) {
            if(ch<32 || ch==127){display[out++]='^';display[out++]=ch==127?'?':(char)(ch+64);}else{memcpy(display+out,raw+i,k);out+=k;}
        } else if(column>=left){for(size_t w=column;w<left+(size_t)cols;w++)display[out++]=' ';}
        color_span(styles,old,out,style);column+=(size_t)width;i+=k;
    }
    display[out]=0;json_set_string(result,"text",display);json_set_int(result,"line",(int64_t)row);json_set_int(result,"offset",(int64_t)at);json_object_object_add(result,"spans",styles);
    free(raw);free(display);if(lex)json_object_put(lex);return result;
}
json_object *editor_snapshot(Editor *e,int rows,int cols) {
    if(rows<4)rows=4;if(rows>200)rows=200;if(cols<12)cols=12;if(cols>500)cols=500;e->rows=rows;e->cols=cols;
    json_object *out=json_object_new_object(),*panes=json_object_new_array(),*colors=json_object_new_array();
    json_set_int(out,"protocol",1);json_set_int(out,"kill_generation",(int64_t)e->kill_generation);json_set_int(out,"rows",rows);json_set_int(out,"cols",cols);json_set_string(out,"message",e->message);
    json_set_string(out,"prompt",e->prompt);json_set_string(out,"input",e->input);json_set_int(out,"input_point",(int64_t)e->input_point);
    json_set_string(out,"prefix",e->key_prefix);json_object_object_add(out,"quit",json_object_new_boolean(e->quit));json_object_object_add(out,"detach",json_object_new_boolean(e->detach));
    json_set_string(out,"theme",e->theme);json_set_string(out,"font",e->font);json_set_int(out,"font_size",e->font_size);
    for(int i=0;i<10;i++)json_object_array_add(colors,json_object_new_int64((int64_t)e->colors[i]));json_object_object_add(out,"colors",colors);
    Buffer *current=editor_buffer(e);e->panes[e->active_pane].point=current->point;
    int pane_count=e->pane_count;
    if(e->split_vertical && cols/pane_count<12)pane_count=1;
    if(!e->split_vertical && (rows-1)/pane_count<3)pane_count=1;
    for(int visible=0;visible<pane_count;visible++) {
        int index=pane_count==1?e->active_pane:visible;Pane *p=&e->panes[index];Buffer *b=editor_by_id(e,p->buffer);if(!b)continue;
        size_t point=index==e->active_pane?b->point:p->point;if(point>text_size(&b->text))point=text_size(&b->text);
        int x=e->split_vertical?(visible*cols)/pane_count:0,y=e->split_vertical?0:(visible*(rows-1))/pane_count;
        int width=e->split_vertical?((visible+1)*cols)/pane_count-x:cols;
        int height=e->split_vertical?rows-1:((visible+1)*(rows-1))/pane_count-y;
        int gutter=e->show_line_numbers?8:0;if(gutter>width/2)gutter=0;int textcols=width-gutter,textrows=height-1;
        size_t row=text_line_of(&b->text,point),column=editor_column(b,point);
        if(index==e->active_pane){if(row<p->top)p->top=row;if(row>=p->top+(size_t)textrows)p->top=row-(size_t)textrows+1;if(column<p->left)p->left=column;if(column>=p->left+(size_t)textcols)p->left=column-(size_t)textcols+1;}
        json_object *pane=json_object_new_object(),*lines=json_object_new_array();
        json_set_int(pane,"index",index);json_set_int(pane,"id",b->id);json_set_int(pane,"x",x);json_set_int(pane,"y",y);json_set_int(pane,"width",width);json_set_int(pane,"height",height);json_set_int(pane,"gutter",gutter);
        json_set_int(pane,"top",(int64_t)p->top);json_set_int(pane,"left",(int64_t)p->left);json_set_int(pane,"point",(int64_t)point);json_set_int(pane,"mark",(int64_t)b->mark);
        json_set_int(pane,"size",(int64_t)text_size(&b->text));json_set_int(pane,"characters",(int64_t)text_chars(&b->text));json_set_int(pane,"line_count",(int64_t)text_lines(&b->text));json_set_int(pane,"revision",(int64_t)b->revision);
        json_set_int(pane,"cursor_row",(int64_t)row-(int64_t)p->top);json_set_int(pane,"cursor_col",(int64_t)column-(int64_t)p->left+gutter);
        json_set_int(pane,"tab_width",b->tab_width);json_set_int(pane,"indent_width",b->indent_width);
        json_set_string(pane,"name",b->name);json_set_string(pane,"path",b->path?b->path:"");json_set_string(pane,"mode",b->mode);
        json_object_object_add(pane,"active",json_object_new_boolean(index==e->active_pane));json_object_object_add(pane,"modified",json_object_new_boolean(b->state!=b->saved_state));
        json_object_object_add(pane,"mark_active",json_object_new_boolean(b->mark_active));json_object_object_add(pane,"readonly",json_object_new_boolean(b->readonly));
        json_object_object_add(pane,"auto_indent",json_object_new_boolean(b->auto_indent));
        for(int r=0;r<textrows;r++) {
            size_t physical=p->top+(size_t)r;
            if(physical>=text_lines(&b->text)){json_object *empty=json_object_new_object();json_set_string(empty,"text","");json_set_int(empty,"line",-1);json_object_object_add(empty,"spans",json_object_new_array());json_object_array_add(lines,empty);}
            else json_object_array_add(lines,visible_line(e,b,physical,p->left,textcols,point));
        }
        char mode[512];snprintf(mode,sizeof mode,"%s %s %.120s  %zu:%zu  %.80s  %s",b->readonly?"%%-":b->state!=b->saved_state?"**":"--",index==e->active_pane?">":" ",b->name,row+1,column,b->mode,b->crlf?"CRLF":"LF");json_set_string(pane,"modeline",mode);
        json_object_object_add(pane,"lines",lines);json_object_array_add(panes,pane);
    }
    json_object_object_add(out,"panes",panes);return out;
}
