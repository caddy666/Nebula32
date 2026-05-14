#include <cassert>

struct MSF
{
    int minute;
    int second;
    int frame;
};

MSF lba_to_msf(int lba)
{
    lba += 150;

    return {
        lba / (75 * 60),
        (lba / 75) % 60,
        lba % 75
    };
}

int main()
{
    MSF m = lba_to_msf(0);

    assert(m.minute == 0);
    assert(m.second == 2);
    assert(m.frame == 0);
}