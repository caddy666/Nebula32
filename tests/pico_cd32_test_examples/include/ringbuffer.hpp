
#pragma once
#include <vector>
#include <cstdint>

class RingBuffer
{
public:
    explicit RingBuffer(size_t size)
        : buffer(size)
    {
    }

    bool write(uint8_t value)
    {
        if(count == buffer.size())
            return false;

        buffer[head] = value;
        head = (head + 1) % buffer.size();
        count++;
        return true;
    }

    bool read(uint8_t& value)
    {
        if(count == 0)
            return false;

        value = buffer[tail];
        tail = (tail + 1) % buffer.size();
        count--;
        return true;
    }

    size_t size() const
    {
        return count;
    }

private:
    std::vector<uint8_t> buffer;
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
};
