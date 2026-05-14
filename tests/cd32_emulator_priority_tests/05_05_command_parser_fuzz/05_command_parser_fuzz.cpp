#include <random>
#include <vector>

void decode_command(const std::vector<unsigned char>& p)
{
}

int main()
{
    std::mt19937 rng(12345);

    for(int i=0;i<100000;i++)
    {
        std::vector<unsigned char> p(16);

        for(auto& b : p)
            b = rng() & 0xFF;

        decode_command(p);
    }
}