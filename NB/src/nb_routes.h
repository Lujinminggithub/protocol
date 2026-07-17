#ifndef NB_ROUTES_H
#define NB_ROUTES_H

#include <stddef.h>
#include <stdint.h>

#define NB_ROUTE_MAX 16

typedef struct {char name[32];char hop[256];uint16_t weight;} nb_route_entry_t;
typedef struct {nb_route_entry_t entries[NB_ROUTE_MAX];size_t count;uint32_t cursor;uint32_t total_weight;} nb_routes_t;

int nb_routes_load(nb_routes_t* routes,const char* path,char* error,size_t error_cap);
const nb_route_entry_t* nb_routes_pick(nb_routes_t* routes);

#endif
