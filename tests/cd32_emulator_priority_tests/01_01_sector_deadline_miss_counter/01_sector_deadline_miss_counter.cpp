#include <chrono>
#include <thread>
#include <cassert>

using namespace std::chrono;

constexpr double SECTOR_MS = 13.333;

int deadline_miss_count = 0;

void stream_sector(int lba)
{
    std::this_thread::sleep_for(milliseconds(5));
}

int main()
{
    auto next_deadline = steady_clock::now();

    for(int i=0;i<5000;i++)
    {
        auto start = steady_clock::now();

        stream_sector(i);

        auto end = steady_clock::now();

        if(end > next_deadline)
            deadline_miss_count++;

        next_deadline += microseconds(13333);

        std::this_thread::sleep_until(next_deadline);
    }

    assert(deadline_miss_count == 0);
}