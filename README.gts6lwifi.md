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
| `.github/workflows/build-kernel.yml` | builds `Image`, modules, `boot.img`, `dtbo.img` and an Odin tar |

### Hardware support

| Block | State |
|---|---|
| CPU, RPMh, clocks, pinctrl, interconnects, SMMU, PMICs | described; drivers built in |
| Memory map | Samsung's layout: TZ region, PIL carve-outs, TIMA/RKP, sec_debug, plog |
| UFS (system), microSD | yes |
| USB | USB 2.0 device mode (adb/MTP gadget). The S2MM005 Type-C/PD chip has no mainline driver |
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
3. Flash `gts6lwifi-mainline-odin.tar` in Odin's **AP** slot. It contains
   `boot.img` and `dtbo.img`. TWRP can flash the two images instead.
4. To go back, flash the stock `boot.img` **and** `dtbo.img` from your
   firmware's AP tar.

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

The toolchain is the AOSP clang that this ACK branch pins (`CLANG_VERSION` in
`bazel/constants.scl`). It comes from `prebuilts/clang/host/linux-x86` on the
`main-kernel` branch, as in ACK's own manifest, and is cached between runs.
Artifacts: `kernel` (Image, Image-dtb, DTBs, config, System.map, build log),
`modules`, and `boot-img` (boot.img, dtbo.img, Odin tar, SHA256SUMS).

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
  result does not depend on the build machine. Every other `gki_defconfig`
  option is unchanged.
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

* **Last kernel log after a failed boot**. ramoops uses the same region and
  record layout as the stock kernel. Boot TWRP (or stock) right after a failed
  mainline boot and read `/sys/fs/pstore/console-ramoops-0`.
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
