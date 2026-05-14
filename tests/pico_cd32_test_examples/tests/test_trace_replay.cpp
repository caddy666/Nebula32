
#include <cassert>
#include <vector>
#include "../include/cdrom.hpp"

struct TraceEntry
{
    std::vector<uint8_t> packet;
};

int main()
{
    std::vector<TraceEntry> trace = {
        {{0x12, 0x00, 0x10, 0x00}},
        {{0x12, 0x00, 0x20, 0x00}},
    };

    for(const auto& t : trace)
    {
        Command c = decode_command(t.packet);

        assert(c.type == CMD_SEEK);
    }

    return 0;
}
