#ifndef NB_ROUTES_H
#define NB_ROUTES_H

#include <stddef.h>
#include <stdint.h>

#define NB_ROUTE_MAX 16

typedef struct {char name[32];char hop[256];uint16_t weight;uint32_t capacity;uint32_t active;uint32_t failures;uint64_t unhealthy_until_us;uint64_t selected;uint64_t rejected;} nb_route_entry_t;
typedef struct {nb_route_entry_t entries[NB_ROUTE_MAX];size_t count;uint32_t cursor;uint32_t total_weight;} nb_routes_t;

int nb_routes_load(nb_routes_t* routes,const char* path,char* error,size_t error_cap);
const nb_route_entry_t* nb_routes_pick(nb_routes_t* routes);
const nb_route_entry_t* nb_routes_pick_key(nb_routes_t* routes,uint64_t key,uint64_t now_us);
int nb_routes_acquire(nb_routes_t* routes,const nb_route_entry_t* route);
void nb_routes_release(nb_routes_t* routes,size_t index);
void nb_routes_finish_session(nb_routes_t* routes,size_t index);
void nb_routes_report(nb_routes_t* routes,size_t index,int success,uint64_t now_us,uint64_t cooldown_us);
void nb_routes_report_result(nb_routes_t* routes,size_t index,int result,uint64_t now_us,uint64_t cooldown_us);
int nb_routes_report_name(nb_routes_t* routes,const char* name,int result,uint64_t now_us,uint64_t cooldown_us);
int nb_routes_control_command(nb_routes_t* routes,const char* command,char* out,size_t cap,uint64_t now_us,uint64_t cooldown_us);
uint64_t nb_routes_hash(const char* first,const char* second,uint16_t port);
int nb_routes_render_json(const nb_routes_t* routes,char* out,size_t cap,uint64_t now_us);
int nb_routes_hop_endpoint(const nb_route_entry_t* route,char* host,size_t host_cap,uint16_t* port);
int nb_routes_signal_direct_candidate(const char* rule_name,int udp_mode,int enabled);

#endif
