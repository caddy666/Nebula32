
#include <cassert>
#include "../include/ringbuffer.hpp"

int main()
{
    RingBuffer rb(4);

    assert(rb.write(1));
    assert(rb.write(2));
    assert(rb.write(3));
    assert(rb.write(4));

    assert(!rb.write(5));

    uint8_t v = 0;

    assert(rb.read(v));
    assert(v == 1);

    assert(rb.read(v));
    assert(v == 2);

    assert(rb.size() == 2);

    return 0;
}
