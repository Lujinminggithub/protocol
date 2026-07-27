#include "nb_v2_metadata.h"

#include <string.h>

static void put16(uint8_t* p,uint16_t value){p[0]=(uint8_t)(value>>8);p[1]=(uint8_t)value;}
static void put32(uint8_t* p,uint32_t value){p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;}
static void put64(uint8_t* p,uint64_t value){put32(p,(uint32_t)(value>>32));put32(p+4,(uint32_t)value);}
static uint16_t get16(const uint8_t* p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static uint32_t get32(const uint8_t* p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t get64(const uint8_t* p){return ((uint64_t)get32(p)<<32)|get32(p+4);}

static int class_valid(uint8_t value){
    return value>=NB_V2_META_CLASS_RELIABLE&&value<=NB_V2_META_CLASS_CONTROL;
}

static int path_valid(uint8_t value){return value<=NB_V2_META_PATH_REDUNDANT;}

static int text_valid(const char* text,size_t maximum,int optional,int tag){
    if(text==NULL)return optional;
    size_t length=strlen(text);if(length==0)return optional;if(length>maximum)return 0;
    for(size_t i=0;i<length;i++){
        unsigned char c=(unsigned char)text[i];
        if(tag){if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return 0;}
        else if(c<0x21||c>0x7e)return 0;
    }
    return 1;
}

static int fields_valid(uint32_t flags,uint8_t traffic_class,uint8_t priority,
    uint8_t path_preference,uint64_t session_id,uint64_t flow_id,uint32_t deadline_ms){
    if((flags&~NB_V2_META_FLAGS_ALL)!=0||!class_valid(traffic_class)||
        priority>NB_V2_META_PRIORITY_MAX||!path_valid(path_preference)||
        session_id==0||flow_id==0||deadline_ms>NB_V2_META_DEADLINE_MAX_MS)return 0;
    if((flags&NB_V2_META_FLAG_ALLOW_FEC)!=0&&traffic_class!=NB_V2_META_CLASS_REALTIME)return 0;
    if(traffic_class==NB_V2_META_CLASS_REALTIME&&deadline_ms==0)return 0;
    if(path_preference==NB_V2_META_PATH_REDUNDANT&&
        (flags&NB_V2_META_FLAG_ALLOW_MULTIPATH)==0)return 0;
    return 1;
}

int nb_v2_flow_metadata_validate(const nb_v2_flow_metadata_t* metadata){
    if(metadata==NULL||!fields_valid(metadata->flags,metadata->traffic_class,
        metadata->priority,metadata->path_preference,metadata->session_id,
        metadata->flow_id,metadata->deadline_ms))return 0;
    return text_valid(metadata->route,NB_V2_META_ROUTE_MAX,0,0)&&
        text_valid(metadata->target,NB_V2_META_TARGET_MAX,0,0)&&
        text_valid(metadata->business_tag,NB_V2_META_TAG_MAX,1,1);
}

int nb_v2_flow_metadata_encode(uint8_t* output,size_t capacity,
    const nb_v2_flow_metadata_t* metadata){
    if(output==NULL||!nb_v2_flow_metadata_validate(metadata))return -1;
    uint16_t route_length=(uint16_t)strlen(metadata->route);
    uint16_t target_length=(uint16_t)strlen(metadata->target);
    uint16_t tag_length=metadata->business_tag?(uint16_t)strlen(metadata->business_tag):0;
    size_t total=NB_V2_META_HEADER_SIZE+(size_t)route_length+target_length+tag_length;
    if(capacity<total)return -1;
    memset(output,0,NB_V2_META_HEADER_SIZE);put32(output,NB_V2_META_MAGIC);
    output[4]=NB_V2_META_VERSION;output[5]=NB_V2_META_TYPE_FLOW_OPEN;
    put16(output+6,NB_V2_META_HEADER_SIZE);put32(output+8,metadata->flags);
    output[12]=metadata->traffic_class;output[13]=metadata->priority;
    output[14]=metadata->path_preference;put64(output+16,metadata->session_id);
    put64(output+24,metadata->flow_id);put32(output+32,metadata->deadline_ms);
    put32(output+36,metadata->policy_id);put16(output+40,route_length);
    put16(output+42,target_length);put16(output+44,tag_length);
    size_t offset=NB_V2_META_HEADER_SIZE;memcpy(output+offset,metadata->route,route_length);offset+=route_length;
    memcpy(output+offset,metadata->target,target_length);offset+=target_length;
    if(tag_length)memcpy(output+offset,metadata->business_tag,tag_length);
    return (int)total;
}

static int wire_text_valid(const uint8_t* text,size_t length,int tag){
    if(text==NULL||length==0)return 0;
    for(size_t i=0;i<length;i++){
        unsigned char c=text[i];
        if(tag){if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return 0;}
        else if(c<0x21||c>0x7e)return 0;
    }
    return 1;
}

int nb_v2_flow_metadata_decode(const uint8_t* data,size_t length,
    nb_v2_flow_metadata_view_t* output){
    if(data==NULL||output==NULL||length<NB_V2_META_HEADER_SIZE||
        get32(data)!=NB_V2_META_MAGIC||data[4]!=NB_V2_META_VERSION||
        data[5]!=NB_V2_META_TYPE_FLOW_OPEN||get16(data+6)!=NB_V2_META_HEADER_SIZE||
        data[15]!=0||get16(data+46)!=0)return -1;
    uint16_t route_length=get16(data+40),target_length=get16(data+42),tag_length=get16(data+44);
    if(route_length==0||route_length>NB_V2_META_ROUTE_MAX||target_length==0||
        target_length>NB_V2_META_TARGET_MAX||tag_length>NB_V2_META_TAG_MAX||
        length!=NB_V2_META_HEADER_SIZE+(size_t)route_length+target_length+tag_length)return -1;
    memset(output,0,sizeof(*output));output->flags=get32(data+8);
    output->traffic_class=data[12];output->priority=data[13];output->path_preference=data[14];
    output->session_id=get64(data+16);output->flow_id=get64(data+24);
    output->deadline_ms=get32(data+32);output->policy_id=get32(data+36);
    if(!fields_valid(output->flags,output->traffic_class,output->priority,
        output->path_preference,output->session_id,output->flow_id,output->deadline_ms))return -1;
    size_t offset=NB_V2_META_HEADER_SIZE;output->route=data+offset;output->route_length=route_length;offset+=route_length;
    output->target=data+offset;output->target_length=target_length;offset+=target_length;
    output->business_tag=tag_length?data+offset:NULL;output->business_tag_length=tag_length;
    if(!wire_text_valid(output->route,route_length,0)||!wire_text_valid(output->target,target_length,0)||
        (tag_length&&!wire_text_valid(output->business_tag,tag_length,1)))return -1;
    return 0;
}
