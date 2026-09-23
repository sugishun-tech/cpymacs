#ifndef CPYMACS_TRANSPORT_H
#define CPYMACS_TRANSPORT_H
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include <json-c/json.h>
#define CP_MAX_FRAME (16u*1024u*1024u)
typedef struct { int fd; pid_t child; char *input; size_t length,capacity; } Connection;
bool connection_open(Connection *c,const char *socket_path,const char *executable,char **backend_args,int argc);
json_object *connection_request(Connection *c,json_object *request);
void connection_close(Connection *c);
json_object *request_new(const char *op);
void json_set_string(json_object *obj,const char *key,const char *value);
void json_set_int(json_object *obj,const char *key,int64_t value);
const char *json_string(json_object *obj,const char *key,const char *fallback);
int64_t json_int(json_object *obj,const char *key,int64_t fallback);
bool jbool(json_object *obj,const char *key,bool fallback);
json_object *json_field(json_object *obj,const char *key);
char *protocol_read_line(Connection *c);
bool protocol_write(int fd,const char *text,size_t n);
int tui_run(Connection *connection);
#endif
