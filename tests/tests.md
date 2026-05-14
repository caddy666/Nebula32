New TESTS

1. Sector Addressing & Offset Math

An ODE maps LBA (Logical Block Addressing) from the console to Byte Offsets on your SD card.

    LBA to File Offset: If the console asks for Sector 150, does your code correctly calculate 150 * 2048 (for a standard Data CD)?

    Multi-bin/Cue Support: If an image has multiple tracks, does your "Virtual Head" jump to the correct byte offset when a track change is requested?

    Boundary Test: Test the very first sector (0) and the very last sector of a 700MB image to ensure no integer overflows.

2. CD-ROM Protocol State Machine

The console communicates via a handshake 

    Command Parsing: Feed your parser a raw hex buffer (e.g., 0x28 for Read 10). Verify it extracts the correct LBA and length.

    Invalid Commands: What happens if the console sends a "Junk" command? Your unit test should verify the emulator returns a "Check Condition" or "Error" status rather than crashing.

    State Transitions: If the drive is "Seeking," it should reject "Read" commands until the seek timer completes.

3. Data Transfer & Buffer Management

The Pi Pico has limited RAM (264KB). You likely use a "Ping-Pong" buffer to stream from SD while sending to the console.

    Buffer Underflow: Simulate a "Slow SD Card" in your test. If the buffer is empty when the console requests data, does your code signal a "Busy" status or send garbage?

    Partial Reads: Test scenarios where the console asks for 2048 bytes but the SD card read returns only 512 bytes at a time.

    Alignment: Ensure your code handles reads that don't start on a 512-byte sector boundary (though most consoles stick to sector alignment).

4. Timing & Latency (The "Hardware" Unit Test)

While strictly an integration test, you can unit-test your Timing Logic.

    Seek Time Simulation: A real CD-ROM isn't instant; the head has to move. If you respond too fast, some games might glitch. Verify your get_seek_time() function returns a realistic delay based on distance between sectors.

    CDDA (Audio) Streaming: If emulating audio tracks, verify your sample-rate math. If the console expects 44.1kHz, your "buffer-fill" logic needs to stay ahead of that clock.

5. File System (FatFs) Abstraction

Since you'll likely use FatFs to read the SD card:

    Filename Handling: Test that your code can find GAME.GDI, game.gdi, and GAME.gdi (case sensitivity).

    Fragmented Files: Mock a file that is spread across non-contiguous clusters on the SD card. Verify your emulator follows the cluster chain correctly.


6. Command Decoder Tests

These are critical.

The CD32 drive controller will send command packets to the emulator. You need deterministic parsing.

Example commands to test
TEST(parse_seek_command)
{
    uint8_t cmd[] = {0x12, 0x00, 0x34, 0x56};

    command_t c = decode_command(cmd);

    ASSERT_EQ(CMD_SEEK, c.type);
    ASSERT_EQ(0x3456, c.lba);
}
Suggested cases
Test	Purpose
Valid SEEK	Correct LBA decode
Invalid opcode	Reject gracefully
Partial packet	Timeout handling
Malformed checksum	Error path
Repeated command	Idempotency
Rapid command burst	Queue stability
Unknown subcommand	Compatibility
7. Sector Read Tests

This is probably the single most important test group.

Verify:

exact sector size
ECC bytes
sync pattern
subchannel layout
endian correctness
Test raw sector generation
TEST(mode1_sector_size)
{
    uint8_t sector[2352];

    build_mode1_sector(1234, sector);

    ASSERT_EQ(2352, sizeof(sector));
}
Verify sync header
TEST(cd_sync_pattern)
{
    uint8_t sector[2352];

    build_sector(0, sector);

    uint8_t expected[12] = {
        0x00, 0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0x00
    };

    ASSERT_MEM_EQ(expected, sector, 12);
}
8. Subcode/Q Channel Tests

CD32 titles sometimes rely on valid Q subchannel timing.

Test:

track numbers
index values
MSF conversion
CRC generation
Example
TEST(msf_conversion)
{
    msf_t m = lba_to_msf(150);

    ASSERT_EQ(0, m.minute);
    ASSERT_EQ(2, m.second);
    ASSERT_EQ(0, m.frame);
}
Important edge cases
LBA	Expected
0	00:02:00
149	00:03:74
Track boundary	Correct rollover
Lead-out	Valid TOC behavior
9. ISO9660 / Filesystem Tests

If you parse images internally:

Verify
directory traversal
path lookup
sector mapping
Joliet/RR support if implemented
Example
TEST(find_boot_file)
{
    iso_t iso;

    iso_open(&iso, "test.iso");

    file_t *f = iso_find(&iso, "/startup-sequence");

    ASSERT_NOT_NULL(f);
}
10. DMA Ring Buffer Tests

Very important for streaming stability.

Test scenarios
Test	Goal
Buffer wraparound	No corruption
Underflow	Correct stall
Overflow	No overwrite
Double-buffer swap	Seamless transition
Unaligned transfer	Correct copy
Small transfer	No deadlock
Example
TEST(ring_wrap)
{
    ring_t r;

    ring_init(&r, 16);

    for(int i=0;i<32;i++)
        ring_write(&r, i);

    ASSERT_EQ(16, ring_count(&r));
}
11. PIO State Machine Tests

These are integration tests on real hardware.

You want to verify:

clock frequency
bit timing
line polarity
setup/hold timing
Useful tests
Loopback validation

Connect:

TX pin → RX pin

Verify transmitted bitstream equals received.

Timing verification

Use logic analyzer captures and compare against:

real CXD2500 timing
service manual specs

You can automate this with golden captures.

12. Golden Trace Replay Tests

Extremely useful.

Capture:

command stream
timing
responses

from:

real CD32 drive
logic analyzer

Then replay into emulator and compare output.

Example
TEST(replay_boot_sequence)
{
    trace_t t = load_trace("boot.trace");

    emulator_reset();

    replay_trace(&t);

    ASSERT_TRUE(system_booted());
}

This catches:

timing drift
protocol mistakes
edge-case sequencing
13. TOC (Table of Contents) Tests

CD software can be picky here.

Verify
track count
first/last track
lead-out
audio/data flags
Example
TEST(single_track_toc)
{
    toc_t toc;

    build_toc(&toc, image);

    ASSERT_EQ(1, toc.last_track);
}
14. Audio Playback Tests

If supporting CDDA:

Test
sector continuity
pause/resume
seek during playback
stereo alignment
sample rate accuracy
Validate
ASSERT_EQ(44100, playback_rate);
15. Error Injection Tests

Real drives fail in weird ways.

Emulate:

slow seeks
read retries
bad sectors
delayed response
buffer starvation

This is hugely valuable.

Example
TEST(retry_bad_sector)
{
    inject_bad_sector(123);

    int r = read_sector(123);

    ASSERT_EQ(ERR_RETRY, r);
}
16. SD Card / Storage Backend Tests

Since the Pico likely streams from SD:

Verify
fragmented reads
cache correctness
FAT edge cases
large image support
hot removal handling
17. Boot Compatibility Tests

Create automated compatibility suites for:

Game	What to Verify
Microcosm	FMV streaming
Simon the Sorcerer	XA/audio
Alien Breed 3D	seek latency
Chaos Engine	subcode sensitivity
Audio CD	CDDA playback

You can make:

scripted boot tests
serial log assertions
watchdog timeout detection
18. Cycle-Accurate Timing Assertions

Critical for CD32 hardware compatibility.

Measure
command response latency
inter-byte spacing
DMA refill latency
sector delivery cadence

Example:

ASSERT_LT(response_us, 120);
19. Fuzz Tests

Feed random garbage packets into parser.

This catches:

buffer overruns
undefined states
crashes
Example
for(int i=0;i<100000;i++)
{
    randomize(packet, 16);
    decode_command(packet);
}
Recommended Test Architecture
Host-side Tests

Run on PC:

protocol parser
ISO parsing
sector builders
CRC/ECC
ring buffers

Fast CI execution.

Pico Hardware Tests

Run on Pico:

PIO timing
DMA
GPIO edge timing
interrupt latency

Use UART logging for pass/fail.

Hardware-in-Loop

Real CD32 motherboard:

boot tests
streaming validation
long-duration soak tests
Especially Important Tests For Your Project

Given your CXD2500BQ reverse-engineering work, prioritize:

Serial command framing
Q-subcode correctness
DMA underrun handling
PIO timing drift
Seek timing emulation
Sector cadence accuracy
Long streaming stability
Boot-sequence replay tests

Those are the areas most likely to break compatibility with real CD32 hardware.
