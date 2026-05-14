
#include <cassert>
#include <cstring>
#include "../include/cdrom.hpp"

int main()
{
    uint8_t sector[2352] = {};

    build_sector(0x123456, sector);

    assert(sector[0] == 0x00);

    for(int i=1;i<11;i++)
        assert(sector[i] == 0xFF);

    assert(sector[11] == 0x00);

    assert(sector[12] == 0x12);
    assert(sector[13] == 0x34);
    assert(sector[14] == 0x56);

    return 0;
}
