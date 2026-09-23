#include "text.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* An implicit randomized treap over immutable pieces. All stored offsets are
 * independent of realloc; no node holds a pointer into the append-only store. */
struct Piece {
    Piece *left, *right;
    size_t offset, length, newlines, chars, bytes_sum, lines_sum, chars_sum;
    uint32_t priority;
    unsigned source;
};
void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("cpymacs: out of memory\n", stderr); abort(); }
    return p;
}
void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("cpymacs: out of memory\n", stderr); abort(); }
    return q;
}
char *xstrdup(const char *s) { size_t n=strlen(s)+1; char *r=xmalloc(n); memcpy(r,s,n); return r; }
static size_t nb(const Piece *p) { return p ? p->bytes_sum : 0; }
static size_t nl(const Piece *p) { return p ? p->lines_sum : 0; }
static size_t nc(const Piece *p) { return p ? p->chars_sum : 0; }
static const char *data(const Text *t, const Piece *p) { return (p->source ? t->added : t->original) + p->offset; }
static void update(Piece *p) {
    if (p) {
        p->bytes_sum=nb(p->left)+p->length+nb(p->right);
        p->lines_sum=nl(p->left)+p->newlines+nl(p->right);
        p->chars_sum=nc(p->left)+p->chars+nc(p->right);
    }
}
static uint32_t random_next(Text *t) {
    uint32_t x=t->random; x^=x<<13; x^=x>>17; x^=x<<5; return t->random=x;
}
static Piece *node(Text *t, unsigned source, size_t off, size_t n) {
    Piece *p=xmalloc(sizeof(*p)); memset(p,0,sizeof(*p));
    p->source=source; p->offset=off; p->length=n; p->priority=random_next(t);
    const char *s=data(t,p);
    for(size_t i=0;i<n;i++) { p->newlines+=s[i]=='\n'; p->chars+=((unsigned char)s[i]&0xc0)!=0x80; }
    update(p); return p;
}
static Piece *merge(Piece *a, Piece *b) {
    if(!a) return b;
    if(!b) return a;
    if(a->priority>b->priority) { a->right=merge(a->right,b); update(a); return a; }
    b->left=merge(a,b->left); update(b); return b;
}
static void split(Text *t, Piece *p, size_t at, Piece **a, Piece **b) {
    if(!p) { *a=*b=NULL; return; }
    size_t left=nb(p->left);
    if(at<left) { split(t,p->left,at,a,&p->left); update(p); *b=p; }
    else if(at>left+p->length) { split(t,p->right,at-left-p->length,&p->right,b); update(p); *a=p; }
    else if(at==left) { *a=p->left; p->left=NULL; update(p); *b=p; }
    else if(at==left+p->length) { *b=p->right; p->right=NULL; update(p); *a=p; }
    else {
        size_t k=at-left;
        Piece *x=node(t,p->source,p->offset,k), *y=node(t,p->source,p->offset+k,p->length-k);
        x->priority=p->priority; y->priority=p->priority;
        *a=merge(p->left,x); *b=merge(y,p->right); free(p);
    }
}
static void destroy(Piece *p) { if(p) { destroy(p->left); destroy(p->right); free(p); } }
static Piece *pieces(Text *t,unsigned src,size_t off,size_t n) {
    Piece *r=NULL;
    while(n) {
        size_t m=n>4096 ? 4096 : n;
        const char *s=(src ? t->added : t->original)+off;
        if(m<n) while(m && ((unsigned char)s[m]&0xc0)==0x80) m--;
        r=merge(r,node(t,src,off,m)); off+=m; n-=m;
    }
    return r;
}
void text_init(Text *t,const char *s,size_t n) {
    memset(t,0,sizeof(*t)); t->random=0x9e3779b9u; t->original=xmalloc(n+1);
    if(n) memcpy(t->original,s,n);
    t->original[n]=0; t->original_len=n; t->root=pieces(t,0,0,n);
}
void text_free(Text *t) { destroy(t->root); free(t->original); free(t->added); memset(t,0,sizeof(*t)); }
size_t text_size(const Text *t) { return nb(t->root); }
size_t text_lines(const Text *t) { return nl(t->root)+1; }
size_t text_chars(const Text *t) { return nc(t->root); }
bool text_boundary(const Text *t,size_t at) { return at<=text_size(t) && (at==text_size(t) || (text_byte(t,at)&0xc0)!=0x80); }
static bool append_right(Text *t,Piece *p,size_t offset,const char *s,size_t n) {
    if(!p)return false;
    if(p->right) {bool ok=append_right(t,p->right,offset,s,n);if(ok)update(p);return ok;}
    if(!p->source || p->offset+p->length!=offset || p->length+n>4096)return false;
    p->length+=n;for(size_t i=0;i<n;i++){p->newlines+=s[i]=='\n';p->chars+=((unsigned char)s[i]&0xc0)!=0x80;}update(p);return true;
}
bool text_insert(Text *t,size_t at,const char *s,size_t n) {
    if(!text_boundary(t,at) || !utf8_valid(s,n) || n>SIZE_MAX-t->added_len-1) return false;
    if(!n) return true;
    size_t end=t->added_len+n;
    if(end+1>t->added_cap) {
        size_t cap=t->added_cap ? t->added_cap : 4096;
        while(cap<end+1) { if(cap>SIZE_MAX/2) { cap=end+1; break; } cap*=2; }
        t->added=xrealloc(t->added,cap); t->added_cap=cap;
    }
    memcpy(t->added+t->added_len,s,n); t->added[end]=0;
    Piece *a,*b;size_t offset=t->added_len;t->added_len=end;
    split(t,t->root,at,&a,&b);
    if(append_right(t,a,offset,s,n))t->root=merge(a,b);
    else t->root=merge(merge(a,pieces(t,1,offset,n)),b);
    return true;
}
bool text_delete(Text *t,size_t at,size_t n) {
    if(at>text_size(t) || n>text_size(t)-at || !text_boundary(t,at) || !text_boundary(t,at+n)) return false;
    Piece *a,*b,*c; split(t,t->root,at,&a,&b); split(t,b,n,&b,&c); destroy(b); t->root=merge(a,c); return true;
}
static void copy_range(const Text *t,const Piece *p,size_t at,size_t n,char **out) {
    if(!p || !n) return;
    size_t l=nb(p->left);
    if(at<l) { size_t m=l-at<n ? l-at : n; copy_range(t,p->left,at,m,out); n-=m; at+=m; }
    if(n && at<l+p->length) {
        size_t k=at>l ? at-l : 0, m=p->length-k<n ? p->length-k : n;
        memcpy(*out,data(t,p)+k,m); *out+=m; n-=m; at+=m;
    }
    if(n) copy_range(t,p->right,at-l-p->length,n,out);
}
void text_copy(const Text *t,size_t at,size_t n,char *out) { if(at<=text_size(t)) { if(n>text_size(t)-at) n=text_size(t)-at; copy_range(t,t->root,at,n,&out); } }
char *text_slice(const Text *t,size_t at,size_t n) {
    if(at>text_size(t)) at=text_size(t);
    if(n>text_size(t)-at) n=text_size(t)-at;
    char *out=xmalloc(n+1); text_copy(t,at,n,out); out[n]=0; return out;
}
unsigned char text_byte(const Text *t,size_t at) {
    const Piece *p=t->root;
    while(p) { size_t l=nb(p->left); if(at<l) p=p->left; else if(at<l+p->length) return (unsigned char)data(t,p)[at-l]; else { at-=l+p->length; p=p->right; } }
    return 0;
}
size_t text_next(const Text *t,size_t at) { size_t n=text_size(t); if(at>=n) return n; do { at++; } while(at<n && (text_byte(t,at)&0xc0)==0x80); return at; }
size_t text_prev(const Text *t,size_t at) { if(at>text_size(t)) at=text_size(t); if(!at) return 0; do { at--; } while(at && (text_byte(t,at)&0xc0)==0x80); return at; }
size_t text_line_start(const Text *t,size_t line) {
    if(!line) return 0;
    if(line>=text_lines(t)) return text_size(t);
    const Piece *p=t->root; size_t off=0;
    while(p) {
        size_t l=nl(p->left);
        if(line<=l) p=p->left;
        else {
            line-=l; off+=nb(p->left);
            if(line<=p->newlines) { const char *s=data(t,p); for(size_t i=0;i<p->length;i++) if(s[i]=='\n' && --line==0) return off+i+1; }
            line-=p->newlines; off+=p->length; p=p->right;
        }
    }
    return text_size(t);
}
static size_t prefix_metric(const Text *t,size_t at,bool chars) {
    const Piece *p=t->root; size_t result=0;
    while(p) {
        size_t l=nb(p->left);
        if(at<l) p=p->left;
        else {
            result+=chars ? nc(p->left) : nl(p->left); at-=l;
            if(at<=p->length) { const char *s=data(t,p); for(size_t i=0;i<at;i++) result+=chars ? (((unsigned char)s[i]&0xc0)!=0x80) : (s[i]=='\n'); return result; }
            result+=chars ? p->chars : p->newlines; at-=p->length; p=p->right;
        }
    }
    return result;
}
size_t text_line_of(const Text *t,size_t at) { return prefix_metric(t,at,false); }
size_t text_byte_to_char(const Text *t,size_t at) { return prefix_metric(t,at,true); }
size_t text_char_to_byte(const Text *t,size_t ch) {
    const Piece *p=t->root; size_t off=0;
    while(p) {
        size_t l=nc(p->left);
        if(ch<l) p=p->left;
        else {
            ch-=l; off+=nb(p->left);
            if(ch<=p->chars) {
                const char *s=data(t,p);
                for(size_t i=0;i<p->length;i++) if(((unsigned char)s[i]&0xc0)!=0x80) { if(!ch) return off+i; ch--; }
                return off+p->length;
            }
            ch-=p->chars; off+=p->length; p=p->right;
        }
    }
    return off;
}
static bool write_piece(const Text *t,const Piece *p,FILE *f,bool crlf) {
    if(!p) return true;
    if(!write_piece(t,p->left,f,crlf)) return false;
    const char *s=data(t,p);
    if(!crlf) { if(fwrite(s,1,p->length,f)!=p->length) return false; }
    else for(size_t i=0;i<p->length;i++) { if(s[i]=='\n' && fputc('\r',f)==EOF) return false; if(fputc((unsigned char)s[i],f)==EOF) return false; }
    return write_piece(t,p->right,f,crlf);
}
bool text_write(const Text *t,FILE *f,bool crlf) { return write_piece(t,t->root,f,crlf); }
uint32_t utf8_decode(const char *s,size_t n,size_t *used) {
    *used=1; if(!n) return 0;
    const unsigned char *p=(const unsigned char *)s; uint32_t c=p[0]; size_t k;
    if(c<0x80) return c;
    if(c>=0xc2 && c<=0xdf) { k=2; c&=31; }
    else if(c>=0xe0 && c<=0xef) { k=3; c&=15; }
    else if(c>=0xf0 && c<=0xf4) { k=4; c&=7; }
    else return 0xfffd;
    if(n<k) return 0xfffd;
    for(size_t i=1;i<k;i++) { if((p[i]&0xc0)!=0x80) return 0xfffd; c=(c<<6)|(p[i]&63); }
    if((k==3 && c<0x800)||(k==4 && c<0x10000)||c>0x10ffff||(c>=0xd800 && c<=0xdfff)) return 0xfffd;
    *used=k; return c;
}
bool utf8_valid(const char *s,size_t n) {
    for(size_t i=0;i<n;) { size_t k; uint32_t c=utf8_decode(s+i,n-i,&k); if((unsigned char)s[i]>=128 && k==1) return false; if(c==0) return false; i+=k; }
    return true;
}
static int check_node(const Text *t,const Piece *p) {
    if(!p) return 1;
    size_t nn=0,cc=0; const char *s=data(t,p);
    for(size_t i=0;i<p->length;i++) { nn+=s[i]=='\n'; cc+=((unsigned char)s[i]&0xc0)!=0x80; }
    return (!p->left || p->left->priority<=p->priority) && (!p->right || p->right->priority<=p->priority) && nn==p->newlines && cc==p->chars && p->bytes_sum==nb(p->left)+p->length+nb(p->right) && p->lines_sum==nl(p->left)+nn+nl(p->right) && p->chars_sum==nc(p->left)+cc+nc(p->right) && check_node(t,p->left) && check_node(t,p->right);
}
int text_check(const Text *t) { return check_node(t,t->root); }
