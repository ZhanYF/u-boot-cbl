### u-boot-cbl

**This fork is no longer being worked on**

U-Boot Common Base Loader: A fork of megi's pinephone pro u-boot tree with additional tweaks

Initial import from https://xff.cz/git/u-boot/log/?h=ppp-2023.07

Build

```
make -C ./cbl-build
```

Install (assume /dev/sda is your EMMC or sdcard)

```
sudo dd if=idbloader.img of=/dev/sda seek=64  oflag=direct,sync status=progress
sudo dd if=u-boot.itb of=/dev/sda seek=16384  oflag=direct,sync status=progress
```
