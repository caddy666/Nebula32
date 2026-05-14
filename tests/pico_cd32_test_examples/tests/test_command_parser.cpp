
#include <cassert>
#include <vector>
#include "../include/cdrom.hpp"

int main()
{
    std::vector<uint8_t> cmd = {
        0x12, 0x00, 0x34, 0x56
    };

    Command c = decode_command(cmd);

    assert(c.type == CMD_SEEK);
    assert(c.lba == 0x3456);

    return 0;
}
