#ifndef NB_CONTROL_H
#define NB_CONTROL_H

#include <stddef.h>

typedef int (*nb_control_render_fn)(const char* command,char* out,size_t out_cap,void* ctx);

int nb_control_open(const char* path,char* error,size_t error_cap);
int nb_control_serve(int listen_fd,nb_control_render_fn render,void* ctx);

#endif
