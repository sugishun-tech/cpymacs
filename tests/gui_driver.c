#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Deterministic X11 input/selection driver for real frontend tests. */
static void pause_ms(long ms){struct timespec t={ms/1000,(ms%1000)*1000000};nanosleep(&t,NULL);}
static Window find_editor(Display *d,Window w) {
    XClassHint hint={0};
    if(XGetClassHint(d,w,&hint)){
        int match=hint.res_class&&!strcmp(hint.res_class,"Cpymacs");
        if(hint.res_name)XFree(hint.res_name);if(hint.res_class)XFree(hint.res_class);
        if(match)return w;
    }
    Window root,parent,*children=NULL;unsigned count=0;
    if(!XQueryTree(d,w,&root,&parent,&children,&count))return None;
    Window result=None;for(unsigned i=0;i<count&&!result;i++)result=find_editor(d,children[i]);
    if(children)XFree(children);return result;
}
static Window wait_editor(Display *d) {
    for(int i=0;i<250;i++){Window w=find_editor(d,DefaultRootWindow(d));if(w)return w;pause_ms(20);}return None;
}
static int send_key(Display *d,Window w,const char *token) {
    unsigned state=0;const char *name=token;
    if(!strncmp(name,"C-",2)){state|=ControlMask;name+=2;}
    if(!strncmp(name,"M-",2)){state|=Mod1Mask;name+=2;}
    KeySym symbol=NoSymbol;
    if(strlen(name)==1)symbol=(unsigned char)*name;
    else if(!strcmp(name,"RET"))symbol=XK_Return;
    else if(!strcmp(name,"TAB"))symbol=XK_Tab;
    else if(!strcmp(name,"SPC"))symbol=XK_space;
    else if(!strcmp(name,"DEL"))symbol=XK_BackSpace;
    else if(!strcmp(name,"<left>"))symbol=XK_Left;
    else if(!strcmp(name,"<right>"))symbol=XK_Right;
    else if(!strcmp(name,"<up>"))symbol=XK_Up;
    else if(!strcmp(name,"<down>"))symbol=XK_Down;
    else symbol=XStringToKeysym(name);
    if(symbol==NoSymbol)return 0;
    KeyCode code=XKeysymToKeycode(d,symbol);if(!code)return 0;
    if(XkbKeycodeToKeysym(d,code,0,1)==symbol && XkbKeycodeToKeysym(d,code,0,0)!=symbol)state|=ShiftMask;
    XKeyEvent key={0};key.display=d;key.window=w;key.root=DefaultRootWindow(d);key.same_screen=True;key.keycode=code;key.state=state;key.time=CurrentTime;key.type=KeyPress;
    XSendEvent(d,w,False,KeyPressMask,(XEvent*)&key);key.type=KeyRelease;XSendEvent(d,w,False,KeyReleaseMask,(XEvent*)&key);XFlush(d);pause_ms(15);return 1;
}
static int next_event(Display *d,XEvent *event,int timeout) {
    if(XPending(d)){XNextEvent(d,event);return 1;}
    struct pollfd p={ConnectionNumber(d),POLLIN,0};if(poll(&p,1,timeout)<=0)return 0;XNextEvent(d,event);return 1;
}
static int read_clipboard(Display *d) {
    Window w=XCreateSimpleWindow(d,DefaultRootWindow(d),0,0,1,1,0,0,0);
    Atom clipboard=XInternAtom(d,"CLIPBOARD",False),utf8=XInternAtom(d,"UTF8_STRING",False),property=XInternAtom(d,"TEST_SELECTION",False),incr=XInternAtom(d,"INCR",False);
    XSelectInput(d,w,PropertyChangeMask);XConvertSelection(d,clipboard,utf8,property,w,CurrentTime);XFlush(d);int receiving=0;
    for(int i=0;i<1000;i++){
        XEvent ev;if(!next_event(d,&ev,5000))return 2;
        if(ev.type==SelectionNotify || (receiving&&ev.type==PropertyNotify&&ev.xproperty.state==PropertyNewValue)){
            if(ev.type==SelectionNotify&&ev.xselection.property==None)return 3;
            Atom type;int format;unsigned long n,after;unsigned char *bytes=NULL;
            XGetWindowProperty(d,w,property,0,4*1024*1024,True,AnyPropertyType,&type,&format,&n,&after,&bytes);
            if(type==incr){receiving=1;XDeleteProperty(d,w,property);XFlush(d);}
            else if(format==8){if(n)fwrite(bytes,1,n,stdout);if(!receiving||!n){if(bytes)XFree(bytes);XDestroyWindow(d,w);return 0;}}
            if(bytes)XFree(bytes);XFlush(d);
        }
    }
    return 4;
}
static int serve_clipboard(Display *d,const char *text) {
    Window w=XCreateSimpleWindow(d,DefaultRootWindow(d),0,0,1,1,0,0,0);
    Atom clipboard=XInternAtom(d,"CLIPBOARD",False),utf8=XInternAtom(d,"UTF8_STRING",False);
    XSetSelectionOwner(d,clipboard,w,CurrentTime);XFlush(d);puts("ready");fflush(stdout);
    for(int i=0;i<100;i++){
        XEvent ev;if(!next_event(d,&ev,5000))return 2;
        if(ev.type==SelectionRequest){XSelectionRequestEvent *req=&ev.xselectionrequest;Atom p=req->property?req->property:req->target;
            XChangeProperty(d,req->requestor,p,utf8,8,PropModeReplace,(const unsigned char*)text,(int)strlen(text));
            XSelectionEvent out={0};out.type=SelectionNotify;out.display=d;out.requestor=req->requestor;out.selection=req->selection;out.target=req->target;out.property=p;out.time=req->time;
            XSendEvent(d,req->requestor,False,0,(XEvent*)&out);XFlush(d);pause_ms(200);XDestroyWindow(d,w);return 0;
        }
    }return 3;
}
static unsigned channel(unsigned long pixel,unsigned long mask) {
    if(!mask)return 0;unsigned shift=0;while(!(mask&1)){mask>>=1;shift++;}
    return (unsigned)(((pixel>>shift)&mask)*255/mask);
}
static int screenshot(Display *d,Window w,const char *path) {
    XWindowAttributes attr;if(!XGetWindowAttributes(d,w,&attr))return 2;
    XImage *image=XGetImage(d,w,0,0,(unsigned)attr.width,(unsigned)attr.height,AllPlanes,ZPixmap);
    if(!image)return 3;FILE *f=fopen(path,"wb");if(!f){XDestroyImage(image);return 4;}
    fprintf(f,"P6\n%d %d\n255\n",attr.width,attr.height);
    for(int y=0;y<attr.height;y++)for(int x=0;x<attr.width;x++){
        unsigned long pixel=XGetPixel(image,x,y);unsigned char rgb[3]={
            (unsigned char)channel(pixel,image->red_mask),
            (unsigned char)channel(pixel,image->green_mask),
            (unsigned char)channel(pixel,image->blue_mask)};
        fwrite(rgb,1,3,f);
    }
    XDestroyImage(image);return fclose(f)?5:0;
}
int main(int argc,char **argv) {
    if(argc<2)return 2;Display *d=XOpenDisplay(NULL);if(!d)return 3;int result=0;
    if(!strcmp(argv[1],"read-clipboard"))result=read_clipboard(d);
    else if(!strcmp(argv[1],"serve-clipboard")&&argc==3)result=serve_clipboard(d,argv[2]);
    else {
        Window w=wait_editor(d);if(!w){fputs("No cpymacs window found\n",stderr);XCloseDisplay(d);return 4;}
        if(!strcmp(argv[1],"screenshot")&&argc==3)result=screenshot(d,w,argv[2]);
        else if(!strcmp(argv[1],"wait"))printf("%lu\n",w);
        else if(!strcmp(argv[1],"keys")){for(int i=2;i<argc;i++)if(!send_key(d,w,argv[i]))result=5;}
        else if(!strcmp(argv[1],"type")&&argc==3){for(size_t i=0;i<strlen(argv[2]);i++){char token[2]={argv[2][i],0};if(!send_key(d,w,token))result=5;}}
        else if(!strcmp(argv[1],"resize")&&argc==4){XResizeWindow(d,w,(unsigned)atoi(argv[2]),(unsigned)atoi(argv[3]));XFlush(d);}
        else if(!strcmp(argv[1],"click")&&argc==4){XButtonEvent ev={0};ev.display=d;ev.window=w;ev.root=DefaultRootWindow(d);ev.same_screen=True;ev.button=Button1;ev.x=atoi(argv[2]);ev.y=atoi(argv[3]);ev.type=ButtonPress;XSendEvent(d,w,False,ButtonPressMask,(XEvent*)&ev);ev.type=ButtonRelease;XSendEvent(d,w,False,ButtonReleaseMask,(XEvent*)&ev);XFlush(d);}
        else result=2;
    }
    XCloseDisplay(d);return result;
}
