
#pragma once
#include <cstdint>
#include <vector>

enum CommandType
{
    CMD_INVALID,
    CMD_SEEK,
    CMD_READ
};

struct Command
{
    CommandType type;
    uint32_t lba;
};

inline Command decode_command(const std::vector<uint8_t>& data)
{
    if(data.size() < 4)
        return {CMD_INVALID, 0};

    if(data[0] == 0x12)
    {
        uint32_t lba =
            (data[1] << 16) |
            (data[2] << 8) |
            data[3];

        return {CMD_SEEK, lba};
    }

    return {CMD_INVALID, 0};
}

inline void build_sector(uint32_t lba, uint8_t* sector)
{
    sector[0] = 0x00;

    for(int i=1;i<11;i++)
        sector[i] = 0xFF;

    sector[11] = 0x00;

    sector[12] = (lba >> 16) & 0xFF;
    sector[13] = (lba >> 8) & 0xFF;
    sector[14] = lba & 0xFF;
}

struct MSF
{
    int minute;
    int second;
    int frame;
};

inline MSF lba_to_msf(int lba)
{
    lba += 150;

    MSF m;
    m.minute = lba / (75 * 60);
    m.second = (lba / 75) % 60;
    m.frame = lba % 75;

    return m;
}
