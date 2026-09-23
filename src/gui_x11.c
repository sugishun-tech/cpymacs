#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "transport.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <wchar.h>
#include <stdint.h>
#include <errno.h>

/* This executable links only Xlib and the protocol client. No editor core and no
 * Python interpreter are present in the GUI process. */
typedef struct {
    Display *display;int screen;Window window;GC gc;Pixmap back;
    XFontSet fonts;XFontStruct *fallback;XIM im;XIC ic;
    int width,height,cell_width,line_height,ascent,font_size;
    unsigned long pixels[10],rgb[10];
    Connection connection;json_object *state;
    Atom wm_delete,utf8,clipboard,targets,selection_property,incr;
    char *clipboard_text,*primary_text,*incoming;size_t incoming_len;
    bool receiving,done,dragging;
    Atom pending_selection;
    Window send_window;Atom send_property;char *sending;size_t send_length,send_offset;
    uint64_t kill_generation;
    char font_name[128];
} Gui;
static int text_columns(const char *s,size_t n) {
    int columns=0;mbstate_t state={0};
    for(size_t i=0;i<n;){wchar_t wc;size_t k=mbrtowc(&wc,s+i,n-i,&state);if(k==(size_t)-1||k==(size_t)-2||!k){memset(&state,0,sizeof state);k=1;wc=L'?';}int w=wcwidth(wc);columns+=w<0?1:w;i+=k;}return columns;
}
static char *safe_label(const char *s) {
    size_t n=strlen(s);char *out=malloc(n+1);if(!out)return NULL;for(size_t i=0;i<n;i++)out[i]=(unsigned char)s[i]<32 || s[i]==127?' ':s[i];out[n]=0;return out;
}
static void put_text(Gui *g,int x,int y,const char *s,size_t n,int color) {
    XSetForeground(g->display,g->gc,g->pixels[color]);
    if(g->fonts)Xutf8DrawString(g->display,g->back,g->fonts,g->gc,x,y+g->ascent,s,(int)n);
    else {
        char *ascii=malloc(n+1);if(!ascii)return;size_t j=0;for(size_t i=0;i<n;i++)if((unsigned char)s[i]<128)ascii[j++]=s[i];else if(((unsigned char)s[i]&0xc0)!=0x80)ascii[j++]='?';
        XDrawString(g->display,g->back,g->gc,x,y+g->ascent,ascii,(int)j);free(ascii);
    }
}
static void rectangle(Gui *g,int x,int y,int width,int height,int color) {
    if(width<=0||height<=0)return;XSetForeground(g->display,g->gc,g->pixels[color]);XFillRectangle(g->display,g->back,g->gc,x,y,(unsigned)width,(unsigned)height);
}
static void load_font(Gui *g,const char *name,int size) {
    if(g->fonts){XFreeFontSet(g->display,g->fonts);g->fonts=NULL;}if(g->fallback){XFreeFont(g->display,g->fallback);g->fallback=NULL;}
    char pattern[512];int pixels=size<14?13:size<17?15:size<20?18:20;
    if(!strcmp(name,"fixed"))snprintf(pattern,sizeof pattern,"-misc-fixed-medium-r-normal--%d-*-*-*-*-*-*-*,fixed",pixels);else snprintf(pattern,sizeof pattern,"%.250s,fixed",name);
    char **missing=NULL,*default_string=NULL;int missing_count=0;
    g->fonts=XCreateFontSet(g->display,pattern,&missing,&missing_count,&default_string);if(missing)XFreeStringList(missing);
    if(g->fonts){XFontSetExtents *ext=XExtentsOfFontSet(g->fonts);g->cell_width=Xutf8TextEscapement(g->fonts,"M",1);g->line_height=ext->max_logical_extent.height+4;g->ascent=-ext->max_logical_extent.y+2;}
    else{g->fallback=XLoadQueryFont(g->display,"fixed");if(!g->fallback){fputs("cpymacs-gui: no usable X11 font\n",stderr);g->done=true;return;}XSetFont(g->display,g->gc,g->fallback->fid);g->cell_width=g->fallback->max_bounds.width;g->line_height=g->fallback->ascent+g->fallback->descent+4;g->ascent=g->fallback->ascent+2;}
    if(g->cell_width<1)g->cell_width=8;if(g->line_height<1)g->line_height=18;g->font_size=size;snprintf(g->font_name,sizeof g->font_name,"%s",name);
}
static void allocate_colors(Gui *g,json_object *state) {
    json_object *colors=json_field(state,"colors");
    for(int i=0;i<10;i++){unsigned long rgb=(unsigned long)json_object_get_int64(json_object_array_get_idx(colors,(size_t)i));if(g->rgb[i]==rgb && g->pixels[i])continue;g->rgb[i]=rgb;XColor c={0};c.red=(unsigned short)(((rgb>>16)&255)*257);c.green=(unsigned short)(((rgb>>8)&255)*257);c.blue=(unsigned short)((rgb&255)*257);c.flags=DoRed|DoGreen|DoBlue;if(XAllocColor(g->display,DefaultColormap(g->display,g->screen),&c))g->pixels[i]=c.pixel;else g->pixels[i]=i==1?BlackPixel(g->display,g->screen):WhitePixel(g->display,g->screen);}
}
static void draw(Gui *g) {
    if(!g->state || !g->back)return;allocate_colors(g,g->state);rectangle(g,0,0,g->width,g->height,1);
    json_object *panes=json_field(g->state,"panes");int cursor_x=0,cursor_y=0;int rows=(int)json_int(g->state,"rows",24),cols=(int)json_int(g->state,"cols",80);
    for(size_t i=0;i<json_object_array_length(panes);i++) {
        json_object *pane=json_object_array_get_idx(panes,i);int px=(int)json_int(pane,"x",0)*g->cell_width,py=(int)json_int(pane,"y",0)*g->line_height;
        int width=(int)json_int(pane,"width",cols)*g->cell_width,height=(int)json_int(pane,"height",rows-1),gutter=(int)json_int(pane,"gutter",0);
        XRectangle clip={(short)px,(short)py,(unsigned short)width,(unsigned short)(height*g->line_height)};XSetClipRectangles(g->display,g->gc,0,0,&clip,1,Unsorted);
        json_object *lines=json_field(pane,"lines");
        for(size_t r=0;r<json_object_array_length(lines);r++) {
            json_object *line=json_object_array_get_idx(lines,r);int y=py+(int)r*g->line_height,x=px+gutter*g->cell_width;
            if(gutter && json_int(line,"line",-1)>=0){char number[32];snprintf(number,sizeof number,"%6lld",(long long)json_int(line,"line",0)+1);put_text(g,px,y,number,strlen(number),2);}
            const char *text=json_string(line,"text","");size_t n=strlen(text);json_object *spans=json_field(line,"spans");
            if(spans && json_object_array_length(spans))for(size_t j=0;j<json_object_array_length(spans);j++) {
                json_object *s=json_object_array_get_idx(spans,j);size_t a=(size_t)json_object_get_int64(json_object_array_get_idx(s,0)),z=(size_t)json_object_get_int64(json_object_array_get_idx(s,1));int style=json_object_get_int(json_object_array_get_idx(s,2));if(z>n||a>z||style<0||style>9)continue;
                int pixels=text_columns(text+a,z-a)*g->cell_width;if(style==8)rectangle(g,x,y,pixels,g->line_height,8);put_text(g,x,y,text+a,z-a,style==8?0:style);x+=pixels;
            }else put_text(g,x,y,text,n,0);
        }
        int my=py+(height-1)*g->line_height;rectangle(g,px,my,width,g->line_height,9);char *label=safe_label(json_string(pane,"modeline",""));if(label){put_text(g,px,my,label,strlen(label),0);free(label);}
        if(jbool(pane,"active",false)){cursor_x=px+(int)json_int(pane,"cursor_col",0)*g->cell_width;cursor_y=py+(int)json_int(pane,"cursor_row",0)*g->line_height;}
    }
    XSetClipMask(g->display,g->gc,None);const char *prompt=json_string(g->state,"prompt","");int echo_y=(rows-1)*g->line_height;
    if(*prompt){char *s=safe_label(prompt),*input=safe_label(json_string(g->state,"input",""));if(s&&input){put_text(g,0,echo_y,s,strlen(s),0);int offset=text_columns(s,strlen(s))*g->cell_width;put_text(g,offset,echo_y,input,strlen(input),0);size_t at=(size_t)json_int(g->state,"input_point",0);if(at>strlen(input))at=strlen(input);cursor_x=offset+text_columns(input,at)*g->cell_width;cursor_y=echo_y;}free(s);free(input);}
    else{char *s=safe_label(json_string(g->state,"message",""));if(s){put_text(g,0,echo_y,s,strlen(s),0);free(s);}}
    if(cursor_x<0)cursor_x=0;if(cursor_y<0)cursor_y=0;XSetForeground(g->display,g->gc,g->pixels[6]);XDrawRectangle(g->display,g->back,g->gc,cursor_x,cursor_y,(unsigned)(g->cell_width-1),(unsigned)(g->line_height-1));
    XCopyArea(g->display,g->back,g->window,g->gc,0,0,(unsigned)g->width,(unsigned)g->height,0,0);XFlush(g->display);
}
static json_object *send_request(Gui *g,json_object *request) {
    json_set_int(request,"rows",g->height/g->line_height);json_set_int(request,"cols",g->width/g->cell_width);
    json_object *response=connection_request(&g->connection,request);json_object_put(request);
    if(!response){g->done=true;fputs("cpymacs-gui: backend disconnected\n",stderr);return NULL;}
    json_object *state=json_field(response,"state");
    if(state){if(g->state)json_object_put(g->state);g->state=json_object_get(state);if(jbool(state,"quit",false)||jbool(state,"detach",false))g->done=true;
        const char *font=json_string(state,"font","fixed");int size=(int)json_int(state,"font_size",16);if(size!=g->font_size||strcmp(font,g->font_name))load_font(g,font,size);
        json_object *panes=json_field(state,"panes");for(size_t i=0;i<json_object_array_length(panes);i++){json_object *p=json_object_array_get_idx(panes,i);if(jbool(p,"active",false)){char title[512];snprintf(title,sizeof title,"%s%.400s - cpymacs",jbool(p,"modified",false)?"* ":"",json_string(p,"name",""));XStoreName(g->display,g->window,title);break;}}
    }
    return response;
}
static void refresh(Gui *g){json_object *r=send_request(g,request_new("snapshot"));if(r)json_object_put(r);draw(g);}
static void update_kill_clipboard(Gui *g) {
    uint64_t generation=(uint64_t)json_int(g->state,"kill_generation",0);if(generation==g->kill_generation)return;g->kill_generation=generation;
    json_object *req=request_new("kill_ring");json_object_object_add(req,"no_snapshot",json_object_new_boolean(true));json_object *r=send_request(g,req);
    if(r && jbool(r,"ok",false)){json_object *value=json_field(r,"result");if(value && json_object_is_type(value,json_type_string)){free(g->clipboard_text);g->clipboard_text=strdup(json_object_get_string(value));XSetSelectionOwner(g->display,g->clipboard,g->window,CurrentTime);}}
    if(r)json_object_put(r);
}
static void key_request(Gui *g,const char *key) {
    json_object *req=request_new("key");json_set_string(req,"key",key);json_object *r=send_request(g,req);if(r)json_object_put(r);if(!g->done)update_kill_clipboard(g);draw(g);
}
static void paste(Gui *g,Atom selection) {
    Window owner=XGetSelectionOwner(g->display,selection);
    if(owner==None){key_request(g,"C-y");return;}
    if(owner==g->window && selection==g->clipboard){key_request(g,"C-y");return;}
    g->pending_selection=selection;XConvertSelection(g->display,selection,g->utf8,g->selection_property,g->window,CurrentTime);
}
static void insert_clipboard(Gui *g,const char *s,size_t n) {
    if(n>8u*1024u*1024u)return;json_object *req=request_new("yank_external");json_object_object_add(req,"text",json_object_new_string_len(s,(int)n));json_object *r=send_request(g,req);if(r)json_object_put(r);draw(g);
}
static void selection_request(Gui *g,XSelectionRequestEvent *request) {
    XSelectionEvent response={0};response.type=SelectionNotify;response.display=request->display;response.requestor=request->requestor;response.selection=request->selection;response.target=request->target;response.time=request->time;response.property=None;
    Atom property=request->property==None?request->target:request->property;const char *text=request->selection==XA_PRIMARY?g->primary_text:g->clipboard_text;if(!text)text="";
    if(request->target==g->targets){Atom targets[]={g->targets,g->utf8,XA_STRING};XChangeProperty(g->display,request->requestor,property,XA_ATOM,32,PropModeReplace,(unsigned char*)targets,3);response.property=property;}
    else if(request->target==g->utf8 || request->target==XA_STRING) {
        size_t n=strlen(text);if(n<=65536){XChangeProperty(g->display,request->requestor,property,request->target,8,PropModeReplace,(const unsigned char*)text,(int)n);response.property=property;}
        else if(!g->sending){g->sending=strdup(text);g->send_length=n;g->send_offset=0;g->send_window=request->requestor;g->send_property=property;unsigned long size=(unsigned long)n;XSelectInput(g->display,request->requestor,PropertyChangeMask);XChangeProperty(g->display,request->requestor,property,g->incr,32,PropModeReplace,(unsigned char*)&size,1);response.property=property;}
    }
    XSendEvent(g->display,request->requestor,False,0,(XEvent*)&response);XFlush(g->display);
}
static void property_event(Gui *g,XPropertyEvent *event) {
    if(g->sending && event->window==g->send_window && event->atom==g->send_property && event->state==PropertyDelete) {
        size_t n=g->send_length-g->send_offset;if(n>65536)n=65536;XChangeProperty(g->display,g->send_window,g->send_property,g->utf8,8,PropModeReplace,(unsigned char*)g->sending+g->send_offset,(int)n);g->send_offset+=n;
        if(!n){free(g->sending);g->sending=NULL;}return;
    }
    if(g->receiving && event->window==g->window && event->atom==g->selection_property && event->state==PropertyNewValue) {
        Atom type;int format;unsigned long count,after;unsigned char *data=NULL;
        if(XGetWindowProperty(g->display,g->window,g->selection_property,0,CP_MAX_FRAME/4,True,AnyPropertyType,&type,&format,&count,&after,&data)!=Success)return;
        if(format!=8 || after || g->incoming_len+count>8u*1024u*1024u){g->receiving=false;free(g->incoming);g->incoming=NULL;g->incoming_len=0;}
        else if(!count){g->receiving=false;if(g->incoming)insert_clipboard(g,g->incoming,g->incoming_len);free(g->incoming);g->incoming=NULL;g->incoming_len=0;}
        else{char *p=realloc(g->incoming,g->incoming_len+count+1);if(p){g->incoming=p;memcpy(p+g->incoming_len,data,count);g->incoming_len+=count;g->incoming[g->incoming_len]=0;}}
        if(data)XFree(data);
    }
}
static void selection_notify(Gui *g,XSelectionEvent *event) {
    if(event->property==None)return;Atom type;int format;unsigned long count,after;unsigned char *data=NULL;
    if(XGetWindowProperty(g->display,g->window,event->property,0,CP_MAX_FRAME/4,True,AnyPropertyType,&type,&format,&count,&after,&data)!=Success)return;
    if(type==g->incr){g->receiving=true;free(g->incoming);g->incoming=NULL;g->incoming_len=0;XDeleteProperty(g->display,g->window,event->property);}
    else if(format==8 && !after && data)insert_clipboard(g,(const char*)data,count);
    if(data)XFree(data);
}
static void key_event(Gui *g,XKeyEvent *event) {
    bool control=(event->state&ControlMask)!=0,meta=(event->state&Mod1Mask)!=0;KeySym symbol=NoSymbol;char text[256]={0};int n=0;Status status=0;
    if(g->ic && !control && !meta){n=Xutf8LookupString(g->ic,event,text,sizeof text-1,&symbol,&status);if(status==XBufferOverflow){char *large=malloc((size_t)n+1);if(!large)return;n=Xutf8LookupString(g->ic,event,large,n,&symbol,&status);large[n]=0;json_object *req=request_new("insert");json_set_string(req,"text",large);json_object *r=send_request(g,req);if(r)json_object_put(r);free(large);draw(g);return;}}
    else n=XLookupString(event,text,sizeof text-1,&symbol,NULL);
    if(n<0)n=0;text[n]=0;const char *special=NULL;char key[100]={0},base[80]={0};
    switch(symbol){case XK_Return:case XK_KP_Enter:special="RET";break;case XK_Tab:case XK_ISO_Left_Tab:special="TAB";break;case XK_BackSpace:special="DEL";break;case XK_Delete:special="<delete>";break;
        case XK_Left:special="<left>";break;case XK_Right:special="<right>";break;case XK_Up:special="<up>";break;case XK_Down:special="<down>";break;case XK_Home:special="<home>";break;case XK_End:special="<end>";break;case XK_Page_Up:special="<prior>";break;case XK_Page_Down:special="<next>";break;case XK_Escape:special="ESC";break;default:break;}
    if(symbol>=XK_F1 && symbol<=XK_F12){snprintf(base,sizeof base,"<f%lu>",(unsigned long)(symbol-XK_F1+1));special=base;}
    if(control){
        if(symbol==XK_space || symbol==XK_at)strcpy(key,"C-SPC");
        else if(symbol==XK_slash)strcpy(key,"C-/");else if(symbol==XK_underscore)strcpy(key,"C-_");else if(symbol==XK_question)strcpy(key,"C-?");
        else if(symbol==XK_Left)strcpy(key,"M-b");else if(symbol==XK_Right)strcpy(key,"M-f");
        else{KeySym unshift=XLookupKeysym(event,0);if(unshift>=XK_a && unshift<=XK_z)snprintf(key,sizeof key,"C-%c",(char)unshift);else if(unshift>=XK_A && unshift<=XK_Z)snprintf(key,sizeof key,"C-%c",(char)(unshift-XK_A+'a'));else if(special)snprintf(key,sizeof key,"C-%s",special);}
    }else if(meta){if(special)snprintf(key,sizeof key,"M-%s",special);else if(n)snprintf(key,sizeof key,"M-%.70s",text);}
    else if(special)snprintf(key,sizeof key,"%s",special);
    else if(n){if((size_t)n<sizeof key)memcpy(key,text,(size_t)n+1);else return;}
    if(!*key)return;
    if(!strcmp(key,"C-y") && !*json_string(g->state,"prompt","") && XGetSelectionOwner(g->display,g->clipboard)!=g->window && XGetSelectionOwner(g->display,g->clipboard)!=None){paste(g,g->clipboard);return;}
    /* An input method may commit multiple Unicode code points in one event. */
    mbstate_t commit_state={0};wchar_t commit_char;size_t first_commit=mbrtowc(&commit_char,text,(size_t)n,&commit_state);
    if(!control&&!meta&&!special && first_commit<(size_t)n){json_object *req=request_new("insert");json_set_string(req,"text",text);json_object *r=send_request(g,req);if(r)json_object_put(r);draw(g);}
    else key_request(g,key);
}
static void mouse_point(Gui *g,int x,int y,bool extend) {
    if(!g->state)return;json_object *panes=json_field(g->state,"panes");int cx=x/g->cell_width,cy=y/g->line_height;
    for(size_t i=0;i<json_object_array_length(panes);i++){json_object *p=json_object_array_get_idx(panes,i);int px=(int)json_int(p,"x",0),py=(int)json_int(p,"y",0),w=(int)json_int(p,"width",0),h=(int)json_int(p,"height",0),gutter=(int)json_int(p,"gutter",0);
        if(cx<px || cx>=px+w || cy<py || cy>=py+h-1)continue;json_object *req=request_new("click");json_set_int(req,"pane",json_int(p,"index",0));json_set_int(req,"line",json_int(p,"top",0)+cy-py);int column=cx-px-gutter;if(column<0)column=0;json_set_int(req,"column",json_int(p,"left",0)+column);json_object_object_add(req,"extend",json_object_new_boolean(extend));json_object *r=send_request(g,req);if(r)json_object_put(r);draw(g);break;}
}
static void select_primary(Gui *g) {
    json_object *req=request_new("selection");json_object_object_add(req,"no_snapshot",json_object_new_boolean(true));json_object *r=send_request(g,req);
    if(r && jbool(r,"ok",false)){json_object *s=json_field(r,"result");if(s && json_object_is_type(s,json_type_string) && *json_object_get_string(s)){free(g->primary_text);g->primary_text=strdup(json_object_get_string(s));XSetSelectionOwner(g->display,XA_PRIMARY,g->window,CurrentTime);}}
    if(r)json_object_put(r);
}
static int ignore_x_error(Display *display,XErrorEvent *event){char error[256];XGetErrorText(display,event->error_code,error,sizeof error);fprintf(stderr,"cpymacs-gui: X11: %s\n",error);return 0;}
int main(int argc,char **argv) {
    setlocale(LC_ALL,"");XSetLocaleModifiers("");const char *backend="cpymacs",*connect=NULL;char **forward=calloc((size_t)argc,sizeof(char*));if(!forward)return 1;int count=0;
    for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--backend-exe") && i+1<argc)backend=argv[++i];else if(!strcmp(argv[i],"--connect") && i+1<argc)connect=argv[++i];else if(!strcmp(argv[i],"--help")){puts("Usage: cpymacs-gui [--backend-exe PATH] [--connect SOCKET] [backend options] [FILE ...]");free(forward);return 0;}else forward[count++]=argv[i];}
    char resolved[4096];if(!strchr(backend,'/')){ssize_t n=readlink("/proc/self/exe",resolved,sizeof resolved-1);if(n>0){resolved[n]=0;char *s=strrchr(resolved,'/');if(s){strcpy(s+1,"cpymacs");backend=resolved;}}}
    Gui g;memset(&g,0,sizeof g);g.connection.fd=-1;g.width=1000;g.height=720;g.display=XOpenDisplay(NULL);
    if(!g.display){fputs("cpymacs-gui: cannot open DISPLAY; run cpymacs --nox\n",stderr);free(forward);return 1;}
    XSetErrorHandler(ignore_x_error);g.screen=DefaultScreen(g.display);g.window=XCreateSimpleWindow(g.display,RootWindow(g.display,g.screen),0,0,(unsigned)g.width,(unsigned)g.height,0,0,BlackPixel(g.display,g.screen));
    XClassHint hint={"cpymacs","Cpymacs"};XSetClassHint(g.display,g.window,&hint);XStoreName(g.display,g.window,"cpymacs");
    XSelectInput(g.display,g.window,ExposureMask|KeyPressMask|KeyReleaseMask|StructureNotifyMask|FocusChangeMask|ButtonPressMask|ButtonReleaseMask|Button1MotionMask|PropertyChangeMask);
    g.gc=XCreateGC(g.display,g.window,0,NULL);g.back=XCreatePixmap(g.display,g.window,(unsigned)g.width,(unsigned)g.height,(unsigned)DefaultDepth(g.display,g.screen));
    g.wm_delete=XInternAtom(g.display,"WM_DELETE_WINDOW",False);XSetWMProtocols(g.display,g.window,&g.wm_delete,1);
    g.utf8=XInternAtom(g.display,"UTF8_STRING",False);g.clipboard=XInternAtom(g.display,"CLIPBOARD",False);g.targets=XInternAtom(g.display,"TARGETS",False);g.selection_property=XInternAtom(g.display,"CPYMACS_SELECTION",False);g.incr=XInternAtom(g.display,"INCR",False);
    load_font(&g,"fixed",16);g.im=XOpenIM(g.display,NULL,NULL,NULL);
    if(g.im){XIMStyles *styles=NULL;XGetIMValues(g.im,XNQueryInputStyle,&styles,NULL);XIMStyle selected=0;if(styles){for(unsigned short i=0;i<styles->count_styles;i++)if(styles->supported_styles[i]==(XIMPreeditNothing|XIMStatusNothing)){selected=styles->supported_styles[i];break;}XFree(styles);}if(selected)g.ic=XCreateIC(g.im,XNInputStyle,selected,XNClientWindow,g.window,XNFocusWindow,g.window,NULL);}
    XMapWindow(g.display,g.window);
    if(!connection_open(&g.connection,connect,backend,forward,count)){perror("cpymacs-gui backend");g.done=true;}
    else {
        if(connect)for(int i=0;i<count;i++){if(forward[i][0]=='-'){fprintf(stderr,"cpymacs-gui: backend options cannot be changed on attach\n");g.done=true;break;}json_object *req=request_new("open");json_set_string(req,"path",forward[i]);json_object *r=send_request(&g,req);if(r)json_object_put(r);}
        if(!g.done)refresh(&g);
    }
    free(forward);
    while(!g.done) {
        while(XPending(g.display) && !g.done) {
            XEvent event;XNextEvent(g.display,&event);if(XFilterEvent(&event,g.window))continue;
            switch(event.type) {
            case Expose:if(!event.xexpose.count)draw(&g);break;
            case ConfigureNotify:if(g.width!=event.xconfigure.width || g.height!=event.xconfigure.height){g.width=event.xconfigure.width;g.height=event.xconfigure.height;XFreePixmap(g.display,g.back);g.back=XCreatePixmap(g.display,g.window,(unsigned)g.width,(unsigned)g.height,(unsigned)DefaultDepth(g.display,g.screen));refresh(&g);}break;
            case KeyPress:key_event(&g,&event.xkey);break;
            case FocusIn:if(g.ic)XSetICFocus(g.ic);break;
            case FocusOut:if(g.ic)XUnsetICFocus(g.ic);break;
            case ClientMessage:if((Atom)event.xclient.data.l[0]==g.wm_delete)key_request(&g,"C-x"),key_request(&g,"C-c");break;
            case ButtonPress:if(event.xbutton.button==Button1){g.dragging=true;mouse_point(&g,event.xbutton.x,event.xbutton.y,(event.xbutton.state&ShiftMask)!=0);}else if(event.xbutton.button==Button2)paste(&g,XA_PRIMARY);else if(event.xbutton.button==Button4 || event.xbutton.button==Button5){json_object *req=request_new("command");json_set_string(req,"name",event.xbutton.button==Button4?"scroll-down-command":"scroll-up-command");json_set_int(req,"count",3);json_object *r=send_request(&g,req);if(r)json_object_put(r);draw(&g);}break;
            case MotionNotify:if(g.dragging)mouse_point(&g,event.xmotion.x,event.xmotion.y,true);break;
            case ButtonRelease:if(event.xbutton.button==Button1){g.dragging=false;select_primary(&g);}break;
            case SelectionRequest:selection_request(&g,&event.xselectionrequest);break;
            case SelectionNotify:selection_notify(&g,&event.xselection);break;
            case PropertyNotify:property_event(&g,&event.xproperty);break;
            default:break;
            }
        }
        if(g.done)break;struct pollfd fd={ConnectionNumber(g.display),POLLIN,0};int ready=poll(&fd,1,200);if(ready==0)refresh(&g);else if(ready<0 && errno!=EINTR)break;
    }
    if(g.state)json_object_put(g.state);if(g.ic)XDestroyIC(g.ic);if(g.im)XCloseIM(g.im);if(g.fonts)XFreeFontSet(g.display,g.fonts);if(g.fallback)XFreeFont(g.display,g.fallback);
    XFreePixmap(g.display,g.back);XFreeGC(g.display,g.gc);XDestroyWindow(g.display,g.window);XCloseDisplay(g.display);connection_close(&g.connection);
    free(g.clipboard_text);free(g.primary_text);free(g.incoming);free(g.sending);return 0;
}
