all: clone-arm-trusted-firmware build-arm-trusted-firmware configure-uboot-efi build-uboot-efi

clone-arm-trusted-firmware:
	@echo ':: clone arm-trusted-firmware'
	git clone https://github.com/ARM-software/arm-trusted-firmware --depth=1

# From docs/getting_started/image-terminology.rst
# EL3 Runtime Firmware: AP_BL31
# Also known as "SoC AP firmware" or "EL3 monitor firmware". Its primary
# purpose is to handle transitions between the normal and secure world.
build-arm-trusted-firmware:
	@echo ':: build arm-trusted-firmware for rk3399'
	cd arm-trusted-firmware && \
		make CROSS_COMPILE=aarch64-linux-gnu- PLAT=rk3399 -j$(nproc) bl31
	cp arm-trusted-firmware/build/rk3399/release/bl31/bl31.elf ./

configure-uboot-efi:
	@echo ':: configure u-boot efi'
	cd ../ && \
		CROSS_COMPILE=aarch64-linux-gnu- BL31=cbl-build/bl31.elf ARCH=arm64 \
			make pinephone-pro-rk3399_efi_defconfig
build-uboot-efi:
	@echo ':: build u-boot'
	cd ../ && CROSS_COMPILE=aarch64-linux-gnu- BL31=cbl-build/bl31.elf ARCH=arm64 \
			make all

prepare-debian-u-boot:
	@echo ':: setup build environment for u-boot'
	apt install -y build-essential bison flex swig gcc-aarch64-linux-gnu \
                libssl-dev arm-trusted-firmware-tools gcc-arm-none-eabi \
                python3-setuptools python3-dev python3-pyelftools bc

# Boot sequence
# https://opensource.rock-chips.com/wiki_Boot_option
show-flash-commands:
	@echo 'sudo dd if=idbloader.img of=/dev/sda seek=64  oflag=direct,sync status=progress'
	@echo 'sudo dd if=u-boot.itb of=/dev/sda seek=16384  oflag=direct,sync status=progress'
