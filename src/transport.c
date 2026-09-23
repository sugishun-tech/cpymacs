#define _POSIX_C_SOURCE 200809L
#include "transport.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

json_object *json_field(json_object *o,const char *key){json_object *v=NULL;if(o)json_object_object_get_ex(o,key,&v);return v;}
const char *json_string(json_object *o,const char *key,const char *fallback){json_object *v=json_field(o,key);return v && json_object_is_type(v,json_type_string)?json_object_get_string(v):fallback;}
int64_t json_int(json_object *o,const char *key,int64_t fallback){json_object *v=json_field(o,key);return v && json_object_is_type(v,json_type_int)?json_object_get_int64(v):fallback;}
bool jbool(json_object *o,const char *key,bool fallback){json_object *v=json_field(o,key);return v && json_object_is_type(v,json_type_boolean)?json_object_get_boolean(v)!=0:fallback;}
void json_set_string(json_object *o,const char *key,const char *value){json_object_object_add(o,key,json_object_new_string(value?value:""));}
void json_set_int(json_object *o,const char *key,int64_t value){json_object_object_add(o,key,json_object_new_int64(value));}
json_object *request_new(const char *op){json_object *r=json_object_new_object();json_set_string(r,"op",op);return r;}
bool protocol_write(int fd,const char *s,size_t n){while(n){ssize_t k=write(fd,s,n);if(k<0 && errno==EINTR)continue;if(k<=0)return false;s+=k;n-=(size_t)k;}return true;}
char *protocol_read_line(Connection *c) {
    for(;;) {
        if(c->length){char *lf=memchr(c->input,'\n',c->length);if(lf){size_t n=(size_t)(lf-c->input);char *line=malloc(n+1);if(!line)return NULL;memcpy(line,c->input,n);line[n]=0;memmove(c->input,lf+1,c->length-n-1);c->length-=n+1;return line;}}
        if(c->length>=CP_MAX_FRAME){errno=EMSGSIZE;return NULL;}
        if(c->capacity-c->length<4096){size_t cap=c->capacity?c->capacity*2:8192;if(cap>CP_MAX_FRAME+1)cap=CP_MAX_FRAME+1;char *buf=realloc(c->input,cap);if(!buf)return NULL;c->input=buf;c->capacity=cap;}
        ssize_t n=read(c->fd,c->input+c->length,c->capacity-c->length);if(n<0 && errno==EINTR)continue;if(n<=0)return NULL;c->length+=(size_t)n;
    }
}
bool connection_open(Connection *c,const char *path,const char *executable,char **args,int argc) {
    memset(c,0,sizeof(*c));c->fd=-1;signal(SIGPIPE,SIG_IGN);
    if(path) {
        struct sockaddr_un address;memset(&address,0,sizeof address);address.sun_family=AF_UNIX;
        if(strlen(path)>=sizeof address.sun_path){errno=ENAMETOOLONG;return false;}strcpy(address.sun_path,path);
        c->fd=socket(AF_UNIX,SOCK_STREAM,0);if(c->fd<0)return false;
        if(connect(c->fd,(struct sockaddr *)&address,sizeof address)!=0){close(c->fd);c->fd=-1;return false;}fcntl(c->fd,F_SETFD,FD_CLOEXEC);return true;
    }
    int fds[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,fds)!=0)return false;
    pid_t child=fork();if(child<0){close(fds[0]);close(fds[1]);return false;}
    if(child==0) {
        close(fds[0]);if(dup2(fds[1],STDIN_FILENO)<0 || dup2(fds[1],STDOUT_FILENO)<0)_exit(126);if(fds[1]>2)close(fds[1]);
        char **argv=calloc((size_t)argc+3,sizeof(char*));if(!argv)_exit(126);argv[0]=(char*)executable;argv[1]="--backend";for(int i=0;i<argc;i++)argv[i+2]=args[i];execv(executable,argv);perror("cpymacs backend");_exit(127);
    }
    close(fds[1]);c->fd=fds[0];c->child=child;fcntl(c->fd,F_SETFD,FD_CLOEXEC);return true;
}
json_object *connection_request(Connection *c,json_object *request) {
    const char *s=json_object_to_json_string_ext(request,JSON_C_TO_STRING_PLAIN);size_t n=strlen(s);
    if(n>CP_MAX_FRAME || !protocol_write(c->fd,s,n) || !protocol_write(c->fd,"\n",1))return NULL;
    char *line=protocol_read_line(c);if(!line)return NULL;json_object *response=json_tokener_parse(line);free(line);return response;
}
void connection_close(Connection *c) {
    if(c->fd>=0)close(c->fd);free(c->input);c->input=NULL;c->fd=-1;
    if(c->child>0){int status;while(waitpid(c->child,&status,0)<0 && errno==EINTR){}}c->child=0;
}
