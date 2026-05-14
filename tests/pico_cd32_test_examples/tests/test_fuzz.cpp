
#include <random>
#include <vector>
#include "../include/cdrom.hpp"

int main()
{
    std::mt19937 rng(1234);

    for(int i=0;i<100000;i++)
    {
        std::vector<uint8_t> packet(4);

        for(auto& b : packet)
            b = rng() & 0xFF;

        decode_command(packet);
    }

    return 0;
}
