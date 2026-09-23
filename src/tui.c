#define _GNU_SOURCE
#include "transport.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <locale.h>
#include <errno.h>

static struct termios saved_terminal;
static bool terminal_active;
static volatile sig_atomic_t stopping;
static void restore(void) {
    if(terminal_active){tcsetattr(STDIN_FILENO,TCSAFLUSH,&saved_terminal);fputs("\033[0m\033[?25h\033[?2004l\033[?1049l",stdout);fflush(stdout);terminal_active=false;}
}
static void stop(int signo){(void)signo;stopping=1;}
static void color(FILE *out,unsigned long rgb,bool background){fprintf(out,"\033[%d;2;%lu;%lu;%lum",background?48:38,(rgb>>16)&255,(rgb>>8)&255,rgb&255);}
static int safe_print(FILE *out,const char *s,size_t n,int limit) {
    int columns=0;mbstate_t state={0};
    for(size_t i=0;i<n && columns<limit;){wchar_t wc;size_t k=mbrtowc(&wc,s+i,n-i,&state);if(k==(size_t)-1 || k==(size_t)-2 || !k){memset(&state,0,sizeof state);k=1;wc=L'?';}
        int w=wcwidth(wc);if(w<0){if(columns+2>limit)break;fputc('^',out);fputc(wc==127?'?':(int)((unsigned)wc+64)%128,out);columns+=2;}
        else {if(columns+w>limit)break;fwrite(s+i,1,k,out);columns+=w;}i+=k;
    }return columns;
}
static void print_spaces(FILE *out,int n){while(n-->0)fputc(' ',out);}
static void draw_state(json_object *state,char **cache,int *oldrows,int *oldcols) {
    int rows=(int)json_int(state,"rows",24),cols=(int)json_int(state,"cols",80);unsigned long colors[10]={0};json_object *palette=json_field(state,"colors");
    for(int i=0;i<10;i++)colors[i]=(unsigned long)json_object_get_int64(json_object_array_get_idx(palette,(size_t)i));
    if(rows!=*oldrows || cols!=*oldcols){fputs("\033[2J",stdout);for(int i=0;i<200;i++){free(cache[i]);cache[i]=NULL;}*oldrows=rows;*oldcols=cols;}
    json_object *panes=json_field(state,"panes");int cursor_y=0,cursor_x=0;
    for(int screenrow=0;screenrow<rows;screenrow++) {
        char *line=NULL;size_t length=0;FILE *out=open_memstream(&line,&length);if(!out)return;
        if(screenrow==rows-1){fprintf(out,"\033[%d;1H",rows);color(out,colors[0],false);color(out,colors[1],true);
            const char *prompt=json_string(state,"prompt","");int used=0;
            if(*prompt){used+=safe_print(out,prompt,strlen(prompt),cols);const char *input=json_string(state,"input","");used+=safe_print(out,input,strlen(input),cols-used);}
            else{const char *message=json_string(state,"message","");used=safe_print(out,message,strlen(message),cols);}print_spaces(out,cols-used);
        }else for(size_t i=0;i<json_object_array_length(panes);i++) {
            json_object *p=json_object_array_get_idx(panes,i);int x=(int)json_int(p,"x",0),y=(int)json_int(p,"y",0),width=(int)json_int(p,"width",cols),height=(int)json_int(p,"height",rows-1),gutter=(int)json_int(p,"gutter",0);
            if(screenrow<y || screenrow>=y+height)continue;
            fprintf(out,"\033[%d;%dH",screenrow+1,x+1);color(out,colors[0],false);color(out,colors[screenrow==y+height-1?9:1],true);
            int used=0;
            if(screenrow==y+height-1){const char *s=json_string(p,"modeline","");used=safe_print(out,s,strlen(s),width);}
            else{
                json_object *row=json_object_array_get_idx(json_field(p,"lines"),(size_t)(screenrow-y));
                if(gutter){char text[32];int64_t n=json_int(row,"line",-1);if(n>=0)snprintf(text,sizeof text,"%6lld  ",(long long)n+1);else strcpy(text,"        ");color(out,colors[2],false);safe_print(out,text,strlen(text),gutter);used=gutter;}
                const char *text=json_string(row,"text","");size_t n=strlen(text);json_object *spans=json_field(row,"spans");
                if(spans && json_object_array_length(spans))for(size_t j=0;j<json_object_array_length(spans);j++) {
                    json_object *span=json_object_array_get_idx(spans,j);size_t a=(size_t)json_object_get_int64(json_object_array_get_idx(span,0)),z=(size_t)json_object_get_int64(json_object_array_get_idx(span,1));int style=json_object_get_int(json_object_array_get_idx(span,2));
                    if(a>z || z>n || style<0 || style>9)continue;color(out,colors[style==8?0:style],false);color(out,colors[style==8?8:1],true);used+=safe_print(out,text+a,z-a,width-used);
                }else{color(out,colors[0],false);used+=safe_print(out,text,n,width-used);}
            }
            color(out,colors[screenrow==y+height-1?9:1],true);print_spaces(out,width-used);
            if(jbool(p,"active",false)){cursor_y=y+(int)json_int(p,"cursor_row",0);cursor_x=x+(int)json_int(p,"cursor_col",0);}
        }
        fclose(out);
        if(!cache[screenrow] || strcmp(cache[screenrow],line)){fwrite(line,1,length,stdout);free(cache[screenrow]);cache[screenrow]=line;}else free(line);
    }
    const char *prompt=json_string(state,"prompt","");if(*prompt){FILE *sink=fopen("/dev/null","w");const char *input=json_string(state,"input","");size_t point=(size_t)json_int(state,"input_point",0);if(point>strlen(input))point=strlen(input);cursor_y=rows-1;cursor_x=safe_print(sink,prompt,strlen(prompt),cols)+safe_print(sink,input,point,cols);fclose(sink);}
    if(cursor_y<0)cursor_y=0;if(cursor_y>=rows)cursor_y=rows-1;if(cursor_x<0)cursor_x=0;if(cursor_x>=cols)cursor_x=cols-1;
    fprintf(stdout,"\033[%d;%dH\033[?25h",cursor_y+1,cursor_x+1);fflush(stdout);
}
static int get_byte(int timeout) {
    struct pollfd p={STDIN_FILENO,POLLIN,0};int result=poll(&p,1,timeout);if(result<=0)return -1;unsigned char c;return read(STDIN_FILENO,&c,1)==1?c:-1;
}
static void base_key(int c,char *key) {
    if(c==0)strcpy(key,"C-SPC");else if(c==9)strcpy(key,"TAB");else if(c==13)strcpy(key,"RET");else if(c==127)strcpy(key,"DEL");
    else if(c>0 && c<27)snprintf(key,80,"C-%c",'a'+c-1);else if(c==31)strcpy(key,"C-_");else if(c<32)snprintf(key,80,"C-%c",c+64);else{key[0]=(char)c;key[1]=0;}
}
static json_object *read_event(void) {
    int c=get_byte(0);if(c<0)return NULL;char key[80]={0};
    if(c==27) {
        int next=get_byte(60);if(next<0){strcpy(key,"ESC");}
        else if(next=='[' || next=='O') {
            char seq[64]={0};size_t n=0;int ch;
            while(n<sizeof seq-1 && (ch=get_byte(60))>=0){seq[n++]=(char)ch;if(ch>=0x40 && ch<=0x7e)break;}
            if(next=='[' && !strcmp(seq,"200~")) {
                char *paste=malloc(4096);if(!paste)return NULL;size_t len=0,cap=4096;
                while(len<CP_MAX_FRAME-1024){int b=get_byte(2000);if(b<0)break;if(len+2>=cap){cap*=2;char *tmp=realloc(paste,cap);if(!tmp){free(paste);return NULL;}paste=tmp;}paste[len++]=(char)b;if(len>=6 && !memcmp(paste+len-6,"\033[201~",6)){len-=6;break;}}
                paste[len]=0;json_object *r=request_new("insert");json_object_object_add(r,"text",json_object_new_string_len(paste,(int)len));free(paste);return r;
            }
            const char *mapped=NULL;
            if(!strcmp(seq,"A"))mapped="<up>";else if(!strcmp(seq,"B"))mapped="<down>";else if(!strcmp(seq,"C"))mapped="<right>";else if(!strcmp(seq,"D"))mapped="<left>";
            else if(!strcmp(seq,"H")||!strcmp(seq,"1~")||!strcmp(seq,"7~"))mapped="<home>";else if(!strcmp(seq,"F")||!strcmp(seq,"4~")||!strcmp(seq,"8~"))mapped="<end>";
            else if(!strcmp(seq,"3~"))mapped="<delete>";else if(!strcmp(seq,"5~"))mapped="<prior>";else if(!strcmp(seq,"6~"))mapped="<next>";
            else if(!strcmp(seq,"1;5D"))mapped="M-b";else if(!strcmp(seq,"1;5C"))mapped="M-f";
            else if(!strcmp(seq,"P"))mapped="<f1>";else if(!strcmp(seq,"Q"))mapped="<f2>";else if(!strcmp(seq,"R"))mapped="<f3>";else if(!strcmp(seq,"S"))mapped="<f4>";
            else if(!strcmp(seq,"15~"))mapped="<f5>";else if(!strcmp(seq,"17~"))mapped="<f6>";else if(!strcmp(seq,"18~"))mapped="<f7>";else if(!strcmp(seq,"19~"))mapped="<f8>";else if(!strcmp(seq,"20~"))mapped="<f9>";else if(!strcmp(seq,"21~"))mapped="<f10>";else if(!strcmp(seq,"23~"))mapped="<f11>";else if(!strcmp(seq,"24~"))mapped="<f12>";
            snprintf(key,sizeof key,"%s",mapped?mapped:"<unknown>");
        }else{char base[80];base_key(next,base);snprintf(key,sizeof key,"M-%.70s",base);}
    }else if(c>=0xc2 && c<=0xf4) {
        size_t n=c<0xe0?2:c<0xf0?3:4;key[0]=(char)c;
        for(size_t i=1;i<n;i++){int b=get_byte(100);if(b<0){key[i]=0;break;}key[i]=(char)b;}
    }else base_key(c,key);
    json_object *r=request_new("key");json_set_string(r,"key",key);return r;
}
int tui_run(Connection *connection) {
    if(!isatty(STDIN_FILENO)||!isatty(STDOUT_FILENO)){fputs("cpymacs: terminal frontend needs a TTY; use --batch for JSON Lines\n",stderr);return 2;}
    setlocale(LC_ALL,"");if(tcgetattr(STDIN_FILENO,&saved_terminal)!=0)return 1;struct termios raw=saved_terminal;cfmakeraw(&raw);raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
    if(tcsetattr(STDIN_FILENO,TCSAFLUSH,&raw)!=0)return 1;terminal_active=true;atexit(restore);signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGHUP,stop);
    fputs("\033[?1049h\033[?2004h\033[2J",stdout);fflush(stdout);char *cache[200]={0};int oldrows=0,oldcols=0,result=0;
    while(!stopping) {
        struct winsize size={0};ioctl(STDOUT_FILENO,TIOCGWINSZ,&size);int rows=size.ws_row?size.ws_row:24,cols=size.ws_col?size.ws_col:80;
        json_object *req=NULL;struct pollfd p={STDIN_FILENO,POLLIN,0};int ready=poll(&p,1,oldrows?200:0);
        if(ready>0){if(p.revents&(POLLHUP|POLLERR)){break;}req=read_event();}
        if(!req)req=request_new("snapshot");json_set_int(req,"rows",rows);json_set_int(req,"cols",cols);
        json_object *response=connection_request(connection,req);json_object_put(req);
        if(!response){result=1;break;}json_object *state=json_field(response,"state");
        if(state){draw_state(state,cache,&oldrows,&oldcols);if(jbool(state,"quit",false)||jbool(state,"detach",false)){json_object_put(response);break;}}
        json_object_put(response);
    }
    restore();for(int i=0;i<200;i++)free(cache[i]);if(result)fputs("cpymacs: backend disconnected\n",stderr);return result;
}
