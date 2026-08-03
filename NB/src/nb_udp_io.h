#ifndef NB_UDP_IO_H
#define NB_UDP_IO_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef struct {
    uint32_t last;
    int seen;
} nb_udp_rxq_state_t;

int nb_udp_enable_rxq_overflow(int fd);
int nb_udp_configure_buffers(int fd, int requested,
    int* actual_receive, int* actual_send);
uint64_t nb_udp_rxq_overflow_update(nb_udp_rxq_state_t* state,
    const struct msghdr* message);
ssize_t nb_udp_recv(int fd, void* buffer, size_t length, int flags,
    struct sockaddr* source, socklen_t* source_length,
    nb_udp_rxq_state_t* rxq_state, uint64_t* dropped);
int nb_udp_bind_relay_socket(uint16_t port_min, uint16_t port_max,
    uint16_t* next_port, struct sockaddr_in* bound);
int nb_udp_endpoint_equal(const struct sockaddr_storage* a,
    const struct sockaddr_storage* b);
size_t nb_udp_send_gso(int fd, const uint8_t* buffer, size_t length,
    size_t segment_size, const struct sockaddr* destination,
    socklen_t destination_length);

#endif
