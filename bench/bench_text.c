#define _POSIX_C_SOURCE 200809L
#include "text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static uint32_t seed=0x31876543;
static uint32_t random32(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static void checked(bool ok){if(!ok){fputs("Benchmark invariant failed\n",stderr);exit(1);}}
int main(int argc,char **argv) {
    long mib=argc>1?strtol(argv[1],NULL,10):64;
    long operations=argc>2?strtol(argv[2],NULL,10):20000;
    if(mib<1||mib>1024||operations<1||operations>1000000){fputs("Usage: bench_text [MiB:1..1024] [iterations:1..1000000]\n",stderr);return 2;}
    size_t size=(size_t)mib*1024*1024;char *input=xmalloc(size);
    memset(input,'a',size);for(size_t i=79;i<size;i+=80)input[i]='\n';
    double begin=seconds();Text t;text_init(&t,input,size);double load=seconds()-begin;free(input);
    begin=seconds();for(long i=0;i<operations;i++){size_t at=random32()%size;checked(text_insert(&t,at,"x",1));checked(text_delete(&t,at,1));}double edits=seconds()-begin;
    checked(text_size(&t)==size);checked(text_check(&t));
    volatile size_t checksum=0;size_t lines=text_lines(&t);
    begin=seconds();for(long i=0;i<operations;i++){size_t row=random32()%lines;size_t at=text_line_start(&t,row);checksum+=at;checked(text_line_of(&t,at)==row);}double seeks=seconds()-begin;
    size_t middle=size/2;long typing=operations*5;
    begin=seconds();for(long i=0;i<typing;i++)checked(text_insert(&t,middle+(size_t)i,"x",1));double typed=seconds()-begin;
    checked(text_size(&t)==size+(size_t)typing);checked(text_check(&t));
    FILE *sink=fopen("/dev/null","wb");if(!sink){perror("/dev/null");return 1;}
    begin=seconds();checked(text_write(&t,sink,false));checked(fflush(sink)==0);double write=seconds()-begin;fclose(sink);
    struct rusage usage;getrusage(RUSAGE_SELF,&usage);
    printf("{\n  \"benchmark\": \"native piece table only; not editor or disk latency\",\n"
           "  \"input_bytes\": %zu,\n  \"edit_pairs\": %ld,\n  \"seek_pairs\": %ld,\n  \"typed_characters\": %ld,\n"
           "  \"load_seconds\": %.9f,\n  \"edit_pair_microseconds\": %.6f,\n  \"seek_pair_microseconds\": %.6f,\n"
           "  \"typing_microseconds_per_character\": %.6f,\n  \"stream_to_dev_null_seconds\": %.9f,\n"
           "  \"peak_rss_kib\": %ld,\n  \"checksum\": %zu\n}\n",
           size,operations,operations,typing,load,edits*1e6/(double)operations,seeks*1e6/(double)operations,
           typed*1e6/(double)typing,write,usage.ru_maxrss,(size_t)checksum);
    text_free(&t);return 0;
}
