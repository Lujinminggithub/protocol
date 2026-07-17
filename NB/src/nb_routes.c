#include "nb_routes.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char* out,size_t cap,const char* fmt,...){if(out&&cap){va_list ap;va_start(ap,fmt);vsnprintf(out,cap,fmt,ap);va_end(ap);}return -1;}

int nb_routes_load(nb_routes_t* routes,const char* path,char* error,size_t error_cap){
    if(!routes||!path)return fail(error,error_cap,"missing route config");
    FILE* f=fopen(path,"r");if(!f)return fail(error,error_cap,"open %s: %s",path,strerror(errno));
    nb_routes_t next;memset(&next,0,sizeof(next));char line[512];unsigned line_no=0;
    while(fgets(line,sizeof(line),f)){
        line_no++;char* p=line;while(*p==' '||*p=='\t')p++;if(*p==0||*p=='#'||*p=='\r'||*p=='\n')continue;
        char keyword[16],name[32],hop[256],extra[16];unsigned weight=0;
        int fields=sscanf(p,"%15s %31s %255s %u %15s",keyword,name,hop,&weight,extra);
        if(fields!=4||strcmp(keyword,"route")||strncmp(hop,"H:",2)||weight==0||weight>1000){
            fclose(f);return fail(error,error_cap,"invalid route at line %u",line_no);
        }
        const char* colon=strrchr(hop+2,':');char* end=NULL;long port=colon?strtol(colon+1,&end,10):0;
        if(!colon||colon==hop+2||!end||*end||port<=0||port>65535||next.count>=NB_ROUTE_MAX||next.total_weight>UINT32_MAX-weight){
            fclose(f);return fail(error,error_cap,"invalid route endpoint at line %u",line_no);
        }
        for(size_t i=0;i<next.count;i++)if(!strcmp(next.entries[i].name,name)){fclose(f);return fail(error,error_cap,"duplicate route name at line %u",line_no);}
        nb_route_entry_t* entry=&next.entries[next.count++];strcpy(entry->name,name);strcpy(entry->hop,hop);entry->weight=(uint16_t)weight;
        next.total_weight+=weight;
    }
    fclose(f);if(next.count==0)return fail(error,error_cap,"route config is empty");*routes=next;return 0;
}

const nb_route_entry_t* nb_routes_pick(nb_routes_t* routes){
    if(!routes||routes->count==0||routes->total_weight==0)return NULL;
    uint32_t point=routes->cursor++%routes->total_weight,base=0;
    for(size_t i=0;i<routes->count;i++){base+=routes->entries[i].weight;if(point<base)return &routes->entries[i];}
    return &routes->entries[routes->count-1];
}
