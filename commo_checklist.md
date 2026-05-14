# CD Player Control System Functionality Checklist

## System Initialization
- [ ] Initialize the CD6 circuit and default register settings (Speed, DAC, MOT output modes) [cite: 1622].
- [ ] Initialize GPIO pins for Pico hardware mapping (UCL, UDAT, ULAT, etc.) [cite: 1921].
- [ ] Perform DSIC2 and CD6 reset sequence including defined delays [cite: 1943, 1945].
- [ ] Initialize command handler state based on door status (Open/Closed) [cite: 1828].
- [ ] Set default lead-in and play parameters [cite: 1830].

## Subcode Management
- [ ] Enable/Disable subcode module for reading [cite: 1624, 1627].
- [ ] Read and store 10-byte subcode frames (Q-buffer) from the CD6 [cite: 1947, 1951].
- [ ] Convert subcode BCD values to hexadecimal for internal processing [cite: 1643, 1645].
- [ ] Validate subcode types (Absolute Time, Catalog Number, ISRC, Lead-in/Program/Lead-out areas) [cite: 1628, 1632, 1637].
- [ ] Copy and update absolute time from subcode buffers [cite: 1639, 1773].

## Player Commands & Playback
- [ ] Issue and monitor player commands (Startup, Play, Pause, Stop, Seek) [cite: 1689, 1700, 1798].
- [ ] Handle speed switching between single (N=1) and double (N=2) modes [cite: 1762, 1765].
- [ ] Monitor playback borders to ensure requested sectors are played [cite: 1807, 1810].
- [ ] Manage audio output states (Mute on/off, Full Scale, Attenuate) [cite: 1983, 1986, 2082].
- [ ] Calculate track jumps based on time differences [cite: 2043, 2047].

## Servo & Motor Control
- [ ] Initialize and monitor sledge position (Check In/Out states) [cite: 2406, 2413, 2418].
- [ ] Manage focus acquisition and recovery routines [cite: 1475, 1478, 1507].
- [ ] Control spindle motor startup sequence (Mode 1 to Mode 2 acceleration) [cite: 1481, 1484, 1518].
- [ ] Perform radial initialization and lock acquisition (including track jumps to gain lock) [cite: 1490, 2292, 2296].
- [ ] Execute long and short jumps using DSIC2 commands [cite: 1534, 1538, 2373].

## Communication & Interface
- [ ] Handle host commands via a command conversion table [cite: 1669, 1814].
- [ ] Report drive progress and status updates to the host (Seeking, Playing, Stopped) [cite: 1705, 1718, 1724].
- [ ] Send ID packages and Q-subcode data packets to the host [cite: 1727, 1738, 1740].
- [ ] Process non-player commands (LED control, Resend, Status requests) [cite: 1730, 1744, 1819].
- [ ] Manage communication state machine for receiving and transmitting data with checksums [cite: 1877, 1883, 1899].

## Error Handling & Safety
- [ ] Translate internal player errors to host-readable error codes [cite: 1678, 1681].
- [ ] Monitor door switch to trigger priority stop if opened during playback [cite: 1801, 1803].
- [ ] Implement recovery states for radial, focus, and subcode timeout errors [cite: 1499, 1507, 1550].
- [ ] Detect and respond to HF (High Frequency) detector status [cite: 1500, 1952].

