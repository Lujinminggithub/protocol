#include "nb_transport_profile.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(value) do{if(!(value)){fprintf(stderr,"check failed line=%d\n",__LINE__);return 1;}}while(0)

int main(void){
    char path[]="nb-profile-XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);
    const char* text="schema=1\nline_id=line-1\ngeneration=2\nrole=middle\n"
        "ingress.cc=cubic\ningress.bbr_options=\ningress.cwin_max_bytes=524288\ningress.mtu_max=1404\n"
        "ingress.reorder_gap=8\ningress.reorder_delay_us=20000\ningress.udp_gso=false\ningress.fec_observe=true\ningress.fec_active=false\n"
        "ingress.udp_fec_adaptive=false\ningress.udp_fec_k=8\ningress.udp_fec_hold_us=2000\n"
        "ingress.target_rate_bps=5000000\ningress.seed_rtt_us=5000\ningress.startup_cwin_bytes=262144\n"
        "egress.cc=bbr\negress.bbr_options=Q0.0001:\negress.cwin_max_bytes=0\negress.mtu_max=1404\n"
        "egress.reorder_gap=128\negress.reorder_delay_us=410000\negress.udp_gso=false\negress.fec_observe=true\negress.fec_active=false\n"
        "egress.udp_fec_adaptive=true\negress.udp_fec_k=8\negress.udp_fec_hold_us=2000\n"
        "egress.target_rate_bps=5000000\negress.seed_rtt_us=202000\negress.startup_cwin_bytes=262144\n";
    CHECK(write(fd,text,strlen(text))==(ssize_t)strlen(text));close(fd);chmod(path,0600);
    char absolute[1024],cwd[900];CHECK(getcwd(cwd,sizeof(cwd))!=NULL);
    CHECK(snprintf(absolute,sizeof(absolute),"%s/%s",cwd,path)>0);
    char error[256];nb_transport_profile_t loaded;int load=nb_transport_profile_load(absolute,&loaded,error,sizeof(error));
    if(load!=0)fprintf(stderr,"load failed: %s path=%s\n",error,absolute);
    CHECK(load==0&&loaded.egress.udp_fec_adaptive==1&&loaded.egress.udp_fec_k==8&&loaded.egress.udp_fec_hold_us==2000);
    CHECK(loaded.egress.target_rate_bps==5000000&&loaded.egress.seed_rtt_us==202000&&loaded.egress.startup_cwin_bytes==262144);
    CHECK(nb_transport_shared_cwin_max(&loaded.ingress,&loaded.egress)==0);
    nb_transport_link_profile_t bounded=loaded.egress;bounded.cwin_max_bytes=1048576;
    CHECK(nb_transport_shared_cwin_max(&loaded.ingress,&bounded)==1048576);
    bounded.present=0;
    CHECK(nb_transport_shared_cwin_max(&loaded.ingress,&bounded)==loaded.ingress.cwin_max_bytes);
    nb_transport_profile_state_t state;nb_transport_profile_state_init(&state);
    CHECK(nb_transport_profile_prepare(&state,absolute,2,loaded.fingerprint,"line-1","middle",error,sizeof(error))==0);
    CHECK(nb_transport_profile_commit(&state,2,error,sizeof(error))==0&&state.active.generation==2);
    CHECK(nb_transport_profile_prepare(&state,absolute,2,loaded.fingerprint+1,"line-1","middle",error,sizeof(error))!=0);
    CHECK(nb_transport_profile_abort(&state,3)==0);char status[512];CHECK(nb_transport_profile_render_status(&state,status,sizeof(status))>0);
    CHECK(strstr(status,"\"active_generation\":2")!=NULL);
    fd=open(path,O_WRONLY|O_APPEND);CHECK(fd>=0);const char* duplicate="role=middle\n";
    CHECK(write(fd,duplicate,strlen(duplicate))==(ssize_t)strlen(duplicate));close(fd);
    CHECK(nb_transport_profile_load(absolute,&loaded,error,sizeof(error))!=0);
    const char* legacy="schema=1\nline_id=line-1\ngeneration=2\nrole=entry\n"
        "egress.cc=bbr\negress.mtu_max=1404\negress.reorder_gap=8\negress.reorder_delay_us=20000\n";
    fd=open(path,O_WRONLY|O_TRUNC);CHECK(fd>=0);CHECK(write(fd,legacy,strlen(legacy))==(ssize_t)strlen(legacy));close(fd);
    CHECK(nb_transport_profile_load(absolute,&loaded,error,sizeof(error))==0&&loaded.egress.target_rate_bps==0);
    const char* partial="egress.target_rate_bps=5000000\n";
    fd=open(path,O_WRONLY|O_APPEND);CHECK(fd>=0);CHECK(write(fd,partial,strlen(partial))==(ssize_t)strlen(partial));close(fd);
    CHECK(nb_transport_profile_load(absolute,&loaded,error,sizeof(error))!=0);
    const char* schema2="schema=2\nline_id=line-1\ngeneration=3\nrole=middle\n"
        "ingress.cc=cubic\ningress.cwin_max_bytes=524288\ningress.mtu_max=1404\ningress.reorder_gap=8\ningress.reorder_delay_us=20000\n"
        "egress.cc=bbr\negress.cwin_max_bytes=0\negress.mtu_max=1404\negress.reorder_gap=16\negress.reorder_delay_us=120000\n"
        "egress.udp_fec_mode=nb-yfe2-optional\n";
    fd=open(path,O_WRONLY|O_TRUNC);CHECK(fd>=0);CHECK(write(fd,schema2,strlen(schema2))==(ssize_t)strlen(schema2));close(fd);
    CHECK(nb_transport_profile_load(absolute,&loaded,error,sizeof(error))==0);
    CHECK(loaded.schema_version==2&&!strcmp(loaded.egress.udp_fec_mode,"nb-yfe2-optional"));
    const char* schema1_mode="schema=1\nline_id=line-1\ngeneration=3\nrole=middle\n"
        "ingress.cc=cubic\ningress.cwin_max_bytes=524288\ningress.mtu_max=1404\ningress.reorder_gap=8\ningress.reorder_delay_us=20000\n"
        "egress.cc=bbr\negress.cwin_max_bytes=0\negress.mtu_max=1404\negress.reorder_gap=16\negress.reorder_delay_us=120000\n"
        "egress.udp_fec_mode=nb-yfe2-optional\n";
    fd=open(path,O_WRONLY|O_TRUNC);CHECK(fd>=0);CHECK(write(fd,schema1_mode,strlen(schema1_mode))==(ssize_t)strlen(schema1_mode));close(fd);
    CHECK(nb_transport_profile_load(absolute,&loaded,error,sizeof(error))!=0);
    const char* entry_mode="schema=2\nline_id=line-1\ngeneration=3\nrole=entry\n"
        "egress.cc=cubic\negress.cwin_max_bytes=524288\negress.mtu_max=1404\negress.reorder_gap=8\negress.reorder_delay_us=20000\n"
        "egress.udp_fec_mode=nb-yfe2-optional\n";
    fd=open(path,O_WRONLY|O_TRUNC);CHECK(fd>=0);CHECK(write(fd,entry_mode,strlen(entry_mode))==(ssize_t)strlen(entry_mode));close(fd);
    CHECK(nb_transport_profile_load(absolute,&loaded,error,sizeof(error))!=0);
    unlink(path);puts("transport profile tests passed");return 0;
}
