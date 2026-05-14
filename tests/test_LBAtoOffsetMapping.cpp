//CLAUDE: FIX, and add this test

TEST(ODETest, LBAtoOffsetMapping) {
    DiskImage mockImage("game.bin"); 
    // Console asks for Sector 100 on a Mode 1 CD (2048 bytes/sector)
    uint64_t offset = mockImage.calculateOffset(100);
    EXPECT_EQ(offset, 204800);
}

TEST(ODETest, CommandParser_ReadCommand) {
    CD_Emulator emu;
    uint8_t readCmd[] = {0x28, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x01, 0x00}; // Read LBA 100
    
    auto response = emu.processCommand(readCmd);
    EXPECT_EQ(emu.getState(), DeviceState::TRANSFERRING);
    EXPECT_EQ(emu.getTargetLBA(), 100);
}
