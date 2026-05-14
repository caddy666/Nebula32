
#include "pico/stdlib.h"
#include <vector>

struct ReplayEvent
{
    uint32_t delay_ns;
    uint8_t gpio_mask;
};

#define PIN_IF_CLK  2
#define PIN_IF_DATA 3
#define PIN_SUBDATA 4

void apply_gpio_mask(uint8_t mask)
{
    gpio_put(PIN_IF_CLK,  mask & (1 << 0));
    gpio_put(PIN_IF_DATA, mask & (1 << 1));
    gpio_put(PIN_SUBDATA, mask & (1 << 2));
}

int main()
{
    stdio_init_all();

    gpio_init(PIN_IF_CLK);
    gpio_init(PIN_IF_DATA);
    gpio_init(PIN_SUBDATA);

    gpio_set_dir(PIN_IF_CLK, GPIO_OUT);
    gpio_set_dir(PIN_IF_DATA, GPIO_OUT);
    gpio_set_dir(PIN_SUBDATA, GPIO_OUT);

    std::vector<ReplayEvent> trace;

    absolute_time_t start =
        get_absolute_time();

    uint64_t accumulated_ns = 0;

    for(const auto& e : trace)
    {
        accumulated_ns += e.delay_ns;

        while(to_us_since_boot(
                get_absolute_time())
                < (accumulated_ns / 1000))
        {
        }

        apply_gpio_mask(e.gpio_mask);
    }
}
