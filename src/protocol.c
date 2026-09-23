#include "editor.h"
#include "transport.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static bool field_type(json_object *req,const char *name,enum json_type type,bool required) {
    json_object *value=json_field(req,name);return value?json_object_is_type(value,type):!required;
}
static json_object *failure(const char *message) {
    json_object *r=json_object_new_object();json_object_object_add(r,"ok",json_object_new_boolean(false));json_set_string(r,"error",message);return r;
}
static bool safe_strings(json_object *value) {
    if(!value)return true;
    if(json_object_is_type(value,json_type_string))
        return utf8_valid(json_object_get_string(value),(size_t)json_object_get_string_len(value));
    if(json_object_is_type(value,json_type_array)) {
        for(size_t i=0;i<json_object_array_length(value);i++)if(!safe_strings(json_object_array_get_idx(value,i)))return false;
    } else if(json_object_is_type(value,json_type_object)) {
        json_object_object_foreach(value,key,child){(void)key;if(!safe_strings(child))return false;}
    }
    return true;
}
json_object *editor_request(Editor *e,json_object *req) {
    if(!req || !json_object_is_type(req,json_type_object) || !field_type(req,"op",json_type_string,true))return failure("Request must be an object with a string op");
    if(!safe_strings(req))return failure("NUL bytes are not permitted in protocol strings");
    const char *op=json_string(req,"op","");bool ok=true;json_object *result=NULL;e->detach=false;
    int64_t r64=json_int(req,"rows",e->rows),c64=json_int(req,"cols",e->cols);
    int rows=(int)(r64<4?4:r64>200?200:r64),cols=(int)(c64<12?12:c64>500?500:c64);
    if(!strcmp(op,"snapshot") || !strcmp(op,"hello")){}
    else if(!strcmp(op,"key")) {
        if(!field_type(req,"key",json_type_string,true))return failure("key must be a string");ok=editor_key(e,json_string(req,"key",""));
    }else if(!strcmp(op,"keys")) {
        json_object *keys=json_field(req,"keys");if(!keys || !json_object_is_type(keys,json_type_array) || json_object_array_length(keys)>10000)return failure("keys must be an array of at most 10000 strings");
        for(size_t i=0;i<json_object_array_length(keys);i++){json_object *k=json_object_array_get_idx(keys,i);if(!json_object_is_type(k,json_type_string))return failure("Every key must be a string");if(!editor_key(e,json_object_get_string(k)))ok=false;}
    }else if(!strcmp(op,"insert")) {
        if(!field_type(req,"text",json_type_string,true))return failure("text must be a string");
        json_object *text=json_field(req,"text");const char *s=json_object_get_string(text);size_t n=(size_t)json_object_get_string_len(text);e->group++;e->last_kind=0;
        ok=utf8_valid(s,n) && editor_paste(e,s);
    }else if(!strcmp(op,"command")) {
        if(!field_type(req,"name",json_type_string,true) || !field_type(req,"argument",json_type_string,false) || !field_type(req,"count",json_type_int,false))return failure("command requires name; argument must be string and count integer");
        int64_t count=json_int(req,"count",1);if(count<LONG_MIN || count>LONG_MAX)return failure("count is out of range");
        ok=editor_command(e,json_string(req,"name",""),json_string(req,"argument",NULL),(long)count,json_field(req,"count")!=NULL);
    }else if(!strcmp(op,"open")) {
        if(!field_type(req,"path",json_type_string,true))return failure("path must be a string");e->group++;ok=editor_open(e,json_string(req,"path",""));
    }else if(!strcmp(op,"save")) {
        if(!field_type(req,"path",json_type_string,false))return failure("path must be a string");e->group++;ok=editor_save(e,json_string(req,"path",NULL),jbool(req,"force",false));
    }else if(!strcmp(op,"get_text")) {
        Buffer *b=editor_buffer(e);int64_t at=json_int(req,"start",0),end=json_int(req,"end",(int64_t)text_size(&b->text));
        if(at<0 || end<at || (uint64_t)end>text_size(&b->text) || (uint64_t)(end-at)>2u*1024u*1024u || !text_boundary(&b->text,(size_t)at) || !text_boundary(&b->text,(size_t)end))return failure("Invalid byte range, or range exceeds 2 MiB; request chunks");
        char *s=text_slice(&b->text,(size_t)at,(size_t)(end-at));result=json_object_new_string_len(s,(int)(end-at));free(s);
    }else if(!strcmp(op,"replace")) {
        if(!field_type(req,"text",json_type_string,true)||!field_type(req,"start",json_type_int,true)||!field_type(req,"end",json_type_int,true))return failure("replace requires integer start/end and string text");
        int64_t a=json_int(req,"start",-1),z=json_int(req,"end",-1);if(a<0 || z<a)return failure("Invalid byte range");json_object *text=json_field(req,"text");e->group++;ok=editor_replace(e,(size_t)a,(size_t)(z-a),json_object_get_string(text),(size_t)json_object_get_string_len(text));
    }else if(!strcmp(op,"set_text")) {
        if(!field_type(req,"text",json_type_string,true))return failure("text must be a string");json_object *text=json_field(req,"text");e->group++;ok=editor_set_text(e,json_object_get_string(text),(size_t)json_object_get_string_len(text));
    }else if(!strcmp(op,"point")) {
        Buffer *b=editor_buffer(e);int64_t at=json_int(req,"byte",-1);
        if(at<0 || !text_boundary(&b->text,(size_t)at))return failure("Point must be a valid UTF-8 byte boundary");b->point=(size_t)at;b->goal_valid=false;
    }else if(!strcmp(op,"click")) {
        int pane=(int)json_int(req,"pane",e->active_pane);int64_t row=json_int(req,"line",0),col=json_int(req,"column",0);
        if(pane<0 || pane>=e->pane_count || row<0 || col<0)return failure("Invalid click coordinates");
        e->panes[e->active_pane].point=editor_buffer(e)->point;e->active_pane=pane;Buffer *b=editor_by_id(e,e->panes[pane].buffer);editor_switch(e,b);
        if(jbool(req,"extend",false)){if(!b->mark_active){b->mark=b->point;b->has_mark=b->mark_active=true;}}
        else b->mark_active=false;b->point=editor_at_column(b,(size_t)row,(size_t)col);b->goal_valid=false;
    }else if(!strcmp(op,"commands"))result=editor_command_names(e);
    else if(!strcmp(op,"selection")) {
        Buffer *b=editor_buffer(e);if(!b->mark_active)result=json_object_new_string("");else{size_t a=b->point<b->mark?b->point:b->mark,z=b->point>b->mark?b->point:b->mark;if(z-a>2u*1024u*1024u)return failure("Selection exceeds 2 MiB");char *s=text_slice(&b->text,a,z-a);result=json_object_new_string_len(s,(int)(z-a));free(s);}
    }else if(!strcmp(op,"kill_ring")) {
        const char *s=e->kill_count?e->kill_ring[0]:"";
        if(strlen(s)>2u*1024u*1024u)return failure("Clipboard transfer exceeds 2 MiB");
        result=json_object_new_string(s);
    }else if(!strcmp(op,"yank_external")) {
        if(!field_type(req,"text",json_type_string,true))return failure("text must be a string");
        const char *s=json_string(req,"text","");
        ok=*e->prompt_command?editor_paste(e,s):editor_external_yank(e,s);
    }else if(!strcmp(op,"quit"))ok=editor_command(e,jbool(req,"force",false)?"kill-emacs":"save-buffers-kill-terminal",NULL,1,false);
    else return failure("Unknown operation");
    json_object *response=json_object_new_object();json_object_object_add(response,"ok",json_object_new_boolean(ok));if(!ok)json_set_string(response,"error",e->message);
    json_object *id=json_field(req,"id");if(id)json_object_object_add(response,"id",json_object_get(id));if(result)json_object_object_add(response,"result",result);
    if(!jbool(req,"no_snapshot",false))json_object_object_add(response,"state",editor_snapshot(e,rows,cols));return response;
}
