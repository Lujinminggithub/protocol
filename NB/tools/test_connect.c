#include <assert.h>
#include <stdint.h>

#include "nb_connect.h"

int main(void)
{
    assert(nb_connect_deadline_us(UINT64_MAX) == UINT64_MAX);
    assert(nb_connect_expired(UINT64_MAX, UINT64_MAX) == 0);
    assert(nb_connect_expired(1000000ULL, 5999999ULL) == 0);
    assert(nb_connect_expired(1000000ULL, 6000000ULL) == 1);
    assert(nb_connect_deadline_us(UINT64_MAX - 4999999ULL) == UINT64_MAX);
    assert(nb_connect_deadline_us(1000000ULL) == 6000000ULL);
    return 0;
}
