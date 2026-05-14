
#include <cassert>
#include "../include/cdrom.hpp"

int main()
{
    MSF m = lba_to_msf(0);

    assert(m.minute == 0);
    assert(m.second == 2);
    assert(m.frame == 0);

    return 0;
}
