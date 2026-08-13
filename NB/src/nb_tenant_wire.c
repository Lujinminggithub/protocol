#include "nb_tenant_wire.h"

#include <stdio.h>
#include <string.h>

int nb_tenant_wire_name_valid(const char* name){
    if(name==NULL||name[0]==0)return 0;
    for(const unsigned char* p=(const unsigned char*)name;*p;p++)
        if(!( (*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||
              *p=='_'||*p=='.'||*p=='-' ))return 0;
    return 1;
}

int nb_tenant_wire_render(char* out,size_t cap,const char* tenant,const char* route){
    if(out==NULL||cap==0||route==NULL||route[0]==0||strchr(route,'\n')||strchr(route,'\r'))return -1;
    int length;
    if(tenant&&tenant[0]){
        if(!nb_tenant_wire_name_valid(tenant))return -1;
        length=snprintf(out,cap,NB_TENANT_WIRE_PREFIX "%s;%s",tenant,route);
    }else length=snprintf(out,cap,"%s",route);
    return length<=0||(size_t)length>=cap?-1:length;
}

int nb_tenant_wire_parse(const char* input,char* tenant,size_t tenant_cap,const char** route){
    if(input==NULL||tenant==NULL||tenant_cap==0||route==NULL||input[0]==0)return -1;
    tenant[0]=0;*route=input;
    if(strncmp(input,NB_TENANT_WIRE_PREFIX,strlen(NB_TENANT_WIRE_PREFIX))!=0)return 0;
    const char* value=input+strlen(NB_TENANT_WIRE_PREFIX);const char* end=strchr(value,';');
    if(end==NULL||end==value||end[1]==0)return -1;
    size_t length=(size_t)(end-value);if(length>=tenant_cap)return -1;
    memcpy(tenant,value,length);tenant[length]=0;
    if(!nb_tenant_wire_name_valid(tenant))return -1;
    *route=end+1;return 1;
}
