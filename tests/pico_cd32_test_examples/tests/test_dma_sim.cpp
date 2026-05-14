
#include <cassert>
#include <vector>
#include <cstring>

class DMASimulator
{
public:
    void transfer(const uint8_t* src, uint8_t* dst, size_t size)
    {
        std::memcpy(dst, src, size);
    }
};

int main()
{
    DMASimulator dma;

    uint8_t src[8] = {1,2,3,4,5,6,7,8};
    uint8_t dst[8] = {};

    dma.transfer(src, dst, sizeof(src));

    for(int i=0;i<8;i++)
        assert(src[i] == dst[i]);

    return 0;
}
