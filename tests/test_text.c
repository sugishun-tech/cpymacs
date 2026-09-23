#include "text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL: %s at line %d\n",#x,__LINE__);exit(1);} } while(0)
static uint32_t seed=0x41cb64a3;
static uint32_t rnd(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(void) {
    Text t;text_init(&t,"",0);char *reference=malloc(1);reference[0]=0;size_t n=0;
    const char *samples[]={"a","\n"," ","123","\xce\xbb","\xe6\x97\xa5","\xf0\x9f\x98\x80","\t","\xe3\x81\x82\nxyz"};
    size_t checks=0;
    for(int step=0;step<60000;step++) {
        size_t at=n?rnd()%(n+1):0;while(at && at<n && ((unsigned char)reference[at]&0xc0)==0x80)at--;
        if(n<15000 && (rnd()%3 || !n)) {
            const char *s=samples[rnd()%(sizeof samples/sizeof samples[0])];size_t len=strlen(s);
            CHECK(text_insert(&t,at,s,len));reference=realloc(reference,n+len+1);memmove(reference+at+len,reference+at,n-at+1);memcpy(reference+at,s,len);n+=len;
        }else{
            size_t z=at+(n>at?rnd()%(n-at+1):0);while(z>at && z<n && ((unsigned char)reference[z]&0xc0)==0x80)z--;
            CHECK(text_delete(&t,at,z-at));memmove(reference+at,reference+z,n-z+1);n-=z-at;
        }
        CHECK(text_size(&t)==n);CHECK(text_check(&t));char *actual=text_slice(&t,0,n);CHECK(!memcmp(actual,reference,n+1));free(actual);
        size_t chars=0,lines=1;for(size_t i=0;i<n;i++){chars+=((unsigned char)reference[i]&0xc0)!=0x80;lines+=reference[i]=='\n';}
        CHECK(text_chars(&t)==chars);CHECK(text_lines(&t)==lines);
        size_t index=chars?rnd()%(chars+1):0,byte=0,ch=0,row=0;
        while(byte<n && ch<index){byte++;while(byte<n && ((unsigned char)reference[byte]&0xc0)==0x80)byte++;ch++;}
        CHECK(text_char_to_byte(&t,index)==byte);CHECK(text_byte_to_char(&t,byte)==index);CHECK(text_boundary(&t,byte));
        for(size_t i=0;i<byte;i++)row+=reference[i]=='\n';CHECK(text_line_of(&t,byte)==row);
        size_t start=byte;while(start && reference[start-1]!='\n')start--;CHECK(text_line_start(&t,row)==start);checks+=10;
    }
    CHECK(!text_insert(&t,0,"\xff",1));CHECK(!text_insert(&t,0,"\0",1));
    CHECK(!utf8_valid("\xc0\x80",2));CHECK(!utf8_valid("\xed\xa0\x80",3));CHECK(!utf8_valid("\xf4\x90\x80\x80",4));
    text_free(&t);free(reference);
    /* Contiguous typing should coalesce, yet remain valid after arbitrary splits. */
    text_init(&t,"",0);for(int i=0;i<50000;i++)CHECK(text_insert(&t,text_size(&t),"x",1));CHECK(text_size(&t)==50000);CHECK(text_check(&t));text_free(&t);
    printf("PASS: 60000 randomized UTF-8 edits, %zu property checks, 50000 contiguous inserts\n",checks);return 0;
}
