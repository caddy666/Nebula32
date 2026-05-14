
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iostream>

struct ReplayEvent
{
    uint32_t delay_ns;
    uint8_t gpio_mask;
};

int main()
{
    std::ifstream file("sample_digital.csv");

    std::string line;

    std::getline(file, line);

    std::vector<ReplayEvent> events;

    uint64_t previous_time = 0;

    while(std::getline(file, line))
    {
        std::stringstream ss(line);

        std::string timestamp;

        std::getline(ss, timestamp, ',');

        uint64_t current_time =
            static_cast<uint64_t>(events.size() * 63);

        uint8_t mask = 0;

        for(int i=0;i<8;i++)
        {
            std::string bit;

            std::getline(ss, bit, ',');

            if(bit == "1")
                mask |= (1 << i);
        }

        ReplayEvent e;

        e.delay_ns =
            (previous_time == 0)
            ? 0
            : static_cast<uint32_t>(
                current_time - previous_time
            );

        e.gpio_mask = mask;

        events.push_back(e);

        previous_time = current_time;
    }

    std::ofstream out(
        "trace.bin",
        std::ios::binary
    );

    out.write(
        reinterpret_cast<char*>(events.data()),
        events.size() * sizeof(ReplayEvent)
    );

    std::cout << "Converted "
              << events.size()
              << " events\n";
}
