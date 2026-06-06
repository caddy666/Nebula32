# Inherit our settings for wolfBoot, TARGET, ARCH, etc.
-include sim.config

# Make sure environment variables do not corrupt the binary output for MacOS users
LANG=
LC_COLLATE="C"
LC_CTYPE="C"
LC_MESSAGES="C"
LC_MONETARY="C"
LC_NUMERIC="C"
LC_TIME="C"
LC_ALL=

APPSRC:=./app
SEVSRC:=./fwserver
WOLFBOOT_ROOT:=./wolfBoot
WOLFSSL_ROOT:=./wolfBoot/lib/wolfssl
WOLFTPM_ROOT:=/usr/local/include/wolftpm/
WOLFMQTT_ROOT:=./wolfMQTT
DEBUG:=0

include $(WOLFBOOT_ROOT)/tools/config.mk
export WOLFBOOT_ROOT


ifneq ("$(wildcard $(WOLFBOOT_ROOT)/tools/keytools/keygen)","")
	KEYGEN_TOOL:=$(WOLFBOOT_ROOT)/tools/keytools/keygen
else
	ifneq ("$(wildcard $(WOLFBOOT_ROOT)/tools/keytools/keygen.exe)","")
		KEYGEN_TOOL:=$(WOLFBOOT_ROOT)/tools/keytools/keygen.exe
	else
		KEYGEN_TOOL:=python3 $(WOLFBOOT_ROOT)/tools/keytools/keygen.py
	endif
endif

ifneq ("$(wildcard $(WOLFBOOT_ROOT)/tools/keytools/sign)","")
	SIGN_TOOL:=$(WOLFBOOT_ROOT)/tools/keytools/sign
else
	ifneq ("$(wildcard $(WOLFBOOT_ROOT)/tools/keytools/sign.exe)","")
		SIGN_TOOL:=$(WOLFBOOT_ROOT)/tools/keytools/sign.exe
	else
		SIGN_TOOL:=python3 $(WOLFBOOT_ROOT)/tools/keytools/sign.py
	endif
endif

# Signing and test variables (used by test-sim-internal-flash-with-update)
PRIVATE_KEY:=$(WOLFBOOT_ROOT)/wolfboot_signing_private_key.der
SIGN_ENV=IMAGE_HEADER_SIZE=$(IMAGE_HEADER_SIZE) \
	WOLFBOOT_PARTITION_SIZE=$(WOLFBOOT_PARTITION_SIZE) \
	WOLFBOOT_SECTOR_SIZE=$(WOLFBOOT_SECTOR_SIZE) \
	NVM_FLASH_WRITEONCE=$(NVM_FLASH_WRITEONCE) \
	ML_DSA_LEVEL=$(ML_DSA_LEVEL) \
	IMAGE_SIGNATURE_SIZE=$(IMAGE_SIGNATURE_SIZE) \
	LMS_LEVELS=$(LMS_LEVELS) \
	LMS_HEIGHT=$(LMS_HEIGHT) \
	LMS_WINTERNITZ=$(LMS_WINTERNITZ) \
	XMSS_PARAMS=$(XMSS_PARAMS)
SIGN_OPTIONS?=--ecc256
TEST_UPDATE_VERSION?=2
OTA_UPDATE_VERSION?=10
BINASSEMBLE:=$(WOLFBOOT_ROOT)/tools/bin-assemble/bin-assemble
DELTA_UPDATE_OPTIONS?=
ifeq ($(NVM_FLASH_WRITEONCE),1)
  INVERSION=| tr "\000" "\377"
else
  INVERSION=
endif

CFLAGS:=-Wall -Wstack-usage=1024 -ffreestanding -Wno-unused -DPLATFORM_$(TARGET) \
        -I$(WOLFBOOT_ROOT)/include -I$(WOLFBOOT_ROOT) -I$(WOLFSSL_ROOT) \
        -I$(WOLFTPM_ROOT) -I$(APPSRC) -I$(WOLFMQTT_ROOT) -DWOLFBOOT_MEASURED_PCR_A \
        -DSIM_OTA=1
CFLAGS+=-DWOLFBOOT_HASH_SHA256

# fwserver CFLAGS
CFLAGS_SEV:=-g -ggdb -Wall -Wstack-usage=1024 -ffreestanding -Wno-unused \
        -I$(WOLFSSL_ROOT) -I$(SEVSRC) -I$(WOLFMQTT_ROOT)

APP_OBJS:= \
	$(APPSRC)/app_$(TARGET).o \
	$(APPSRC)/tpm_handler.o \
	$(APPSRC)/fwclient.o \
	$(APPSRC)/mqttexample.o \
	$(APPSRC)/mqttnet.o \
	$(WOLFBOOT_ROOT)/hal/$(TARGET).o \
	$(WOLFBOOT_ROOT)/src/libwolfboot.o 

# Add objects for wolfMQTT support
APP_OBJS+= \
	$(WOLFMQTT_ROOT)/src/libwolfmqtt_la-mqtt_client.o \
	$(WOLFMQTT_ROOT)/src/libwolfmqtt_la-mqtt_packet.o \
	$(WOLFMQTT_ROOT)/src/libwolfmqtt_la-mqtt_socket.o

# Add objects for fwserver
SEV_OBJS+= \
    $(SEVSRC)/fwpush.o \
	$(SEVSRC)/mqttexample.o \
	$(SEVSRC)/mqttnet.o \
	$(WOLFMQTT_ROOT)/src/libwolfmqtt_la-mqtt_client.o \
	$(WOLFMQTT_ROOT)/src/libwolfmqtt_la-mqtt_packet.o \
	$(WOLFMQTT_ROOT)/src/libwolfmqtt_la-mqtt_socket.o

# Link libwolfssl (full SSL + wolfCrypt) and macOS frameworks when needed
WOLFSSL_LDFLAGS := -L$(WOLFSSL_ROOT)/src/.libs -lwolfssl
ifeq ($(shell uname -s),Darwin)
  WOLFSSL_LDFLAGS += -Wl,-rpath,$(WOLFSSL_ROOT)/src/.libs
  WOLFSSL_LDFLAGS += -framework CoreFoundation -framework Security
else
  WOLFSSL_LDFLAGS += -lm
endif

# Link libwolfTPM (full TPM support) and macOS frameworks when needed
WOLFTPM_LDFLAGS := -L/usr/local/lib -lwolftpm
ifeq ($(shell uname -s),Darwin)
  WOLFTPM_LDFLAGS += -Wl,-rpath,/usr/local/lib
  WOLFTPM_LDFLAGS += -framework CoreFoundation -framework Security
else
  WOLFTPM_LDFLAGS += -lm
endif


# Inherit cross-compiler and similar settings from wolfBoot
include $(WOLFBOOT_ROOT)/arch.mk
# arch.mk sets OBJCOPY only when USE_GCC=1 (from wolfBoot options.mk); sim-OTA does not include it
OBJCOPY ?= objcopy

ifeq ($(DEBUG),0)
	CFLAGS+=-Os -DNDEBUG -flto
else
	CFLAGS+=-g -ggdb3
endif

vpath %.c $(dir $(WOLFSSL_ROOT)/src)
vpath %.c $(dir $(WOLFSSL_ROOT)/wolfcrypt/src)
vpath %.c $(dir $(WOLFBOOT_ROOT))/lib/wolfTPM/wolftpm

LDFLAGS:=$(CFLAGS)
LDFLAGS_SEV:=$(CFLAGS_SEV)

all: $(WOLFBOOT_ROOT)/wolfboot.elf app/image.elf fwserver/fwserver

$(WOLFBOOT_ROOT)/wolfboot.elf: wolfboot_target
	cd $(WOLFBOOT_ROOT) && $(MAKE) WOLFBOOT_ROOT=$$(pwd) wolfboot.elf

app/image.bin: wolfboot_target app/image.elf
	$(OBJCOPY) -O binary app/image.elf $@
	$(SIZE) app/image.elf

app/image.elf: wolfboot_target $(APP_OBJS)
	@echo "\t[LD] $@"
	$(Q)$(LD) $(LDFLAGS) $(APP_OBJS) $(WOLFSSL_LDFLAGS) $(WOLFTPM_LDFLAGS) -o $@
	@echo

fwserver/fwserver: wolfboot_target $(SEV_OBJS)
	@echo "\t[LD] $@"
	$(Q)$(LD) $(LDFLAGS_SEV) $(SEV_OBJS) $(WOLFSSL_LDFLAGS) -o $@
	@echo

wolfboot_target: FORCE
	cp -f sim.config $(WOLFBOOT_ROOT)/.config
	cp ./hal-sim/sim.c $(WOLFBOOT_ROOT)/hal/sim.c
	make -C $(WOLFBOOT_ROOT) include/target.h

wolfboot.bin: wolfBoot/wolfboot.elf
	@echo "\t[BIN] $@"
	$(Q)$(OBJCOPY) $(OBJCOPY_FLAGS) -O binary $^ $@
	@echo

%.o:%.c
	@echo "\t[CC-$(ARCH)] $@"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $^

%.o:%.S
	@echo "\t[AS-$(ARCH)] $@"
	$(Q)$(CC) $(CFLAGS) -c -o $@ $^

test-sim-internal-flash-with-update: wolfboot.bin app/image.elf FORCE
	$(Q)cp app/image.elf app/image.bak.elf
	$(Q)dd if=/dev/urandom bs=1k count=16 >> app/image.elf
	# Create version 1 of the application (base image)
	$(Q)$(SIGN_ENV) $(SIGN_TOOL) $(SIGN_OPTIONS) app/image.elf $(PRIVATE_KEY) 1
	$(Q)cp app/image.bak.elf app/image.elf
	$(Q)dd if=/dev/urandom bs=1k count=16 >> app/image.elf
	$(Q)$(SIGN_ENV) $(SIGN_TOOL) $(SIGN_OPTIONS) app/image.elf $(PRIVATE_KEY) $(TEST_UPDATE_VERSION)
	$(Q)dd if=/dev/zero bs=$$(($(WOLFBOOT_SECTOR_SIZE))) count=1 2>/dev/null $(INVERSION) > erased_sec.dd
	# Sign the update image (version 2 by default)
	# This command handles both standard and delta update modes based on DELTA_UPDATE_OPTIONS
	# empty DELTA_UPDATE_OPTIONS (Without --delta): Produces image_v2_signed.bin
	# DELTA_UPDATE_OPTIONS="--delta app/image_v1_signed.bin": Produces image_v2_signed_diff.bin
	$(Q)$(SIGN_ENV) $(SIGN_TOOL) $(SIGN_OPTIONS) $(DELTA_UPDATE_OPTIONS) \
		app/image.elf $(PRIVATE_KEY) $(TEST_UPDATE_VERSION)
	# Sign the update image for OTA example
	$(Q)$(SIGN_ENV) $(SIGN_TOOL) $(SIGN_OPTIONS) $(DELTA_UPDATE_OPTIONS) \
		app/image.elf $(PRIVATE_KEY) $(OTA_UPDATE_VERSION)
	$(Q)$(BINASSEMBLE) internal_flash.dd \
		0 wolfboot.bin \
		$$(($(WOLFBOOT_PARTITION_BOOT_ADDRESS) - $(ARCH_FLASH_OFFSET))) app/image_v1_signed.bin \
		$$(($(WOLFBOOT_PARTITION_UPDATE_ADDRESS)-$(ARCH_FLASH_OFFSET))) app/image_v$(TEST_UPDATE_VERSION)_signed.bin \
		$$(($(WOLFBOOT_PARTITION_SWAP_ADDRESS)-$(ARCH_FLASH_OFFSET))) erased_sec.dd

clean:
	make -C $(WOLFBOOT_ROOT) clean
	@rm -f *.bin *.elf $(OBJS) wolfboot.map *.bin  *.hex src/*.o tags *.map
	@rm -f app/*.elf app/*.bin app/image.map app/*.o fwserver/*.o fwserver/fwserver

FORCE:

.PHONY: FORCE clean all
