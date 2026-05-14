#include <cassert>
#include <cstring>

int main()
{
    unsigned char bufA[2048];
    unsigned char bufB[2048];

    memset(bufA, 0xAA, sizeof(bufA));
    memset(bufB, 0x55, sizeof(bufB));

    assert(bufA[0] == 0xAA);
    assert(bufB[0] == 0x55);
}