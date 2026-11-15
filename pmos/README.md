# Nura for the Wileyfox Swift (Longcheer L8150) - SDR setup

Installs the official Nura image (formerly postmarketOS) with a small set of
changes needed to work with the modem:

- **modem (`mpss`) region 80 -> 88 MiB (`0x8e800000`, size `0x5800000`)**
  - Where: `kernel/patches/0001-*.patch` (DTS)
  - Purpose: room for custom modem firmware images
- **`CONFIG_STRICT_DEVMEM=n`**
  - Where: `kernel/kbuild_inner.sh`
  - Purpose: `mmap` of `/dev/mem` on any physical address, including modem RAM
- **unrestricted `valid_phys_addr_range()`**
  - Where: `kernel/patches/0002-*.patch`
  - Purpose: `read()`/`write()` on `/dev/mem` for IMEM (on-chip memory), SMEM
    (memory shared by the Linux side and the modem) and `no-map` regions (skip
    with `PERMISSIVE_RW=0`)
- **modem DIAG channel as `/dev/wwan0qcdm0`**
  - Where: `kernel/patches/0003-*.patch` (module `rpmsg_wwan_ctrl`)
  - Purpose: needed for the modem's factory test mode (FTM, the test mode in
    which manufacturers tune the radio) that sample streaming uses; the module
    is built with the kernel and installed into
    `/lib/modules/6.12.1-msm8916/updates/`
- **audio module blacklist, firmware switch script, console fix**
  - Where: `rootfs/`, installed by `install_config.sh`
  - Purpose: stable boot; switching the modem to a custom firmware image
  - The audio blacklist is needed only when the modem firmware is switched while
    the phone runs (the modem is restarted through remoteproc, and the audio
    drivers do not survive that). If the phone boots straight into the custom
    firmware (`qcom-sdr-modem-fw.service` enabled), the blacklist is not needed
    and sound works normally: remove
    `/etc/modprobe.d/blacklist-audio-unstable.conf` and reboot.

The rest of the kernel configuration is taken from the official kernel (read
from the configuration embedded in its `vmlinuz`), so the kernel release stays
`6.12.1-msm8916` and the modules in the official rootfs keep working. Nothing
from the official image is redistributed: the scripts download it and modify it
locally.

Abbreviations are explained in the [glossary](../README.md#glossary).

## Requirements

A Linux host with `docker`, `fastboot`, `xz`, `simg2img`/`img2simg`
(android-sdk-libsparse-utils), `debugfs`/`e2fsck` (e2fsprogs), `python3`, `curl`
and a USB cable. Flashing **erases the phone's `userdata`** (Android and all its
data).

## Installation

```
# 0. Download and check
#    rootfs: the newest image of https://images.nura.eco/bpo/v26.06/wileyfox-crackling/phosh/
#            (old builds are removed after a few weeks):
#            <date>-nura-v26.06-phosh-wileyfox-crackling.img.xz (not the -boot one), check it against its .sha256
#            file. It must have kernel 6.12.1-msm8916 (build_kernel.sh checks this). Tested with 20261113-1409.
#    lk2nd:  https://github.com/msm8916-mainline/lk2nd/releases/download/23.1/lk2nd-msm8916.img
#            sha256 8f5f788b45b96447517ff121f20841e6bfc95a5d7c4b48fdedb855d90f593144

# 1. Kernel and DTB (docker, a few minutes)
bash extract_official.sh ROOTFS.img.xz  WORK/official
bash kernel/build_kernel.sh WORK/official/vmlinuz  WORK/kbuild      # -> WORK/kbuild/out/

# 2. Rootfs image with the slightly patched kernel and DTB (offline, does not touch the phone)
bash prepare_image.sh ROOTFS.img.xz  WORK/kbuild/out  WORK/img      # -> WORK/img/pmos-rootfs-modified.img

# 3. Flash (ERASES userdata). Reboot the phone into the bootloader first (adb reboot bootloader, or Vol-Down + Power).
fastboot flash userdata WORK/img/pmos-rootfs-modified.img
fastboot boot lk2nd-msm8916.img                  # test run of lk2nd from RAM
fastboot flash boot lk2nd-msm8916.img            # make lk2nd permanent once pmOS has started

# 4. First boot takes several minutes (the USB id 18d1:d001 during boot is normal).
#    On the phone: Settings -> System -> Secure Shell -> enable (otherwise SSH refuses connections).
ssh user@172.16.42.1                             # log in with the password (after a reinstall: ssh-keygen -R 172.16.42.1)
scp check_devmem.py user@172.16.42.1:/tmp/
ssh -t user@172.16.42.1 'sudo python3 /tmp/check_devmem.py'   # verifies /dev/mem and the DTS change
bash install_config.sh user@172.16.42.1 WORK/kbuild/out/rpmsg_wwan_ctrl.ko   # audio blacklist, firmware switch script, console fix, DIAG module
                                                 # (reboot the phone afterwards)
```

## Login

The official image logs in with user `user` and password `147147` (also used by
`sudo`).

## Notes

- lk2nd boots the system from `/extlinux/extlinux.conf` on the boot partition
  inside the rootfs image. Do not flash the `boot.img` shipped inside the image:
  it contains the stock kernel (STRICT_DEVMEM on, 80 MiB modem region).
- Updating the kernel package with `apk` overwrites `/boot/vmlinuz` and the DTB
  with the official ones. Pin the package or put the custom files back after
  such an update.
