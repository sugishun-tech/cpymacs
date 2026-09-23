#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "editor.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Editor *active;
static bool in_hook;
static PyObject *error_result(const char *msg) {PyErr_SetString(PyExc_RuntimeError,msg);return NULL;}
static PyObject *py_info(PyObject *self,PyObject *args) {
    (void)self;(void)args;Buffer *b=editor_buffer(active);
    return Py_BuildValue("{s:i,s:s,s:s,s:s,s:K,s:K,s:K,s:K,s:K,s:O,s:O,s:i,s:i,s:O}",
        "id",b->id,"name",b->name,"path",b->path?b->path:"","mode",b->mode,
        "point",(unsigned long long)b->point,"character_point",(unsigned long long)text_byte_to_char(&b->text,b->point),
        "mark",(unsigned long long)b->mark,"size",(unsigned long long)text_size(&b->text),"revision",(unsigned long long)b->revision,
        "mark_active",b->mark_active?Py_True:Py_False,"modified",b->state!=b->saved_state?Py_True:Py_False,
        "tab_width",b->tab_width,"indent_width",b->indent_width,"auto_indent",b->auto_indent?Py_True:Py_False);
}
static PyObject *py_text(PyObject *self,PyObject *args) {
    (void)self;Py_ssize_t start=0,end=-1;if(!PyArg_ParseTuple(args,"|nn",&start,&end))return NULL;
    Buffer *b=editor_buffer(active);size_t size=text_size(&b->text);if(end<0)end=(Py_ssize_t)size;
    if(start<0 || end<start || (size_t)end>size || !text_boundary(&b->text,(size_t)start) || !text_boundary(&b->text,(size_t)end))return error_result("Invalid UTF-8 byte range");
    char *s=text_slice(&b->text,(size_t)start,(size_t)(end-start));PyObject *v=PyUnicode_DecodeUTF8(s,end-start,"strict");free(s);return v;
}
static PyObject *py_replace(PyObject *self,PyObject *args) {
    (void)self;Py_ssize_t at,n,len;const char *s;if(!PyArg_ParseTuple(args,"nns#",&at,&n,&s,&len))return NULL;
    if(at<0 || n<0 || !editor_replace(active,(size_t)at,(size_t)n,s,(size_t)len))return error_result(active->message);
    Py_RETURN_NONE;
}
static PyObject *py_insert(PyObject *self,PyObject *args) {
    (void)self;const char *s;Py_ssize_t n;if(!PyArg_ParseTuple(args,"s#",&s,&n))return NULL;
    if(!editor_insert(active,s,(size_t)n))return error_result(active->message);Py_RETURN_NONE;
}
static PyObject *py_set_text(PyObject *self,PyObject *args) {
    (void)self;const char *s;Py_ssize_t n;if(!PyArg_ParseTuple(args,"s#",&s,&n))return NULL;
    if(!editor_set_text(active,s,(size_t)n))return error_result(active->message);Py_RETURN_NONE;
}
static PyObject *py_point(PyObject *self,PyObject *args) {
    (void)self;Py_ssize_t n;int chars=0;if(!PyArg_ParseTuple(args,"n|p",&n,&chars))return NULL;
    Buffer *b=editor_buffer(active);if(n<0)return error_result("Negative point");size_t at=chars?text_char_to_byte(&b->text,(size_t)n):(size_t)n;
    if(!text_boundary(&b->text,at))return error_result("Point must be a UTF-8 byte boundary");b->point=at;Py_RETURN_NONE;
}
static PyObject *py_command(PyObject *self,PyObject *args) {
    (void)self;const char *name,*arg=NULL;long count=1;if(!PyArg_ParseTuple(args,"s|zl",&name,&arg,&count))return NULL;
    if(!editor_command(active,name,arg,count,count!=1))return error_result(active->message);Py_RETURN_NONE;
}
static PyObject *py_register(PyObject *self,PyObject *args) {
    (void)self;const char *name;PyObject *func;if(!PyArg_ParseTuple(args,"sO",&name,&func))return NULL;
    if(!PyCallable_Check(func)){PyErr_SetString(PyExc_TypeError,"Command must be callable");return NULL;}
    if(strlen(name)>=100)return error_result("Command name too long");
    if(PyDict_SetItemString((PyObject *)active->python_commands,name,func)<0)return NULL;Py_RETURN_NONE;
}
static PyObject *py_bind(PyObject *self,PyObject *args) {
    (void)self;const char *key,*name,*mode=NULL;if(!PyArg_ParseTuple(args,"ss|z",&key,&name,&mode))return NULL;
    if(strlen(key)>=100 || strlen(name)>=100 || (mode && strlen(mode)>=80))return error_result("Binding too long");
    editor_bind(active,key,name,mode);Py_RETURN_NONE;
}
static PyObject *py_on(PyObject *self,PyObject *args) {
    (void)self;const char *event;PyObject *func;if(!PyArg_ParseTuple(args,"sO",&event,&func))return NULL;
    if(!PyCallable_Check(func)){PyErr_SetString(PyExc_TypeError,"Hook must be callable");return NULL;}
    PyObject *hooks=(PyObject *)active->python_hooks,*list=PyDict_GetItemString(hooks,event);
    if(!list){list=PyList_New(0);if(!list)return NULL;PyDict_SetItemString(hooks,event,list);Py_DECREF(list);list=PyDict_GetItemString(hooks,event);}
    if(PyList_Append(list,func)<0)return NULL;Py_RETURN_NONE;
}
static PyObject *py_option(PyObject *self,PyObject *args) {
    (void)self;const char *name;PyObject *value;if(!PyArg_ParseTuple(args,"sO",&name,&value))return NULL;
    Buffer *b=editor_buffer(active);
    if(!strcmp(name,"tab_width") || !strcmp(name,"indent_width") || !strcmp(name,"font_size")) {
        long n=PyLong_AsLong(value);if(PyErr_Occurred())return NULL;if(n<1 || n>64)return error_result("Option must be between 1 and 64");
        if(!strcmp(name,"tab_width")){b->tab_width=(int)n;b->column_cache.valid=b->seek_cache.valid=false;}else if(!strcmp(name,"indent_width"))b->indent_width=(int)n;else active->font_size=(int)n;
    }else if(!strcmp(name,"auto_indent"))b->auto_indent=PyObject_IsTrue(value)>0;
    else if(!strcmp(name,"line_numbers"))active->show_line_numbers=PyObject_IsTrue(value)>0;
    else if(!strcmp(name,"highlight"))active->show_highlight=PyObject_IsTrue(value)>0;
    else if(!strcmp(name,"read_only"))b->readonly=PyObject_IsTrue(value)>0;
    else if(!strcmp(name,"font")){const char *s=PyUnicode_AsUTF8(value);if(!s)return NULL;snprintf(active->font,sizeof active->font,"%s",s);}
    else return error_result("Unknown option; inspect docs/api.html for supported names");
    Py_RETURN_NONE;
}
static PyObject *py_mode(PyObject *self,PyObject *args) {
    (void)self;const char *name,*syntax,*keywords="",*builtins="";int kind;
    if(!PyArg_ParseTuple(args,"ssi|ss",&name,&syntax,&kind,&keywords,&builtins))return NULL;
    if(kind<0 || kind>5 || strlen(name)>=80)return error_result("Invalid mode or syntax kind");Buffer *b=editor_buffer(active);
    snprintf(b->mode,sizeof b->mode,"%s",name);free(b->syntax.name);free(b->syntax.keywords);free(b->syntax.builtins);
    b->syntax.name=xstrdup(syntax);b->syntax.keywords=xstrdup(keywords);b->syntax.builtins=xstrdup(builtins);b->syntax.kind=kind;b->lex_valid=0;Py_RETURN_NONE;
}
static PyObject *py_message(PyObject *self,PyObject *args) {
    (void)self;const char *s;if(!PyArg_ParseTuple(args,"s",&s))return NULL;editor_message(active,"%s",s);Py_RETURN_NONE;
}
static PyObject *py_open(PyObject *self,PyObject *args) {
    (void)self;const char *s;if(!PyArg_ParseTuple(args,"s",&s))return NULL;if(!editor_open(active,s))return error_result(active->message);Py_RETURN_NONE;
}
static PyObject *py_save(PyObject *self,PyObject *args) {
    (void)self;const char *s=NULL;int force=0;if(!PyArg_ParseTuple(args,"|zp",&s,&force))return NULL;if(!editor_save(active,s,force))return error_result(active->message);Py_RETURN_NONE;
}
static PyObject *py_theme(PyObject *self,PyObject *args) {
    (void)self;const char *name;PyObject *colors;if(!PyArg_ParseTuple(args,"sO",&name,&colors))return NULL;
    PyObject *seq=PySequence_Fast(colors,"Theme must contain ten RGB integers");if(!seq)return NULL;
    if(PySequence_Fast_GET_SIZE(seq)!=10){Py_DECREF(seq);return error_result("Theme requires ten colors");}
    unsigned long parsed[10];for(int i=0;i<10;i++){parsed[i]=PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(seq,i));if(PyErr_Occurred() || parsed[i]>0xffffff){Py_DECREF(seq);if(!PyErr_Occurred())PyErr_SetString(PyExc_ValueError,"Invalid RGB color");return NULL;}}
    memcpy(active->colors,parsed,sizeof parsed);snprintf(active->theme,sizeof active->theme,"%s",name);Py_DECREF(seq);Py_RETURN_NONE;
}
static PyObject *py_log(PyObject *self,PyObject *args) {
    (void)self;const char *name,*s;Py_ssize_t n;if(!PyArg_ParseTuple(args,"ss#",&name,&s,&n))return NULL;
    if(!utf8_valid(s,(size_t)n))return error_result("Log must be UTF-8");Buffer *b=NULL;
    for(size_t i=0;i<active->buffer_count;i++)if(!strcmp(active->buffers[i]->name,name))b=active->buffers[i];
    if(!b)b=editor_new_buffer(active,name);if(!b)return error_result(active->message);
    /* Diagnostic buffers are not editing targets and do not invoke change hooks. */
    text_free(&b->text);text_init(&b->text,s,(size_t)n);b->point=0;b->mark=0;b->mark_active=b->has_mark=false;
    for(size_t i=0;i<b->history_len;i++){free(b->history[i].added);free(b->history[i].removed);}b->history_pos=b->history_len=0;
    b->column_cache.valid=b->seek_cache.valid=false;
    b->revision++;b->saved_state=b->state=++active->next_state;b->readonly=true;b->lex_valid=0;Py_RETURN_NONE;
}
static PyMethodDef methods[]={
    {"info",py_info,METH_NOARGS,"Return active buffer metadata."},
    {"get_text",py_text,METH_VARARGS,"Read a UTF-8 byte range as str."},
    {"replace",py_replace,METH_VARARGS,"Replace a UTF-8 byte range."},
    {"insert",py_insert,METH_VARARGS,"Insert at point."},{"set_text",py_set_text,METH_VARARGS,"Replace changed content only."},
    {"set_point",py_point,METH_VARARGS,"Set a zero-based byte or character position."},
    {"command",py_command,METH_VARARGS,"Invoke a native or Python command."},
    {"register",py_register,METH_VARARGS,"Register a Python callable."},{"bind",py_bind,METH_VARARGS,"Bind an Emacs key sequence."},
    {"on",py_on,METH_VARARGS,"Register an event hook."},{"set_option",py_option,METH_VARARGS,"Set a supported option."},
    {"set_mode",py_mode,METH_VARARGS,"Supply a major mode and native lexical rules."},
    {"message",py_message,METH_VARARGS,"Write to the echo area."},{"open_file",py_open,METH_VARARGS,"Visit a file."},
    {"save_file",py_save,METH_VARARGS,"Atomically save a file."},{"set_theme",py_theme,METH_VARARGS,"Set all native rendering colors."},
    {"log",py_log,METH_VARARGS,"Replace a diagnostic buffer without selecting it."},{NULL,NULL,0,NULL}
};
static struct PyModuleDef module={PyModuleDef_HEAD_INIT,"_cpymacs","Native cpymacs extension API",-1,methods,NULL,NULL,NULL,NULL};
static PyObject *PyInit__cpymacs(void){return PyModule_Create(&module);}
static void report_exception(Editor *e,const char *context) {
    PyObject *type=NULL,*value=NULL,*tb=NULL;PyErr_Fetch(&type,&value,&tb);PyErr_NormalizeException(&type,&value,&tb);
    PyObject *s=value?PyObject_Str(value):NULL;const char *message=s?PyUnicode_AsUTF8(s):"Unknown Python error";
    editor_message(e,"%s: %s",context,message?message:"Python error");fprintf(stderr,"cpymacs: %s\n",e->message);
    Py_XDECREF(s);Py_XDECREF(type);Py_XDECREF(value);Py_XDECREF(tb);PyErr_Clear();
}
bool plugins_load(Editor *e,const char *path) {
    if(!e->python_enabled)return false;FILE *f=fopen(path,"rb");if(!f){editor_message(e,"Cannot open configuration: %s",path);return false;}
    PyObject *globals=(PyObject *)e->python_globals;
    PyObject *file=PyUnicode_DecodeFSDefault(path);if(file){PyDict_SetItemString(globals,"__file__",file);Py_DECREF(file);}
    PyObject *result=PyRun_FileEx(f,path,Py_file_input,globals,globals,1);
    if(!result){report_exception(e,"Configuration failed");return false;}Py_DECREF(result);return true;
}
bool plugins_init(Editor *e,const char *python_dir,bool user_config) {
    active=e;if(PyImport_AppendInittab("_cpymacs",PyInit__cpymacs)<0)return false;
    PyConfig config;PyConfig_InitIsolatedConfig(&config);config.install_signal_handlers=0;
    config.site_import=1;config.user_site_directory=1;
    PyStatus status=Py_InitializeFromConfig(&config);PyConfig_Clear(&config);
    if(PyStatus_Exception(status)){editor_message(e,"Python initialization failed: %s",status.err_msg?status.err_msg:"");return false;}
    PyObject *sys_path=PySys_GetObject("path"),*dir=PyUnicode_DecodeFSDefault(python_dir);if(!dir)return false;PyList_Insert(sys_path,0,dir);Py_DECREF(dir);
    e->python_enabled=true;e->python_commands=PyDict_New();e->python_hooks=PyDict_New();e->python_globals=PyDict_New();
    PyObject *globals=(PyObject *)e->python_globals;PyDict_SetItemString(globals,"__builtins__",PyEval_GetBuiltins());
    /* Plugin stdout must never corrupt the backend's protocol stream. */
    PyRun_SimpleString("import sys\nsys.stdout = sys.stderr\n");
    PyObject *result=PyRun_String("from cpymacs import api\neditor = api\nfrom cpymacs_plugins import modes\nmodes.install(api)\n",Py_file_input,globals,globals);
    if(!result){report_exception(e,"Plugin bootstrap failed");return false;}Py_DECREF(result);
    if(user_config) {
        const char *xdg=getenv("XDG_CONFIG_HOME"),*home=getenv("HOME");char path[4096];
        if(xdg && *xdg)snprintf(path,sizeof path,"%s/cpymacs/init.py",xdg);else snprintf(path,sizeof path,"%s/.config/cpymacs/init.py",home?home:".");
        if(access(path,R_OK)==0)plugins_load(e,path);
    }
    for(int i=0;i<e->config_count;i++)plugins_load(e,e->config_paths[i]);
    return true;
}
bool plugins_command(Editor *e,const char *name,const char *argument,long count) {
    if(!e->python_enabled)return false;PyObject *func=PyDict_GetItemString((PyObject *)e->python_commands,name);if(!func)return false;
    PyObject *api=PyDict_GetItemString((PyObject *)e->python_globals,"api");
    PyObject *n=PyLong_FromLong(count),*arg=argument?PyUnicode_FromString(argument):Py_NewRef(Py_None);
    if(n)PyObject_SetAttrString(api,"argument_count",n);if(arg)PyObject_SetAttrString(api,"argument",arg);Py_XDECREF(n);Py_XDECREF(arg);
    PyObject *result=PyObject_CallFunctionObjArgs(func,api,NULL);
    if(!result){report_exception(e,name);return false;}Py_DECREF(result);return true;
}
static bool event(Editor *e,const char *name,bool strict) {
    if(!e->python_enabled || in_hook)return true;
    PyObject *list=PyDict_GetItemString((PyObject *)e->python_hooks,name);if(!list)return true;
    /* A copied list permits hooks to register further hooks safely. */
    PyObject *copy=PySequence_List(list);if(!copy){report_exception(e,name);return !strict;}
    PyObject *api=PyDict_GetItemString((PyObject *)e->python_globals,"api");in_hook=true;bool ok=true;
    for(Py_ssize_t i=0;i<PyList_GET_SIZE(copy);i++){PyObject *r=PyObject_CallFunctionObjArgs(PyList_GET_ITEM(copy,i),api,NULL);if(!r){report_exception(e,name);ok=false;if(strict)break;}else Py_DECREF(r);}
    in_hook=false;Py_DECREF(copy);return ok || !strict;
}
void plugins_event(Editor *e,const char *name){event(e,name,false);}
bool plugins_before_save(Editor *e){return event(e,"before_save",true);}
json_object *plugins_commands(Editor *e) {
    if(!e->python_enabled)return NULL;json_object *a=json_object_new_array();PyObject *k,*v;Py_ssize_t pos=0;
    while(PyDict_Next((PyObject *)e->python_commands,&pos,&k,&v)){const char *s=PyUnicode_AsUTF8(k);if(s)json_object_array_add(a,json_object_new_string(s));}return a;
}
void plugins_shutdown(Editor *e) {
    if(e->python_enabled){Py_XDECREF((PyObject *)e->python_commands);Py_XDECREF((PyObject *)e->python_hooks);Py_XDECREF((PyObject *)e->python_globals);Py_FinalizeEx();e->python_enabled=false;active=NULL;}
}
