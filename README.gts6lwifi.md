# Android Common Kernel (android-mainline) for the Galaxy Tab S6 Wi-Fi

This is the Android Common Kernel's `android-mainline` branch (Linux 7.3)
ported to the Samsung Galaxy Tab S6 Wi-Fi, SM-T860 (`gts6lwifi`,
Snapdragon 855 / SM8150). The Samsung 4.14 kernel in
[linux-samsung-gts6lwifi](https://github.com/SputnikRocket/linux-samsung-gts6lwifi)
served as the hardware reference.

> **Status: builds, not yet booted on hardware.** Everything below is checked at
> build time. The device tree passes `dtc W=1`, `dtbs_check` and `checkpatch`.
> The boot-chain details were checked against Qualcomm's ABL sources. Expect to
> iterate on a real tablet; [Debugging](#debugging) shows how to read a failed
> boot's log.

## What is in the tree

The first commit is an unmodified snapshot of
`https://android.googlesource.com/kernel/common` `android-mainline`, commit
`1836b19cdc0a`. The commits on top are the port:

| Commit | What |
|---|---|
| `dt-bindings: arm: qcom,ids` / `soc: qcom: socinfo` | SoC ID 361 (SM8150P), which some SM-T860 board revisions use |
| `dt-bindings: arm: qcom` | `samsung,gts6lwifi` compatible |
| `arm64: dts: qcom: Add Samsung Galaxy Tab S6 Wi-Fi` | `sm8150-samsung-gts6lwifi.dts` (and the SM8150P variant) |
| `...: Add a dtbo placeholder` | `sm8150-samsung-gts6lwifi-dtbo.dtso`, and the base DTBs are built with `-@` |
| `ANDROID: arm64: configs: Add gts6lwifi fragment` | `arch/arm64/configs/gts6lwifi.config`, applied on top of `gki_defconfig` |
| `drm/panel: Add Samsung AMSA05RB06` (+ binding) | driver for the AMOLED panel (Anapass ANA38401) |
| `Input: add STMicroelectronics FTS1BA90A` (+ binding) | driver for the touchscreen |
| `...: Enable the panel and touchscreen` | DSI/panel/touch nodes in the DT, plus the matching config |
| `extcon: Add Silicon Mitus SM5705 MUIC` (+ binding) | connects the USB D+/D- lines, which the stock kernel leaves disconnected; the DT node and config follow |
| `ANDROID: arm64: Raise COMMAND_LINE_SIZE to 4096` | the bootloader appends about 2000 characters, past the 2047 upstream keeps |
| `ANDROID: usb: gadget: Skip the uevent hook for legacy gadgets` | the USB serial console build warned in `android_set_connected()` |
| `ANDROID: soc: qcom: rpmh: Skip reads on SoCs that hang on them` | SM8150's RPMh never answers the regulator read-back that 7.3 added, which hung every RPMh regulator (and so storage, USB and the display) |
| `ANDROID: misc: Add gts6lwifi boot diagnostics` | optional timed restart into recovery that first saves the kernel log to the `debug` partition; the log also goes to Samsung's RAM log buffer |
| `.github/workflows/build-kernel.yml` | builds `Image`, modules, `boot.img`, `dtbo.img` and an Odin tar |

### Hardware support

| Block | State |
|---|---|
| CPU, RPMh, clocks, pinctrl, interconnects, SMMU, PMICs | described; drivers built in |
| Memory map | Samsung's layout: TZ region, PIL carve-outs, TIMA/RKP, sec_debug, plog |
| UFS (system), microSD | yes |
| USB | USB 2.0 device mode (adb/MTP gadget). D+/D- run through the SM5705 MUIC, which `extcon-sm5705` connects to the USB PHY (no cable detection yet). The S2MM005 Type-C/PD chip has no mainline driver |
| Display | `panel-samsung-amsa05rb06` (Anapass ANA38401 DDI) on bonded DSI0/DSI1, command mode, 2560x1600@60 via the MSM DPU. Brightness 2–360 nits (`/sys/class/backlight`). The bootloader splash is a `simple-framebuffer` until the MSM driver takes over |
| Touchscreen | `fts1ba90a` (STM FTS1BA90A, Samsung firmware) on i2c17, 10 fingers |
| Power / volume keys, cover hall sensor | yes |
| Debug UART | `uart2` (GPIO 85/86) as `ttyMSM0` |
| Adreno 640 | enabled; needs firmware (`a640_zap.mdt` from vendor, `a640_sqe.fw`/`a640_gmu.bin` from linux-firmware) |
| ADSP / CDSP / SLPI / modem | enabled; load `*.mdt` from the stock firmware partition |
| Wi-Fi / BT (WCN3998) | enabled (ath10k_snoc, hci_qca); needs the modem up and ath10k firmware |
| ramoops | on Samsung's own ramoops region (0xc1300000, same layout as stock) |
| S Pen (Wacom W90xx), audio, charger/fuel gauge, pogo keyboard, cameras | not yet: no mainline drivers |

## How the bootloader handles this image (and what to flash)

The SM-T860 boots with Qualcomm's ABL (LinuxLoader) as modified by Samsung.
These are the facts the port depends on, checked against CodeLinaro's ABL
sources (`QcomModulePkg/Library/BootLib`):

* **boot.img** is header v1 with an empty ramdisk, as on stock. The kernel is
  an uncompressed `Image` behind the `UNCOMPRESSED_IMG` + size header, with the
  base DTBs appended (`Image-dtb`). ABL finds the DTBs right after the Image.
* **dtbo**. When the dtbo partition holds a valid DTBO image, ABL picks the
  appended DTB by SoC ID. It must then find an overlay for the board in the
  dtbo partition and apply it, or it refuses to boot. Samsung's stock overlays
  target downstream labels the mainline DTB does not have, so **flash the
  `dtbo.img` from this build together with `boot.img`**. Its single no-op
  overlay matches every board revision (`qcom,board-id = <8 0>`). It was
  checked with ABL's own libufdt against both base DTBs.
* **root filesystem**. This is a system-as-root device that launched with
  Android 9. ABL appends `skip_initramfs rootwait ro init=/init`, and libavb
  supplies `root=`. With verification **enabled**, libavb emits a legacy `dm=`
  verity table that only Android's old 4.x kernels understand. With
  verification **disabled** it passes `root=PARTUUID=<system>`, which mainline
  mounts directly. So **vbmeta must have verification disabled**, for example
  a vbmeta image made with
  `avbtool make_vbmeta_image --flags 2 --padding_size 4096 --output vbmeta.img`.
* The kernel runtime size must fit below the DTB load address (about 84 MiB on
  SM8150), and boot.img must fit the 64 MiB boot partition. The workflow prints
  both sizes.

### Flashing

1. The bootloader must be unlocked (OEM unlock), as for any custom kernel.
2. Flash a vbmeta with verification disabled (see above).
3. Flash the kernel, either way:
   * **TWRP**: download the `gts6lwifi-mainline-twrp-<run>` artifact and flash
     the downloaded `.zip` as it is (Install, then select the zip). Don't unpack
     it: the artifact *is* the flashable zip. The installer checks that the
     device is an SM-T860, verifies the images' checksums, and checks that
     they fit. It then copies the current `boot` and `dtbo` partitions to
     `/sdcard/gts6lwifi-mainline-backup/<date>/` and flashes `boot.img` and
     `dtbo.img`. It also warns if vbmeta still has verification enabled.
     Leave TWRP's zip signature verification off, since the zip is unsigned.
   * **Odin**: flash `gts6lwifi-mainline-odin.tar` from the `boot-img`
     artifact in the **AP** slot. It contains `boot.img` and `dtbo.img`.
4. To go back, flash the backed-up (or stock) `boot.img` **and** `dtbo.img`,
   for example from TWRP with Install > Install Image.

Samsung's Android 12 vendor partition is built for its 4.14 kernel. Its HALs
need downstream drivers (kgsl, sde, ion, qseecom, ...) that mainline does not
have, so stock Android will not reach the UI. Plan on a GSI with
mainline-friendly HALs, or a Linux userspace.

## Building

### GitHub Actions

Run **Actions → Build kernel + boot.img → Run workflow**. Inputs:

| Input | Default | |
|---|---|---|
| `defconfig` | `gki_defconfig` | base config |
| `config_fragments` | `gts6lwifi.config` | merged on top; the build fails if an option in a fragment does not stick |
| `extra_config` | empty | one-off overrides, e.g. `CONFIG_SM_DISPCC_8250=y` |
| `dtb_names` | both gts6lwifi DTBs | appended to the kernel |
| `dtbo_name` | the placeholder | packaged as `dtbo.img` |
| `cmdline` | empty (workflow default) | boot.img command line |
| `display` | `msm` | `splash`: no MSM display driver, the splash stays up (diagnostic) |
| `usb_console` | off | kernel console on a USB serial gadget (diagnostic) |
| `reboot_to_recovery` | off | save the log to the `debug` partition and restart into TWRP after 30/60/120 s (diagnostic) |

The build also fails if an RPMh regulator in a DTB asks for a voltage its
hardware cannot make (`.github/scripts/check-rpmh-voltages.py`): the
regulator core would then fail every regulator of that PMIC at boot, and
with them storage, USB and the display, without a single visible error.
That happened with LDO17A at 3.3 V (it steps 1.504 V + n * 8 mV).

The toolchain is the AOSP clang that this ACK branch pins (`CLANG_VERSION` in
`bazel/constants.scl`). It comes from `prebuilts/clang/host/linux-x86` on the
`main-kernel` branch, as in ACK's own manifest, and is cached between runs.
Artifacts: `kernel` (Image, Image-dtb, DTBs, config, System.map, build log),
`modules`, `boot-img` (boot.img, dtbo.img, Odin tar, SHA256SUMS), and `twrp`.
The `twrp` artifact is uploaded unpacked, so the zip GitHub serves for download
is the TWRP flashable zip itself. Its installer is `.github/twrp/update-binary`.

### Locally

```bash
export PATH=/path/to/clang-r614150/bin:$PATH
make O=out ARCH=arm64 LLVM=1 gki_defconfig gts6lwifi.config
make O=out ARCH=arm64 LLVM=1 -j$(nproc) Image modules \
    qcom/sm8150-samsung-gts6lwifi.dtb qcom/sm8150p-samsung-gts6lwifi.dtb \
    qcom/sm8150-samsung-gts6lwifi-dtbo.dtbo
```

Packaging into `Image-dtb`, `boot.img` and `dtbo.img` is the Python in the
workflow's "Build kernel" and "Build boot.img and dtbo.img" steps. Host packages
needed: `bc bison flex libssl-dev libelf-dev libdw-dev cpio kmod`, plus pahole
1.26 or newer for BTF. Ubuntu 24.04's 1.25 fails on this kernel ("Reached the
limit of per-CPU variables"). The workflow builds v1.30 from source.

### Config notes

* GKI builds binder and ashmem in Rust, which needs Kleaf's Rust toolchain.
  Plain `make` builds use the C binder (with binderfs) and C ashmem, so the
  result does not depend on the build machine.
* android-mainline's `gki_defconfig` turns on lockdep (`PROVE_LOCKING` and
  the lock debugging it selects); the fragment turns it off, as it slows
  the whole kernel down. Its KASAN is the hardware tag based one, which does
  nothing on this CPU (no MTE). Every other `gki_defconfig` option is
  unchanged.
* The SoC bus node is `soc`, as in Qualcomm's kernels, not mainline's
  `soc@0`: Samsung's vendor init scripts wait for
  `/dev/block/platform/soc/1d84000.ufshc` and mount everything else through
  the `/dev/block/bootdevice` link they make to it. `androidboot.boot_devices`
  is `soc/1d84000.ufshc` to match.
* `CONFIG_CMDLINE_FROM_BOOTLOADER` is set, so GKI's built-in command line
  (including `arm64.no32bit_el0`) is ignored whenever the bootloader passes
  one. Samsung's always does, so 32-bit apps keep working.
* The command line's `clk_ignore_unused pd_ignore_unused` keeps the
  bootloader's display pipeline (and with it the splash framebuffer) alive
  until the MSM display driver takes over.

## Panel and touchscreen notes

Both drivers were written from Samsung's SM-T860 sources and have only been
compile-tested.

* **Panel** (`drivers/gpu/drm/panel/panel-samsung-amsa05rb06.c`). Power-up
  follows the Anapass sequence from the stock driver: supplies, LP-11, reset
  for 70 ms, wait for TCON_RDY (GPIO 140), init in HS, and Display On at least
  220 ms after reset. Commands go over DSI0 only, so the DT bonds the links
  without `qcom,sync-dual-dsi`. Brightness only varies the AMOLED off ratio,
  at the panel's 360-nit gamma calibration. It stays within the AOR range
  Samsung's tables use. Samsung's MTP-based smart dimming, HBM, ACL and
  mDNIe are not implemented.
* **Touchscreen** (`drivers/input/touchscreen/fts1ba90a.c`). It reports in the
  controller's portrait coordinates (1600x2560), while the panel scans
  landscape (2560x1600). If touches don't line up with the screen, add
  `touchscreen-swapped-x-y` and, if needed, `touchscreen-inverted-x` or
  `touchscreen-inverted-y` to the `touchscreen@49` node. The driver force-calibrates
  at probe, as the stock driver does, so don't touch the screen while it
  boots.

## Debugging

* **Kernel log over USB**. This panel only updates when the display engine
  sends it frames, so before the MSM display driver works nothing the kernel
  draws on the splash ever shows. Build with the workflow's `usb_console`
  option instead: the tablet then appears as a USB serial port ("Gadget
  Serial") carrying the kernel console from the start of boot. Android's own
  USB (adb, MTP) doesn't work in that build. On FreeBSD (umodem driver):

  ```bash
  sudo kldload -n umodem; until [ -e /dev/cuaU0 ]; do sleep 0.2; done; sudo cu -l /dev/cuaU0 -s 115200
  ```

  Start that, plug the tablet in, then boot it. Check which `/dev/cuaU*`
  ports exist beforehand: the stock system also has a serial port
  ("SAMSUNG SAMSUNGAndroid"), and another device may already own `cuaU0`.
  On Linux the port is `/dev/ttyACM0` (`picocom /dev/ttyACM0`).
  `sudo usbconfig` (FreeBSD) or `lsusb` (Linux) lists USB devices; the
  kernel's gadget is 0525:a4a7 "Gadget Serial".

  USB only works once the SM5705 MUIC driver has connected D+/D- (see
  `extcon-sm5705.c`): the stock kernel disconnects them at shutdown.

* **Last kernel log after a failed boot**. Restarts on this tablet are hard
  resets (the bootloader logs `HARD_RESET by PS_HOLD`, even when the kernel
  asks for a warm one), which apparently lose memory: pstore and the RAM log
  buffer below came back without the mainline log. Samsung's kernels save
  their log to the `debug` partition at each restart (1 MiB in, 2 MiB long),
  and the bootloader copies its RAM log buffer over that slot at every boot.
  So at every orderly restart or power-off this kernel writes its log, in
  the same format, to the slot for off-mode charging logs, which nothing
  else writes here (6 MiB in, 2 MiB long; `drivers/misc/gts6lwifi-diag.c`).
  It writes with bios rather than through a file, as SELinux does not let
  Android's init, often the task restarting the tablet, write to a block
  device. For a boot that never restarts, build with the workflow's
  `reboot_to_recovery` option, which also saves the log every 15 s
  (`gts6lwifi_diag.save_every`): that many seconds into the boot the kernel
  sets the recovery restart reason and restarts into TWRP
  (`gts6lwifi_diag.reboot_after`), forcing the reset if that hangs. Getting
  to TWRP that way already shows the kernel was still running. Then, in
  TWRP's terminal or over adb:

  ```bash
  dd if=/dev/block/by-name/debug of=/sdcard/mainline-klog.bin bs=1048576 skip=6 count=2
  ```

  The log is plain text after a 16-byte header. If it holds no
  `-gts6lwifi` lines, the kernel did not find the partition: storage did not
  come up.

  If even that finds no partition, the status word still gets through: at
  the same restarts the kernel puts it in the PMIC's RTC alarm value, which
  TWRP's kernel prints at boot (`dmesg | grep sapa_load_alarm`, the `pmic=`
  value) and the bootloader too (`[SAPA] ALARM=` in `/proc/last_kmsg`). It
  starts with hex `a` or `b`; the other bits say which devices have a driver
  bound, and whether a probe was still stuck (`status_devices[]` and
  `STATUS_*` in `drivers/misc/gts6lwifi-diag.c`).

  The kernel also copies its log into Samsung's RAM log buffer (`sec_log=`
  on the bootloader's command line), shown in TWRP's `/proc/last_kmsg` if
  memory ever survives a reset; the end of that file also holds the
  bootloader's own log of each boot, with the reset and boot reasons.
* **Framebuffer console**. `console=tty0` puts the kernel log on the boot
  splash, if the bootloader left the panel running.
* **Serial**. `ttyMSM0` on GPIO 85/86, 115200n8.
* If the bootloader rejects the image, its screen message tells which step
  failed: no matching DTB, DTBO not found or not applied, or the
  signature/vbmeta check.

## Updating to a newer android-mainline

The base is a history-less import, so make a new import commit from the newer
tree and move the port commits onto it:

```bash
git fetch --depth=1 https://android.googlesource.com/kernel/common android-mainline
old=$(git rev-list --max-parents=0 HEAD)
new=$(git commit-tree 'FETCH_HEAD^{tree}' -m "Import Android Common Kernel android-mainline ($(git rev-parse --short FETCH_HEAD))")
git rebase --onto "$new" "$old"
```

The workflow picks up a new `CLANG_VERSION` automatically.
