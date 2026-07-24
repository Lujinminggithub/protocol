#ifndef NB_UDP_IO_H
#define NB_UDP_IO_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

int nb_udp_bind_relay_socket(uint16_t port_min, uint16_t port_max,
    uint16_t* next_port, struct sockaddr_in* bound);
int nb_udp_endpoint_equal(const struct sockaddr_storage* a,
    const struct sockaddr_storage* b);
size_t nb_udp_send_gso(int fd, const uint8_t* buffer, size_t length,
    size_t segment_size, const struct sockaddr* destination,
    socklen_t destination_length);

#endif
