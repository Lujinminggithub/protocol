#include "nb_policy.h"

#include <ctype.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NB_MAX_TIKTOK_RULES 64

typedef struct {
    char pattern[64];
    nb_flow_class_t flow_class;
    nb_flow_lane_t lane_hint;
    nb_flow_fec_t fec_hint;
    int prio;
} nb_tiktok_rule_t;

static const char* g_default_rules[][5] = {
    {"api*",        "ctrl",  "latency", "off",  "2"},
    {"im-api*",     "ctrl",  "latency", "off",  "2"},
    {"tnc*",        "ctrl",  "latency", "off",  "2"},
    {"log*",        "ctrl",  "latency", "off",  "2"},
    {"logger*",     "ctrl",  "latency", "off",  "2"},
    {"mcs*",        "ctrl",  "latency", "off",  "2"},
    {"mon*",        "ctrl",  "latency", "off",  "2"},
    {"common-sign*","ctrl",  "latency", "off",  "2"},
    {"rtc-access*", "media", "latency", "auto", "4"},
    {"rtc*",        "media", "latency", "auto", "4"},
    {"live-netacc*","media", "latency", "auto", "4"},
    {"frontier*",   "media", "latency", "auto", "4"},
    {"webcast*",    "media", "latency", "auto", "4"},
    {"pull-f5*",    "media", "latency", "auto", "4"},
    {"ttcdn*",      "media", "latency", "auto", "4"},
    {"teko*",       "media", "latency", "auto", "4"},
    {"sf16*",       "media", "latency", "auto", "4"},
    {"sf19*",       "media", "latency", "auto", "4"},
    {"oec*",        "bulk",  "bulk",    "off",  "20"},
    {"tos*",        "bulk",  "bulk",    "off",  "20"},
    {"aggr*",       "bulk",  "bulk",    "off",  "20"},
};

struct nb_policy {
    nb_tiktok_rule_t rules[NB_MAX_TIKTOK_RULES];
    size_t rule_count;
    int initialized;
    char path[256];
    time_t mtime;
};

static _Thread_local nb_policy_t* legacy_policy;

static nb_flow_class_t parse_class(const char* s){
    if(strcmp(s, "ctrl") == 0) return NB_FLOW_CLASS_CTRL;
    if(strcmp(s, "media") == 0) return NB_FLOW_CLASS_MEDIA;
    if(strcmp(s, "bulk") == 0) return NB_FLOW_CLASS_BULK;
    return NB_FLOW_CLASS_UNKNOWN;
}

static nb_flow_lane_t parse_lane(const char* s){
    if(strcmp(s, "latency") == 0) return NB_FLOW_LANE_LATENCY;
    if(strcmp(s, "bulk") == 0) return NB_FLOW_LANE_BULK;
    return NB_FLOW_LANE_DEFAULT;
}

static nb_flow_fec_t parse_fec(const char* s){
    if(strcmp(s, "auto") == 0) return NB_FLOW_FEC_AUTO;
    if(strcmp(s, "off") == 0) return NB_FLOW_FEC_OFF;
    if(strcmp(s, "force") == 0 || strcmp(s, "force_on") == 0) return NB_FLOW_FEC_FORCE_ON;
    return NB_FLOW_FEC_DEFAULT;
}

static void to_lower_ascii(char* s){
    for(; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static int append_rule(nb_policy_t* policy,const char* pattern, const char* klass, const char* lane, const char* fec, const char* prio){
    if(policy->rule_count >= NB_MAX_TIKTOK_RULES) return -1;
    nb_tiktok_rule_t* r = &policy->rules[policy->rule_count];
    snprintf(r->pattern, sizeof(r->pattern), "%s", pattern);
    to_lower_ascii(r->pattern);
    r->flow_class = parse_class(klass);
    r->lane_hint = parse_lane(lane);
    r->fec_hint = parse_fec(fec);
    r->prio = atoi(prio);
    if(r->flow_class == NB_FLOW_CLASS_UNKNOWN || r->lane_hint == NB_FLOW_LANE_DEFAULT || r->prio <= 0) return -1;
    policy->rule_count++;
    return 0;
}

static void load_default_rules(nb_policy_t* policy){
    policy->rule_count = 0;
    for(size_t i=0; i<sizeof(g_default_rules)/sizeof(g_default_rules[0]); i++){
        (void)append_rule(policy,g_default_rules[i][0], g_default_rules[i][1], g_default_rules[i][2], g_default_rules[i][3], g_default_rules[i][4]);
    }
}

static int match_pattern_ci(const char* host, const char* pattern){
    char h[256];
    char p[64];
    snprintf(h, sizeof(h), "%s", host ? host : "");
    snprintf(p, sizeof(p), "%s", pattern ? pattern : "");
    to_lower_ascii(h);
    to_lower_ascii(p);
    size_t pl = strlen(p);
    if(pl == 0) return 0;
    if(p[pl - 1] == '*'){
        p[pl - 1] = 0;
        return strstr(h, p) != NULL;
    }
    return strstr(h, p) != NULL;
}

static void maybe_load_rule_file(nb_policy_t* policy,const char* rules_path){
    FILE* f = NULL;
    if(rules_path && rules_path[0]){
        f = fopen(rules_path, "r");
    }else{
        f = fopen("/root/nb/tiktok_flow_rules.conf", "r");
        if(f == NULL) f = fopen("tiktok_flow_rules.conf", "r");
    }
    if(f == NULL) return;
    if(rules_path&&rules_path[0]&&strlen(rules_path)<sizeof(policy->path)){
        memcpy(policy->path,rules_path,strlen(rules_path)+1);
        struct stat status;if(stat(rules_path,&status)==0)policy->mtime=status.st_mtime;
    }

    nb_tiktok_rule_t tmp[NB_MAX_TIKTOK_RULES];
    size_t tmp_count = 0;
    char line[512];
    while(fgets(line, sizeof(line), f)){
        char* p = line;
        while(*p == ' ' || *p == '\t') p++;
        if(*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
        char kw[16], pattern[64], klass[32], lane[32], fec[32], prio[16];
        if(sscanf(p, "%15s %63s class=%31s lane=%31s fec=%31s prio=%15s", kw, pattern, klass, lane, fec, prio) == 6){
            if(strcmp(kw, "match") == 0 && tmp_count < NB_MAX_TIKTOK_RULES){
                nb_tiktok_rule_t* r = &tmp[tmp_count];
                snprintf(r->pattern, sizeof(r->pattern), "%s", pattern);
                to_lower_ascii(r->pattern);
                r->flow_class = parse_class(klass);
                r->lane_hint = parse_lane(lane);
                r->fec_hint = parse_fec(fec);
                r->prio = atoi(prio);
                if(r->flow_class != NB_FLOW_CLASS_UNKNOWN && r->lane_hint != NB_FLOW_LANE_DEFAULT && r->prio > 0){
                    tmp_count++;
                }
            }
        }
    }
    fclose(f);
    if(tmp_count > 0){
        memset(policy->rules, 0, sizeof(policy->rules));
        memcpy(policy->rules, tmp, sizeof(nb_tiktok_rule_t) * tmp_count);
        policy->rule_count = tmp_count;
    }
}

nb_policy_t* nb_policy_create(const char* rules_path){
    nb_policy_t* policy=calloc(1,sizeof(*policy));if(policy==NULL)return NULL;
    load_default_rules(policy);maybe_load_rule_file(policy,rules_path);policy->initialized=1;return policy;
}

void nb_policy_destroy(nb_policy_t* policy){free(policy);}

int nb_policy_reload_if_changed(nb_policy_t* policy){
    if(policy==NULL||policy->path[0]==0)return 0;
    struct stat status;
    if(stat(policy->path,&status)!=0)return -1;
    if(status.st_mtime==policy->mtime)return 0;
    nb_policy_t* next=nb_policy_create(policy->path);if(next==NULL)return -1;
    *policy=*next;free(next);return 1;
}

void nb_policy_init(const char* rules_path){
    nb_policy_t* next=nb_policy_create(rules_path);if(next==NULL)return;
    nb_policy_destroy(legacy_policy);legacy_policy=next;
}

void nb_flow_policy_default(nb_flow_policy_t* out){
    memset(out, 0, sizeof(*out));
    out->matched = 0;
    out->flow_class = NB_FLOW_CLASS_BULK;
    out->lane_hint = NB_FLOW_LANE_BULK;
    out->fec_hint = NB_FLOW_FEC_OFF;
    out->prio = NB_PRIO_BULK;
    snprintf(out->rule_name, sizeof(out->rule_name), "default-bulk");
}

static int is_tiktok_udp_media_port(int port){
    switch(port){
    case 50000:
    case 50001:
    case 50008:
    case 50009:
    case 50020:
    case 50021:
        return 1;
    default:
        return 0;
    }
}

int nb_policy_context_classify(nb_policy_t* policy,const char* host,int port,nb_flow_policy_t* out){
    if(policy==NULL||!policy->initialized){nb_flow_policy_default(out);return 0;}
    nb_flow_policy_default(out);
    if(host == NULL || host[0] == 0) return 0;
    if(is_tiktok_udp_media_port(port)){
        out->matched = 1;
        out->flow_class = NB_FLOW_CLASS_MEDIA;
        out->lane_hint = NB_FLOW_LANE_LATENCY;
        out->fec_hint = NB_FLOW_FEC_AUTO;
        out->prio = NB_PRIO_MEDIA;
        snprintf(out->rule_name, sizeof(out->rule_name), "udp-media-port");
        return 1;
    }
    if(port == 443){
        struct in_addr v4;
        struct in6_addr v6;
        if(inet_pton(AF_INET, host, &v4) == 1 || inet_pton(AF_INET6, host, &v6) == 1){
            out->matched = 1;
            snprintf(out->rule_name, sizeof(out->rule_name), "raw-ip-443");
            return 1;
        }
    }
    for(size_t i=0; i<policy->rule_count; i++){
        if(match_pattern_ci(host, policy->rules[i].pattern)){
            out->matched = 1;
            out->flow_class = policy->rules[i].flow_class;
            out->lane_hint = policy->rules[i].lane_hint;
            out->fec_hint = policy->rules[i].fec_hint;
            out->prio = policy->rules[i].prio;
            size_t rule_len=strlen(policy->rules[i].pattern);
            if(rule_len>=sizeof(out->rule_name))rule_len=sizeof(out->rule_name)-1;
            memcpy(out->rule_name,policy->rules[i].pattern,rule_len);out->rule_name[rule_len]=0;
            return 1;
        }
    }
    return 0;
}

int nb_tiktok_flow_classify(const char* host,int port,nb_flow_policy_t* out){
    if(legacy_policy==NULL)nb_policy_init(NULL);
    return nb_policy_context_classify(legacy_policy,host,port,out);
}

int nb_prio_is_latency(int prio){
    return prio <= NB_PRIO_MEDIA;
}

int nb_prio_is_fec_candidate(int prio){
    return prio == NB_PRIO_MEDIA;
}

const char* nb_flow_class_name(nb_flow_class_t c){
    switch(c){
    case NB_FLOW_CLASS_CTRL: return "ctrl";
    case NB_FLOW_CLASS_MEDIA: return "media";
    case NB_FLOW_CLASS_BULK: return "bulk";
    default: return "unknown";
    }
}

const char* nb_flow_lane_name(nb_flow_lane_t l){
    switch(l){
    case NB_FLOW_LANE_LATENCY: return "latency";
    case NB_FLOW_LANE_BULK: return "bulk";
    default: return "default";
    }
}

const char* nb_flow_fec_name(nb_flow_fec_t f){
    switch(f){
    case NB_FLOW_FEC_AUTO: return "auto";
    case NB_FLOW_FEC_OFF: return "off";
    case NB_FLOW_FEC_FORCE_ON: return "force";
    default: return "default";
    }
}
