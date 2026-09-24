#define _GNU_SOURCE
#include "editor.h"
#include "transport.h"
#include "term_style.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <locale.h>

#ifndef CPYMACS_PYTHON_DIR
#define CPYMACS_PYTHON_DIR "python"
#endif
#ifndef CPYMACS_INSTALL_DATA
#define CPYMACS_INSTALL_DATA "/usr/local/share/cpymacs/python"
#endif
static volatile sig_atomic_t interrupted;
static void stop_handler(int signal_number){(void)signal_number;interrupted=1;}
static void usage(void) {
    puts("cpymacs 0.1.1\nUsage: cpymacs [OPTIONS] [FILE ...]\n"
         "  -nw, -nox, --nox, nox       Use the independent native terminal frontend\n"
         "  --tui-colors MODE           Terminal colours: auto (default), truecolor, 256, mono\n"
         "  --tui-cursor MODE           Terminal cursor: block (default), terminal\n"
         "  --backend, --batch          JSON Lines backend on stdin/stdout\n"
         "  --server SOCKET             Persistent local backend (Unix socket, mode 0600)\n"
         "  --connect SOCKET            Attach frontend to an existing backend\n"
         "  --config FILE               Load trusted Python configuration (repeatable)\n"
         "  -q, --no-user-config        Skip ~/.config/cpymacs/init.py\n"
         "  --no-python                 Disable Python and all plugins\n"
         "  --help                      Show this help\n"
         "  --version                   Show version\n\n"
         "Without --nox, launch cpymacs-gui when DISPLAY is set; otherwise use the TUI.\n"
         "Explicit --tui-colors or --tui-cursor options also imply --nox.\n"
         "Defaults: CPYMACS_TUI_COLORS=auto, CPYMACS_TUI_CURSOR=block.\n"
         "An existing backend owns its configuration; --connect rejects config options.");
}
static char *exe_path(const char *argv0) {
    char path[PATH_MAX];ssize_t n=readlink("/proc/self/exe",path,sizeof path-1);
    if(n>0){path[n]=0;return xstrdup(path);}char *r=realpath(argv0,NULL);return r?r:xstrdup(argv0);
}
static char *python_path(const char *exe) {
    const char *override=getenv("CPYMACS_PYTHON_DIR");if(override && *override)return xstrdup(override);
    char *dir=xstrdup(exe),*slash=strrchr(dir,'/');if(slash)*slash=0;size_t n=strlen(dir)+100;char *candidate=xmalloc(n);
    snprintf(candidate,n,"%s/../share/cpymacs/python",dir);free(dir);
    char *test=xmalloc(n+30);snprintf(test,n+30,"%s/cpymacs.py",candidate);
    if(access(test,R_OK)==0){free(test);return candidate;}free(candidate);
    snprintf(test,n+30,"%s/cpymacs.py",CPYMACS_PYTHON_DIR);
    if(access(test,R_OK)==0){free(test);return xstrdup(CPYMACS_PYTHON_DIR);}free(test);return xstrdup(CPYMACS_INSTALL_DATA);
}
/* json-c strict-mode Unicode handling differs between supported releases.
 * Normalize raw non-ASCII scalars to JSON escapes before strict parsing. This
 * leaves the common ASCII command path allocation-free and retains strict JSON
 * syntax checks instead of enabling the permissive parser. */
static char *strict_json_ascii(const char *line,size_t n,size_t *length) {
    bool nonascii=false;for(size_t i=0;i<n;i++)if((unsigned char)line[i]>=128){nonascii=true;break;}
    if(!nonascii){*length=n;return NULL;}
    static const char hex[]="0123456789abcdef";
    char *out=xmalloc(n*3+1);size_t j=0;
    for(size_t i=0;i<n;) {
        if((unsigned char)line[i]<128){out[j++]=line[i++];continue;}
        size_t k;uint32_t c=utf8_decode(line+i,n-i,&k);i+=k;
        unsigned units[2];int count=1;
        if(c>0xffff){c-=0x10000;units[0]=0xd800+(c>>10);units[1]=0xdc00+(c&1023);count=2;}
        else units[0]=c;
        for(int u=0;u<count;u++){out[j++]='\\';out[j++]='u';for(int shift=12;shift>=0;shift-=4)out[j++]=hex[(units[u]>>shift)&15];}
    }
    out[j]=0;*length=j;return out;
}
static json_object *parse_request(Editor *e,const char *line,size_t n) {
    if(!utf8_valid(line,n)){json_object *bad=json_object_new_object();json_object_object_add(bad,"ok",json_object_new_boolean(false));json_set_string(bad,"error","Invalid UTF-8 request");return bad;}
    json_tokener *tok=json_tokener_new();json_tokener_set_flags(tok,JSON_TOKENER_STRICT);
    size_t parse_len;char *ascii=strict_json_ascii(line,n,&parse_len);
    json_object *request=json_tokener_parse_ex(tok,ascii?ascii:line,(int)parse_len);free(ascii);
    enum json_tokener_error err=json_tokener_get_error(tok);
    json_object *response;
    if(err!=json_tokener_success){response=json_object_new_object();json_object_object_add(response,"ok",json_object_new_boolean(false));json_set_string(response,"error",json_tokener_error_desc(err));}
    else response=editor_request(e,request);
    if(request)json_object_put(request);json_tokener_free(tok);return response;
}
static int backend_stdio(Editor *e) {
    Connection input={.fd=STDIN_FILENO};
    while(!e->quit && !interrupted) {
        char *line=protocol_read_line(&input);if(!line)break;
        json_object *response=parse_request(e,line,strlen(line));free(line);
        const char *text=json_object_to_json_string_ext(response,JSON_C_TO_STRING_PLAIN);
        bool ok=protocol_write(STDOUT_FILENO,text,strlen(text)) && protocol_write(STDOUT_FILENO,"\n",1);json_object_put(response);if(!ok)break;
    }
    free(input.input);return 0;
}
typedef struct {int fd;char *in,*out;size_t in_len,in_cap,out_len,out_sent;} Peer;
static void peer_close(Peer *p){if(p->fd>=0)close(p->fd);free(p->in);free(p->out);memset(p,0,sizeof(*p));p->fd=-1;}
static bool peer_queue(Peer *p,json_object *response) {
    const char *s=json_object_to_json_string_ext(response,JSON_C_TO_STRING_PLAIN);size_t n=strlen(s);
    size_t pending=p->out_len-p->out_sent;if(pending+n+1>CP_MAX_FRAME*4u)return false;
    if(p->out_sent){memmove(p->out,p->out+p->out_sent,pending);p->out_len=pending;p->out_sent=0;}
    p->out=xrealloc(p->out,p->out_len+n+1);memcpy(p->out+p->out_len,s,n);p->out[p->out_len+n]='\n';p->out_len+=n+1;return true;
}
static int backend_server(Editor *e,const char *path) {
    struct sockaddr_un addr;memset(&addr,0,sizeof addr);addr.sun_family=AF_UNIX;
    if(strlen(path)>=sizeof addr.sun_path){fputs("cpymacs: socket path too long\n",stderr);return 1;}strcpy(addr.sun_path,path);
    struct stat st;if(lstat(path,&st)==0){fprintf(stderr,"cpymacs: socket path already exists; refusing to replace it: %s\n",path);return 1;}
    int listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(listener<0){perror("socket");return 1;}
    mode_t old=umask(0177);int bound=bind(listener,(struct sockaddr*)&addr,sizeof addr);umask(old);
    if(bound<0){perror("bind");close(listener);return 1;}
    if(listen(listener,16)<0){perror("listen");close(listener);unlink(path);return 1;}
    Peer peers[32];for(int i=0;i<32;i++){memset(&peers[i],0,sizeof peers[i]);peers[i].fd=-1;}
    fprintf(stderr,"cpymacs: listening on %s\n",path);
    while(!interrupted) {
        struct pollfd fds[33];fds[0]=(struct pollfd){listener,POLLIN,0};bool pending=false;
        for(int i=0;i<32;i++){short events=e->quit?0:POLLIN;if(peers[i].out_len>peers[i].out_sent){events|=POLLOUT;pending=true;}fds[i+1]=(struct pollfd){peers[i].fd,events,0};}
        if(e->quit && !pending)break;
        int n=poll(fds,33,250);if(n<0){if(errno==EINTR)continue;break;}
        if(fds[0].revents&POLLIN){
            int fd=accept4(listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);if(fd>=0){
                struct ucred cred;socklen_t len=sizeof cred;bool valid=getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&len)==0 && cred.uid==geteuid();int slot=-1;
                for(int i=0;i<32;i++)if(peers[i].fd<0){slot=i;break;}
                if(valid && slot>=0)peers[slot].fd=fd;else close(fd);
            }
        }
        for(int i=0;i<32;i++) {
            Peer *p=&peers[i];short events=fds[i+1].revents;if(p->fd<0)continue;
            if(events&POLLIN) {
                char buffer[8192];ssize_t got=read(p->fd,buffer,sizeof buffer);
                if(got<=0){if(got==0 || (errno!=EINTR && errno!=EAGAIN))peer_close(p);continue;}
                if(p->in_len+(size_t)got>CP_MAX_FRAME){peer_close(p);continue;}
                if(p->in_len+(size_t)got+1>p->in_cap){p->in_cap=(p->in_len+(size_t)got+1)*2;p->in=xrealloc(p->in,p->in_cap);}
                memcpy(p->in+p->in_len,buffer,(size_t)got);p->in_len+=(size_t)got;p->in[p->in_len]=0;
                char *lf;while(p->fd>=0 && (lf=memchr(p->in,'\n',p->in_len))!=NULL){size_t len=(size_t)(lf-p->in);json_object *response=parse_request(e,p->in,len);bool ok=peer_queue(p,response);json_object_put(response);if(!ok){peer_close(p);break;}memmove(p->in,p->in+len+1,p->in_len-len-1);p->in_len-=len+1;}
            }
            if(p->fd>=0 && (events&POLLOUT)) {
                ssize_t sent=write(p->fd,p->out+p->out_sent,p->out_len-p->out_sent);
                if(sent>0){p->out_sent+=(size_t)sent;if(p->out_sent==p->out_len){free(p->out);p->out=NULL;p->out_len=p->out_sent=0;}}
                else if(sent<0 && errno!=EINTR && errno!=EAGAIN)peer_close(p);
            }
            if(p->fd>=0 && (events&(POLLERR|POLLNVAL|POLLHUP)))peer_close(p);
        }
    }
    for(int i=0;i<32;i++)peer_close(&peers[i]);close(listener);unlink(path);return 0;
}
int main(int argc,char **argv) {
    if(!setlocale(LC_ALL,""))setlocale(LC_ALL,"C.UTF-8");
    bool nox=false,backend=false,no_python=false,no_user=false;const char *server=NULL,*connect=NULL;
    const char *color_policy=getenv("CPYMACS_TUI_COLORS");
    const char *cursor_policy=getenv("CPYMACS_TUI_CURSOR");
    bool tui_option=false;
    char *configs[32];int config_count=0;char **files=xmalloc((size_t)argc*sizeof(char*));int file_count=0;
    char **forward=xmalloc((size_t)argc*sizeof(char*));int forward_count=0;bool positional=false;
    for(int i=1;i<argc;i++) {
        const char *s=argv[i];
        if(!positional && !strcmp(s,"--")){positional=true;forward[forward_count++]=argv[i];continue;}
        if(!positional && (!strcmp(s,"--nox")||!strcmp(s,"-nox")||!strcmp(s,"-nw")||!strcmp(s,"nox")))nox=true;
        else if(!positional && (!strcmp(s,"--tui-colors") || !strcmp(s,"--tui-cursor") ||
                                !strncmp(s,"--tui-colors=",13) || !strncmp(s,"--tui-cursor=",13))) {
            bool colors=!strncmp(s,"--tui-colors",12); const char *value=strchr(s,'=');
            if(value)++value;
            else {if(i+1==argc){fprintf(stderr,"cpymacs: %s requires a value\n",s);return 2;}value=argv[++i];}
            if(colors)color_policy=value;else cursor_policy=value;
            tui_option=true;
        }
        else if(!positional && (!strcmp(s,"--backend")||!strcmp(s,"--batch")))backend=true;
        else if(!positional && (!strcmp(s,"--server")||!strcmp(s,"--connect")||!strcmp(s,"--config"))) {
            if(i+1==argc){fprintf(stderr,"cpymacs: %s requires a value\n",s);return 2;}
            if(!strcmp(s,"--server"))server=argv[++i];else if(!strcmp(s,"--connect"))connect=argv[++i];
            else{if(config_count==32){fputs("Too many configuration files\n",stderr);return 2;}forward[forward_count++]=argv[i];configs[config_count++]=argv[++i];forward[forward_count++]=argv[i];}
        }else if(!positional && (!strcmp(s,"--no-user-config")||!strcmp(s,"-q"))){no_user=true;forward[forward_count++]=argv[i];}
        else if(!positional && !strcmp(s,"--no-python")){no_python=true;forward[forward_count++]=argv[i];}
        else if(!positional && !strcmp(s,"--version")){puts("cpymacs 0.1.1");return 0;}
        else if(!positional && !strcmp(s,"--help")){usage();return 0;}
        else if(!positional && s[0]=='-'){fprintf(stderr,"cpymacs: unknown option: %s\n",s);return 2;}
        else{files[file_count++]=argv[i];forward[forward_count++]=argv[i];}
    }
    if(tui_option && (backend || server)) {fputs("cpymacs: terminal options are frontend-local\n",stderr);return 2;}
    if(tui_option)nox=true;
    if(connect && (backend||server||config_count||no_python||no_user)){fputs("cpymacs: --connect cannot change backend configuration\n",stderr);return 2;}
    signal(SIGPIPE,SIG_IGN);char *exe=exe_path(argv[0]);
    if(backend||server) {
        Editor e;editor_init(&e);for(int i=0;i<config_count;i++)e.config_paths[e.config_count++]=configs[i];
        char *python=python_path(exe);if(!no_python && !plugins_init(&e,python,!no_user))fprintf(stderr,"cpymacs: %s\n",e.message);free(python);
        for(int i=0;i<file_count;i++)if(!editor_open(&e,files[i]))fprintf(stderr,"cpymacs: %s\n",e.message);
        signal(SIGTERM,stop_handler);signal(SIGINT,stop_handler);
        int status=server?backend_server(&e,server):backend_stdio(&e);editor_destroy(&e);free(files);free(forward);free(exe);return status;
    }
    if(!nox && getenv("DISPLAY")) {
        char *gui=xstrdup(exe);char *slash=strrchr(gui,'/');if(slash)*slash=0;size_t n=strlen(gui)+20;char *path=xmalloc(n);snprintf(path,n,"%s/cpymacs-gui",gui);free(gui);
        if(access(path,X_OK)==0){char **args=xmalloc(((size_t)argc+6)*sizeof(char*));int k=0;args[k++]=path;args[k++]="--backend-exe";args[k++]=exe;if(connect){args[k++]="--connect";args[k++]=(char*)connect;}for(int i=0;i<forward_count;i++)args[k++]=forward[i];args[k]=NULL;execv(path,args);perror("cpymacs-gui");free(args);}else fputs("cpymacs: GUI not built; using terminal frontend\n",stderr);free(path);
    }
    TermColors checked_mode;
    if(!term_colors_parse(color_policy,&checked_mode)) {fputs("cpymacs: --tui-colors must be auto, truecolor, 256, or mono\n",stderr);return 2;}
    if(cursor_policy && strcmp(cursor_policy,"block") && strcmp(cursor_policy,"terminal")) {fputs("cpymacs: --tui-cursor must be block or terminal\n",stderr);return 2;}
    Connection connection;if(!connection_open(&connection,connect,exe,forward,forward_count)){perror("cpymacs connection");return 1;}
    if(connect)for(int i=0;i<file_count;i++){json_object *r=request_new("open");json_set_string(r,"path",files[i]);json_object *resp=connection_request(&connection,r);json_object_put(r);if(resp)json_object_put(resp);}
    int result=tui_run(&connection,color_policy,cursor_policy);connection_close(&connection);free(files);free(forward);free(exe);return result;
}
