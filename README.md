# TWRP Device Tree for Onyx Boox C69HD (`C69_M_STAR`)

An open-source TeamWin Recovery Project (TWRP 11.0) device tree for the **Onyx Boox C69HD** (`C69_M_STAR`, Qualcomm SM6225 / Bengal).

Forked and enhanced from [@7sarus/android_device_onyx_c69](https://github.com/7sarus/android_device_onyx_c69) with **native Qualcomm Atomic DRM EPDC E-Ink display drivers** and **full touchscreen support**.

[![Build TWRP Recovery](https://github.com/Zeusx212/android_device_onyx_c69/actions/workflows/twrp.yml/badge.svg)](https://github.com/Zeusx212/android_device_onyx_c69/actions/workflows/twrp.yml)

> [!IMPORTANT]
> **Zero-Risk Testing via Unlocked Bootloader**:
> The bootloader on the Onyx Boox C69HD is **unlocked** (`unlocked:yes`, `critical unlocked: true`).
> **NEVER flash custom recovery to internal storage directly.** Always tether-boot builds safely in RAM:
> ```bash
> fastboot boot recovery.img
> ```

> [!NOTE]
> **E-Ink Display & Touch Support**:
> This fork solves the display frozen / blank screen issue on the Onyx Boox C69HD. On Qualcomm Bengal/Scuba, the EPDC hardware requires vendor atomic DRM plane properties (`EPDC_UPDATE_PARMS_ADDR` and `EPDC_UPDATE_CNT`) and specific plane binding (`Plane 68` / `SSPP ViG0`) committed with `DRM_MODE_ATOMIC_NONBLOCK` to pulse waveforms across the E-Ink panel without triggering continuous splash SMMU page faults.

---

## Current Status

| Feature | Status | Notes |
| :--- | :--- | :--- |
| **Build & CI** | Working | Compiles cleanly via GitHub Actions & minimal manifest |
| **Kernel Boot** | Working | Boots successfully into RAM (`fastboot boot recovery.img`) |
| **USB / ADB** | Working | Enumerates as recovery device; root ADB shell fully functional |
| **E-Ink Display** | **Verified Working** | Native Atomic DRM EPDC driver on Plane 68 (`EPDC_UPDATE_PARMS_ADDR` + `EPDC_UPDATE_CNT`) |
| **Touchscreen** | **Verified Working** | FocalTech `fts_ts` multi-touch digitizer (`/dev/input/event4`), full GUI navigation |
| **Physical Keys** | Working | Page Up (`0x68`), Page Down (`0x6d`), Home/Back (`0x8f`), Power (`0x74`) |

---

## Technical Deep-Dive: Onyx Boox C69HD Display Pipeline & EPDC Solution

### 1. Dual-Display Hardware Architecture
The Onyx Boox C69HD runs two concurrent display interfaces in kernel DRM:
- **Display 0 (Connector 29, CRTC 75)**: Command-mode interface (`1024x758 @ 40Hz`). Uses **Plane 68** (`SSPP ViG0`). This is where userspace (TWRP GUI and Android SurfaceFlinger) renders framebuffers and queues EPDC waveform updates.
- **Display 1 (Connector 46, CRTC 112)**: Hardware Video-mode interface (`301x769 @ 90Hz`, DSI-2). Uses **Plane 65** (`SSPP DMA0`). This is the hardware timing controller link to the E-Ink EPDC TCON (`qcom,mdss_dsi_epdc_cmd`).

### 2. The SMMU Page Fault & Continuous Splash Trap
- In Qualcomm SDE KMS, the bootloader leaves continuous splash active in physical RAM at `0x5c000000`, scanning out through Plane 65 on CRTC 112.
- If userspace commits an atomic request with `DRM_MODE_ATOMIC_ALLOW_MODESET` (`0x100`) or attempts to modify/disable CRTC 112, the kernel DRM driver calls `_sde_kms_release_splash_resource` and unmaps `0x5c000000` from the SMMU.
- Because Plane 65 continues fetching from `0x5c000000`, this triggers infinite SMMU page faults (`iova=0x5c000000, client name: mdp_0`), choking the MDP bus and freezing the screen permanently on the bootloader logo.

### 3. The Solution in `patches/graphics_drm.cpp`
By reverse-engineering the stock recovery binary (`librecovery_ui.so`), we matched the exact vendor pipeline mechanics:
1. **Leave CRTC 112 / DSI-2 Untouched**:
   Userspace never touches CRTC 112 or Connector 46, matching stock Android SurfaceFlinger.
2. **Bind to Primary Graphics Plane 68**:
   Enumerate planes from `drmModeGetPlaneResources()`, skip plane index 0 (Plane 65 / continuous splash), and bind to plane index 1 (**Plane 68**). Plane 49 is an internal kernel plane reserved for waveform dispatch.
3. **Commit with `DRM_MODE_ATOMIC_NONBLOCK` (`0x400`)**:
   Both initial pipeline modeset and subsequent flips commit with `DRM_MODE_ATOMIC_NONBLOCK`. This never triggers `_sde_kms_release_splash_resource`, keeping `0x5c000000` mapped and eliminating 100% of SMMU faults.
4. **Qualcomm EPDC Waveform Parameters**:
   Every frame attaches `EPDC_UPDATE_PARMS_ADDR` and `EPDC_UPDATE_CNT` on Plane 68, queuing full GC16 waveform updates to the physical panel.

---

## Hardware Architecture

```mermaid
graph TD
    subgraph SoC["Qualcomm Scuba IoT / Bengal (4x Cortex-A53 @ 2.0 GHz)"]
        CPU["CPU Cores (0x801 rev 4)"]
        DSI["DSI Controller (5e94000)"]
        SDHC1["SDHC1 eMMC (4744000)"]
        SDHC2["SDHC2 TF Slot (4784000)"]
        I2C1["I2C Bus 1 (qupv3_se1_i2c)"]
        I2C2["I2C Bus 2 (qupv3_se2_i2c)"]
        GPIO["GPIO Subsystem (TLMM 500000)"]
    end

    subgraph Peripherals["Power & Display Drivers"]
        PMIC_EINK["TI TPS6518x E-Ink PMIC (0x68)"]
        SW_EPDC["Onyx Software EPDC (soc:sepdc_mfd)"]
        PANEL["6.0' E-Ink Carta (758x1024, 212 PPI)"]
        AMP["Qualcomm WSA881x Smart Amp"]
    end

    subgraph Input["Input & Controls"]
        TOUCH["FocalTech Touchscreen (fts_ts)"]
        KEYS["2x Side Page Turn Keys (gpio-keys)"]
        HOME["Central Home/Back Key (onyx-misc)"]
        PWR["Power Key (qpnp_pon)"]
        HALL["Smart Lid Sensor (onyx-hall)"]
    end

    I2C1 --> PMIC_EINK
    PMIC_EINK --> PANEL
    DSI --> SW_EPDC --> PANEL
    GPIO --> KEYS
    GPIO --> HOME
    GPIO --> PWR
    GPIO --> HALL
    CPU --> TOUCH
    I2C1 --> AMP
```

---

## Detailed Hardware Specifications

| Component | Specification | Live Verified Source |
| :--- | :--- | :--- |
| **Model** | Onyx Boox C69HD (`C69_M_STAR`) | `ro.product.model = C69_M_STAR` |
| **SoC Platform** | Qualcomm SM6225 Bengal / Scuba IoT | Platform `bengal`, variant `QCS EMMC` (`001860e1`) |
| **CPU Architecture** | 4× ARM Cortex-A53 @ 2.0 GHz | 64-bit kernel, `arm64-v8a` ABI (Part `0x801`) |
| **RAM** | 2 GB LPDDR4X | `MemTotal: 1817196 kB` |
| **Internal Storage** | 16 GB eMMC | `/dev/block/mmcblk0` (`MMC16G`, Micron/Spectek) |
| **MicroSD Expansion** | TF Slot (Dedicated SDHCI) | `/dev/block/mmcblk1p1` (`sdhci@4784000`) |
| **Display Panel** | 6.0" E-Ink Carta | 758 × 1024 pixels (212 PPI, 4:3 aspect ratio) |
| **E-Ink Display PMIC** | Texas Instruments **TPS6518x** | I2C1 @ `0x68` (VPOS +15V, VNEG -15V, VEE -20V, VDD +22V) |
| **Audio Amplifier** | Qualcomm **WSA881x** | Smart speaker amplifier (`wsa881x-i2c-codec`) |
| **Touchscreen Digitizer** | FocalTech `fts_ts` | 1448 × 1072 digitized surface grid (`/dev/input/event4`) |
| **Front Light / LEDs** | **None** | Completely unpopulated (pure ambient reflective) |
| **Kernel Version** | Linux `4.19.157-perf` | Stock prebuilt kernel included in tree |

---

## Physical Controls & Keycode Mappings

```mermaid
flowchart LR
    Event3["/dev/input/event3 (gpio-keys)"] -->|0x0068| K_PREV["KEY_PAGEUP (Previous Page)"]
    Event3 -->|0x006d| K_NEXT["KEY_PAGEDOWN (Next Page)"]
    Event1["/dev/input/event1 (onyx-misc)"] -->|0x008f| K_HOME["KEY_HOMEPAGE / KEY_BACK"]
    Event0["/dev/input/event0 (qpnp_pon)"] -->|0x0074| K_PWR["KEY_POWER"]
    Event2["/dev/input/event2 (onyx-hall)"] -->|0x0038 / 0x008f| K_LID["Cover Close / Open (Sleep)"]
```

| Event Node | Driver Name | Linux Keycode | Action / TWRP Function |
| :--- | :--- | :--- | :--- |
| `/dev/input/event3` | `gpio-keys` | `0x0068` (104) | Page Up (Menu Up) |
| `/dev/input/event3` | `gpio-keys` | `0x006d` (109) | Page Down (Menu Down) |
| `/dev/input/event1` | `onyx-misc` | `0x008f` (143) | Home / Back Button |
| `/dev/input/event0` | `qpnp_pon` | `0x0074` (116) | Power Button (Select / Screen Off) |
| `/dev/input/event2` | `onyx-hall` | `0x0038`, `0x008f` | Magnetic Folio Cover Sensor |
| `/dev/input/event4` | `fts_ts` | Multi-touch | Capacitive Touch Digitizer |

---

## Partition & Mount Layout

The device uses an A/B dual-slot layout with a 4 GB dynamic `super` container:

| Mount Point | Block Device | Filesystem | Flags / Description |
| :--- | :--- | :--- | :--- |
| `/system` | `system` (in `super`) | ext4 | Logical dynamic partition (A/B) |
| `/system_ext`| `system_ext` (in `super`) | ext4 | Logical dynamic partition (A/B) |
| `/vendor` | `vendor` (in `super`) | ext4 | Logical dynamic partition (A/B) |
| `/product` | `product` (in `super`) | ext4 | Logical dynamic partition (A/B) |
| `/data` | `/dev/block/bootdevice/by-name/userdata` | f2fs | ~8.9 GB user storage (`encryptable=footer`) |
| `/metadata`| `/dev/block/bootdevice/by-name/metadata` | ext4 | Wrapped key metadata storage |
| `/sdcard` | `/dev/block/mmcblk1p1` | vfat | External MicroSD / TF card |
| `/boot` | `/dev/block/bootdevice/by-name/boot` | emmc | 96 MB kernel + base DTB |
| `/misc` | `/dev/block/bootdevice/by-name/misc` | emmc | 1 MB BCB commands |

---

## How to Build

### Option A: Automated GitHub Actions (Recommended)
1. Fork or push this repository to GitHub.
2. Navigate to **Actions > Build TWRP Recovery for Onyx Boox C69**.
3. Click **Run workflow**.
4. Download the compiled `twrp-c69-recovery` artifact containing `recovery.img`.

### Option B: Local Compilation

```bash
# 1. Initialize minimal TWRP-11 manifest
mkdir -p ~/twrp && cd ~/twrp
repo init -u https://github.com/minimal-manifest-twrp/platform_manifest_twrp_aosp.git -b twrp-11 --depth=1
repo sync -c -j$(nproc --all) --force-sync --no-clone-bundle --no-tags

# 2. Clone this device tree
git clone https://github.com/7sarus/android_device_onyx_c69.git device/onyx/c69

# 3. Build recovery image
source build/envsetup.sh
export ALLOW_MISSING_DEPENDENCIES=true
lunch twrp_c69-eng
mka recoveryimage -j$(nproc --all)
```

---

## Testing & Usage Instructions

### Tether-boot in RAM (Recommended & Zero Risk)
```bash
adb reboot bootloader
fastboot boot recovery.img
```
*Note: The recovery executes entirely in RAM without modifying your internal recovery or boot partitions.*

---

## Credits & Upstream Attribution
- **[@7sarus](https://github.com/7sarus)**: Creator of the original device tree, fastboot/EDL partition maps, and initial CI build pipeline ([7sarus/android_device_onyx_c69](https://github.com/7sarus/android_device_onyx_c69)). Sincere gratitude for establishing the foundational device tree and build manifest.
- **TeamWin (TWRP)**: Custom recovery framework and touch UI engine.
- **spavikevik**: Reverse-engineering notes on Qualcomm SDE EPDC DRM plane parameters ([palma2pro-eos](https://github.com/spavikevik/palma2pro-eos)).
