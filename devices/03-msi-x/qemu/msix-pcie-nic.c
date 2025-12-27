/*
 * minimal_pcie_nic.c
 *
 * Minimal skeleton PCIe NIC-like device for QEMU 9.2.0
 * with a PCIe MMIO BAR (BAR0) and read/write callbacks.
 *
 * Notes:
 * - BAR0 exposes a 4 KB MMIO region to the guest
 * - MMIO accesses are trapped and handled by callbacks
 * - memory_region_init_io(..., 0x1000) does NOT allocate 4 KB memory
 *   → It only creates a 0x1000 address window the guest can access
 * - The actual storage backing reads/writes is regs[] (64 bytes)
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "exec/memory.h" /* MemoryRegion */
#include "hw/irq.h"

#define TYPE_MINIMAL_PCIE_NIC "minimal-pcie-nic"
#define MSIX_NUM_VECTORS 4
#define MSIX_BAR_SIZE   0x1000
#define MSIX_ENABLE

OBJECT_DECLARE_SIMPLE_TYPE(MinimalPCIeNICState, MINIMAL_PCIE_NIC)

/* Device state structure */
typedef struct MinimalPCIeNICState {
    PCIDevice parent_obj;      /* Must be first */
    MemoryRegion mmio;         /* BAR0 Device Registers */
    MemoryRegion msix_bar;      /* BAR1: MSI-X table + PBA */
    uint32_t regs[16];         /* Simulated device registers (64 bytes) */
} MinimalPCIeNICState;

/* Generate MSI/MSI-X interrupt */
static void minimal_raise_irq(MinimalPCIeNICState *s, uint32_t vector)
{
    PCIDevice *pdev = &s->parent_obj;

#ifdef MSIX_ENABLE
    printf("MSI-X vector %u\n", vector);
    if (msix_enabled(pdev)) {
        if (vector < msix_nr_vectors_allocated(pdev)) {
            printf("msix notify MSI-X vector %u\n", vector);
            msix_notify(pdev, vector);
        } else {
            printf("invalid MSI-X vector %u\n", vector);
        }
        return;
    }
#endif

    /* Fallback to MSI */
    if (msi_enabled(pdev)) {
        msi_notify(pdev, vector);
        return;
    }

    printf("interrupts not enabled\n");
}

/* MMIO read callback */
static uint64_t minimal_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    MinimalPCIeNICState *s = opaque;
    uint64_t val = 0;

    /* Bounds check: guest may read beyond regs[] */
    if (addr + size > sizeof(s->regs)) {
        printf("minimal_pcie_nic: MMIO read out-of-bounds addr=0x%#" PRIx64 " size=%u\n",
               (uint64_t)addr, size);
        return 0;
    }

    /* Read 1/2/4/8 bytes, little-endian */
    switch (size) {
    case 1:
        val = *((uint8_t *)s->regs + addr);
        break;
    case 2:
        val = le16_to_cpu(*(uint16_t *)((uint8_t *)s->regs + addr));
        break;
    case 4:
        val = le32_to_cpu(*(uint32_t *)((uint8_t *)s->regs + addr));
        break;
    case 8:
        val = le64_to_cpu(*(uint64_t *)((uint8_t *)s->regs + addr));
        break;
    default:
        printf("minimal_pcie_nic: MMIO read unsupported size %u\n", size);
        return 0;
    }

    printf("minimal_pcie_nic: MMIO read addr=0x%#" PRIx64 " size=%u val=0x%llx\n",
           (uint64_t)addr, size, (unsigned long long)val);

    return val;
}

/* MMIO write callback */
static void minimal_mmio_write(void *opaque,
                               hwaddr addr,
                               uint64_t data,
                               unsigned size)
{
    MinimalPCIeNICState *s = opaque;

    if (addr + size > sizeof(s->regs)) {
        printf("minimal_pcie_nic: MMIO write out-of-bounds addr=0x%#" PRIx64
               " size=%u\n", (uint64_t)addr, size);
        return;
    }

    printf("minimal_pcie_nic: MMIO write addr=0x%#" PRIx64
           " size=%u data=0x%llx\n",
           (uint64_t)addr, size, (unsigned long long)data);


    /* This is only for msi/msi-x testing */
    if (addr == 0x0 && size == 4) {
        uint32_t vector = data & 0xff;

        printf("minimal_pcie_nic: trigger IRQ vector=%u\n", vector);
        minimal_raise_irq(s, vector);
        return;
    }

    /* Normal register write */
    switch (size) {
    case 1:
        *((uint8_t *)s->regs + addr) = data & 0xff;
        break;
    case 2:
        *(uint16_t *)((uint8_t *)s->regs + addr) =
            cpu_to_le16(data & 0xffff);
        break;
    case 4:
        *(uint32_t *)((uint8_t *)s->regs + addr) =
            cpu_to_le32(data & 0xffffffff);
        break;
    case 8:
        *(uint64_t *)((uint8_t *)s->regs + addr) =
            cpu_to_le64(data);
        break;
    default:
        printf("minimal_pcie_nic: MMIO write unsupported size %u\n", size);
        break;
    }
}


/* MemoryRegionOps for the MMIO region */
static const MemoryRegionOps minimal_mmio_ops = {
    .read = minimal_mmio_read,
    .write = minimal_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* Realize function: called when device is instantiated */
static void minimal_pcie_nic_realize(PCIDevice *pdev, Error **errp)
{
    MinimalPCIeNICState *s = MINIMAL_PCIE_NIC(pdev);

    printf("minimal_pcie_nic: realize called (host log)\n");

    /* PCI config space: set vendor/device IDs and class */
    pci_config_set_vendor_id(pdev->config, 0x1af4);
    pci_config_set_device_id(pdev->config, 0x10f1);

    /* Class: Ethernet controller */
    pci_config_set_class(pdev->config, PCI_CLASS_NETWORK_ETHERNET);

    /* Revision ID */
    pci_config_set_revision(pdev->config, 0x01);

    /* Initialize internal "registers" to zero */
    memset(s->regs, 0, sizeof(s->regs));

    /* Command register: enable memory accesses and bus mastering */
    uint16_t cmd = PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    pci_set_word(pdev->config + PCI_COMMAND, cmd);

    /* Create BAR0 MMIO region
     * - Size = 4 KB
     * - This does NOT allocate 4 KB memory
     * - Only traps guest accesses to read/write callbacks
     */
    memory_region_init_io(&s->mmio, OBJECT(s), &minimal_mmio_ops, s,
                          "minimal-pcie-mmio", 0x1000);

    /* Register BAR0 with PCI core
     * Guest OS will map this BAR, reads/writes hit callbacks
     */
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    /* BAR1: MSI-X table */
    memory_region_init(&s->msix_bar, OBJECT(s), "minimal-msix-bar", MSIX_BAR_SIZE);

    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->msix_bar);

#ifdef MSIX_ENABLE
    /* MSI-X capability */
    msix_init(pdev, MSIX_NUM_VECTORS, &s->msix_bar,
              1,      /* BAR index */
              0x0,    /* table offset */
              &s->msix_bar,
              1,      /* PBA BAR index */
              0x800); /* PBA offset */
#else
    /* Enable MSI (number of vectors) with per-vector masking */
    if (msi_init(pdev,
                 0,      /* offset in config space */
                 4,      /* number of MSI vectors */
                 false,  /* 32-bit address */
                 true,   /* per-vector masking enabled */
                 errp) < 0) {
        return;
    }
#endif


}

static void minimal_pcie_nic_uninit(PCIDevice *pdev)
{
#ifdef MSIX_ENABLE
    msix_uninit(pdev);
#else
    msi_uninit(pdev);
#endif
    printf("pcie nic un-init\n");
}

/* Class initialization */
static void minimal_pcie_nic_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize  = minimal_pcie_nic_realize;
    k->exit = minimal_pcie_nic_uninit;
    k->vendor_id = 0x1af4;
    k->device_id = 0x10f1;

    /* Class: Ethernet controller */
    k->class_id  = PCI_CLASS_NETWORK_ETHERNET;
    k->revision = 0x1; //set initial revision
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->desc = "Minimal PCIe NIC Card";
}

/* Type registration */
static void minimal_pcie_nic_register_types(void)
{
    static const InterfaceInfo interfaces[] = {
        { INTERFACE_PCIE_DEVICE },
        { }
    };

    static const TypeInfo minimal_pcie_nic_info = {
        .name          = TYPE_MINIMAL_PCIE_NIC,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(MinimalPCIeNICState),
        .class_init    = minimal_pcie_nic_class_init,
        .interfaces    = interfaces,
    };

    type_register_static(&minimal_pcie_nic_info);
}

/* Initialize the type at QEMU startup */
type_init(minimal_pcie_nic_register_types);