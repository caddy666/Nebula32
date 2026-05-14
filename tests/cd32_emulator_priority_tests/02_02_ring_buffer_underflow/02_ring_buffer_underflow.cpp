#include <cassert>
#include <vector>

class RingBuffer
{
public:
    RingBuffer(size_t size)
        : buf(size) {}

    bool push(int v)
    {
        if(count == buf.size())
            return false;

        buf[head] = v;
        head = (head + 1) % buf.size();
        count++;
        return true;
    }

    bool pop(int& v)
    {
        if(count == 0)
            return false;

        v = buf[tail];
        tail = (tail + 1) % buf.size();
        count--;
        return true;
    }

private:
    std::vector<int> buf;
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
};

int main()
{
    RingBuffer rb(8);

    for(int i=0;i<8;i++)
        assert(rb.push(i));

    int value;

    for(int i=0;i<8;i++)
        assert(rb.pop(value));

    assert(!rb.pop(value));
}