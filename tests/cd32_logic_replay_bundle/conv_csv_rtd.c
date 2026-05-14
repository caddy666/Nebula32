#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <ctime>

struct ReplayEvent
{
    uint32_t delay_ns;
    uint8_t gpio_mask;
};

static uint64_t parse_time_ns(const std::string& s)
{
    // VERY simplified parser example.
    // Production version should properly parse ISO8601.

    auto pos = s.find_last_of(':');

    double sec = std::stod(s.substr(pos + 1));

    return (uint64_t)(sec * 1e9);
}

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

        std::string time_str;

        std::getline(ss, time_str, ',');

        uint64_t current_time = parse_time_ns(time_str);

        uint8_t mask = 0;

        for(int i=0;i<8;i++)
        {
            std::string bit;
            std::getline(ss, bit, ',');

            if(std::stoi(bit))
                mask |= (1 << i);
        }

        ReplayEvent e;

        e.delay_ns =
            (previous_time == 0)
            ? 0
            : (uint32_t)(current_time - previous_time);

        e.gpio_mask = mask;

        events.push_back(e);

        previous_time = current_time;
    }

    std::ofstream out("trace.bin", std::ios::binary);

    out.write(
}
