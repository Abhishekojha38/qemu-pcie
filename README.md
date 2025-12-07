# 🚀 QEMU PCIe Device Development Repository

## 📚 Table of Contents

-   [📁 Repository Structure](#-repository-structure)
-   [🧰 Features Covered](#-features-covered)
-   [⚙️ What Happens When You Run
    QEMU](#️-what-happens-when-you-run-qemu)
-   [🛠️ Build Steps (Using Yocto
    Playground)](#️-build-steps-using-yocto-playground)
    -   [1️⃣ Build default Yocto image](#1️⃣-build-default-yocto-image)
    -   [2️⃣ Modify QEMU to add PCIe
        device](#2️⃣-modify-qemu-to-add-pcie-device)
    -   [3️⃣ Add Kconfig entry](#3️⃣-add-kconfig-entry)
    -   [4️⃣ Add Meson build entry](#4️⃣-add-meson-build-entry)
    -   [5️⃣ Commit changes](#5️⃣-commit-changes)
    -   [6️⃣ Finish devtool
        modifications](#6️⃣-finish-devtool-modifications)
    -   [7️⃣ Build final image](#7️⃣-build-final-image)
    -   [▶️ Launch QEMU](#️-launch-qemu)
-   [🔰 01-basic Demo](#-01-basic-demo)
    -   [🔍 lspci Output](#-lspci-output)
    -   [📝 dmesg Output](#-dmesg-output)
-   [🔰 02-mmio Demo](#-02-mmio-demo)
    -   [🔍 lspci Output](#-lspci-output)
    -   [📝 dmesg Output](#-dmesg-output)
    -   [🔢 Memory Read/Write](#-Memory-Read/Write)
    -   [📦 MMIO Region Layout](#-MMIO-Region-Layout)
    -   [🔄 Read–Write Flow](#-Read–Write-Flow)

This repository provides a complete learning path for creating **basic to advanced PCIe devices in QEMU**, along with corresponding **Linux drivers**.  
It is structured so you can explore progressively—from simplest PCI BAR examples to full-featured MSI/MSI-X, DMA engines, and custom capabilities.

## 📁 Repository Structure

    devices/
    ├── 01-basic/
    │   ├── qemu/
    │   │   └── minimal_pcie_nic.c
    ├── 02-mmio/
    │   ├── qemu/
    │   │   └── mmio_pcie_nic.c
    └── README.md

## 🧰 Features Covered

✔️ Basic PCIe Device Creation\
✔️ Visible Entry in `lspci`\
✔️ BARs & MMIO\
✔️ PCIe Configuration Space\
✔️ MSI / MSI-X Interrupts\
✔️ Emulated DMA Engine

## ⚙️ What Happens When You Run QEMU

1.  QEMU starts → registers PCIe device\
2.  PCI bus finds `-device minimal-pcie-nic`\
3.  Device realize sets IDs\
4.  Linux sees device as **1234:11E8**

# 🛠️ Build Steps (Using Yocto Playground)

## 1️⃣ Build default Yocto image

Refer to the yocto-playground README.
https://github.com/Abhishekojha38/yocto-playground/blob/main/README.md

## 2️⃣ Modify QEMU to add PCIe device

``` bash
devtool modify qemu-system-native
cd ~/yocto-playground/build/workspace/sources/qemu-system-native
cp ~/qemu-pcie/device/01-basic/qemu/minimal_pcie_nic.c hw/pci/minimal_pcie_nic.c
```

## 3️⃣ Add Kconfig entry

```bash
    config MINIMAL_PCIE_NIC
        bool
        default y if TEST_DEVICES
        depends on PCI
```

## 4️⃣ Add Meson build entry

```bash
    system_ss.add(when: 'CONFIG_MINIMAL_PCIE_NIC', if_true: files('minimal_pcie_nic.c'))
```

## 5️⃣ Commit changes

``` bash
git add hw/pci/meson.build hw/pci/kconfig hw/pci/minimal_pcie_nic.c
git commit -m "Add minimal pcie nic card"
```

## 6️⃣ Finish devtool modifications

``` bash
devtool finish qemu-system-native ../sources/meta-playground/meta-playground-os/
```

## 7️⃣ Build final image

``` bash
cqfd run
```

## ▶️ Launch QEMU

``` bash
runqemu playground-arm64 nographic slirp qemuparams="-device minimal-pcie-nic"
```

# 🔰 01-basic Demo

## 🔍 lspci Output

``` bash
00:05.0 Ethernet controller: Device 1234:11e8
        Subsystem: Red Hat, Inc. Device 1100
        Flags: fast devsel

00:05.0 Ethernet controller: Device 1234:11e8
00: 34 12 e8 11 00 00 00 00 00 00 00 02 00 00 00 00
10: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
20: 00 00 00 00 00 00 00 00 00 00 00 00 f4 1a 00 11
30: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

## 📝 dmesg Output

``` bash
pci 0000:00:05.0: [1234:11e8] type 00 class 0x020000
```

# 🔰 02-mmio Demo

A PCIe device announces its MMIO requirements in BAR0–BAR5.
Meaning:
* BAR0 Size: `4 KB`
* Host Physical Address: `0x10043000`
* Type: MMIO, non-prefetchable (normal for NIC registers)

## 🔍 lspci Output

``` bash
00:05.0 Ethernet controller: Red Hat, Inc. Device 10f1
        Subsystem: Red Hat, Inc. Device 1100
        Control: I/O- Mem+ BusMaster- SpecCycle- MemWINV- VGASnoop- ParErr- Stepping- SERR- FastB2B- DisINTx-
        Status: Cap- 66MHz- UDF- FastB2B- ParErr- DEVSEL=fast >TAbort- <TAbort- <MAbort- >SERR- <PERR- INTx-
        Region 0: Memory at 10043000 (32-bit, non-prefetchable) [size=4K]

00:05.0 Ethernet controller: Red Hat, Inc. Device 10f1
00: f4 1a f1 10 02 00 00 00 00 00 00 02 00 00 00 00
10: 00 30 04 10 00 00 00 00 00 00 00 00 00 00 00 00
20: 00 00 00 00 00 00 00 00 00 00 00 00 f4 1a 00 11
30: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

## 📝 dmesg Output

``` bash
pci 0000:00:05.0: BAR 0 [mem 0x10043000-0x10043fff]: assigned    
pci 0000:00:05.0: [1af4:10f1] type 00 class 0x020000 conventional PCI endpoint
```

## 🔢 Memory Read/Write

* First read `0x1004300` location

```bash
root@playground-arm64:~# devmem2 0x10043000
/dev/mem opened.
Memory mapped at address 0xffffb861c000.
minimal_pcie_nic: MMIO read addr=0x0 size=4 val=0x0
Read at address  0x10043000 (0xffffb861c000): 0x00000000
```

* Now Write some value to it `deafbeaf`

```bash
root@playground-arm64:~# devmem2 0x10043000 w 0xdeafbeaf
/dev/mem opened.
Memory mapped at address 0xffffb04c0000.
minimal_pcie_nic: MMIO read addr=0x0 size=4 val=0x0
Read at address  0x10043000 (0xffffb04c0000): 0x00000000
minimal_pcie_nic: MMIO write addr=0x0 size=4 data=0xdeafbeaf
Write at address 0x10043000 (0xffffb04c0000): 0xDEAFBEAF, readback 0xDEAFBEAF
```

* Readback the same location `0x10043000`

```bash
root@playground-arm64:~# devmem2 0x10043000
/dev/mem opened.
Memory mapped at address 0xffffa9773000.
minimal_pcie_nic: MMIO read addr=0x0 size=4 val=0xdeafbeaf
Read at address  0x10043000 (0xffffa9773000): 0xDEAFBEAF
root@playground-arm64:~#
```


> All the minimal_pcie_nic: prints are from the read write callbacks. Check source
> code for more details. 

## 📦 MMIO Region Layout

You can describe your NIC’s register map like this:
```log
Offset   Size   Description
------------------------------------
0x0000   4      Control Register
0x0004   4      Status Register
0x0008   4      TX Buffer Pointer
0x000C   4      RX Buffer Pointer
0x0010   4      Interrupt Status
0x0014   4      Interrupt Mask
0x1000   ...    (end of 4 KB region)
```

## 🔄 Read–Write Flow

```diagram
+-----------------------------------------------------------+
|                      PCIe Device                          |
|                 (Your Minimal NIC Model)                  |
+-----------------------------------------------------------+
                |                     |
                | Exposes BAR0        |
                | Size = 4 KB         |
                v                     |
+-----------------------------------------------------------+
|                 PCI Configuration Space                    |
|  - Vendor ID 0x1234                                         |
|  - Device ID 0x11E8                                         |
|  - BAR0 = 0x10043000 (assigned by OS)                      |
|  - Command Register (MemEnable, BusMasterEnable)            |
+-----------------------------------------------------------+
                |
                | BAR0 maps NIC registers into system memory
                v
+-----------------------------------------------------------+
|              System Physical Address Space                |
+-----------------------------------------------------------+
|   0x10043000  →  +--------------------------------------+ |
|                  |         MMIO Region (4 KB)           | |
|                  |--------------------------------------| |
|   0x10043000  →  | 0x0000  Control Register             | |
|   0x10043004  →  | 0x0004  Status Register              | |
|   0x10043008  →  | 0x0008  TX Buffer Pointer            | |
|   0x1004300C  →  | 0x000C  RX Buffer Pointer            | |
|   0x10043010  →  | 0x0010  IRQ Status                   | |
|   0x10043014  →  | 0x0014  IRQ Mask                     | |
|                  |   ... (rest of 4 KB region)          | |
|   0x10043FFF  →  +--------------------------------------+ |
+-----------------------------------------------------------+
                |
                | Linux maps BAR0 into kernel virtual space
                v
+-----------------------------------------------------------+
|               Kernel Virtual Address (ioremap)            |
|       mmio_base = 0xffff9243c0000000 (example)            |
+-----------------------------------------------------------+
                |
                | Register access through ioread32/iowrite32
                v
+-----------------------------------------------------------+
|              Example Register Access Flow                |
+-----------------------------------------------------------+
| CPU → iowrite32(0x1, mmio_base + 0x0000)                  |
|  → QEMU MMIO write callback                               |
|  → mynic_mmio_write() updates internal NIC state           |
|                                                           |
| CPU ← ioread32(mmio_base + 0x0004)                        |
|  ← QEMU MMIO read callback                                |
|  ← mynic_mmio_read() returns register value                |
+-----------------------------------------------------------+

```

## 🧑‍💻 Author
**Abhishek Ojha**
Abhishekojha38@gmail.com