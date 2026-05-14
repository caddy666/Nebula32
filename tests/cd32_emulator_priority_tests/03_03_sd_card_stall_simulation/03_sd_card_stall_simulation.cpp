#include <cstdlib>
#include <thread>
#include <chrono>

void sd_read_sector(int lba)
{
    if((rand() % 1000) == 0)
    {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(50)
        );
    }
}

int main()
{
    for(int i=0;i<100000;i++)
        sd_read_sector(i);
}