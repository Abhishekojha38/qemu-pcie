/*
 * minimal_pcie_nic.c
 *
 * Minimal skeleton PCIe NIC-like device for QEMU 9.2.0
 * with a PCIe MMIO BAR (BAR0) and read/write callbacks.
 *
 * ─── PCIe NIC in QEMU — Conceptual Overview ───────────────────────────────
 *
 * A PCIe NIC in QEMU is composed of three main pillars:
 *
 *  1. PCI CONFIG SPACE
 *     Every PCI device has a 256-byte (PCIe: 4 KB) configuration space visible
 *     to the host OS.  It contains Vendor/Device IDs, class codes, BAR base
 *     addresses, capability pointers (MSI, MSI-X, PCIe), and command/status
 *     registers.  The guest OS reads this during enumeration (lspci) and uses
 *     it to identify and configure the device.
 *
 *  2. BARs (Base Address Registers) — MMIO windows
 *     BARs are regions of memory (or I/O port space) that the device exposes
 *     to the guest.  The guest OS programs BAR base addresses during PCI
 *     enumeration.  When the guest reads/writes into a BAR window, QEMU
 *     intercepts the access and calls the registered MemoryRegionOps
 *     (.read/.write callbacks).  No real RAM is allocated — the BAR is a
 *     virtual trap window backed by our callback logic.
 *
 *     BAR0 (this device): 4 KB MMIO — device registers (RX ring config, etc.)
 *     BAR1 (this device): 4 KB      — MSI-X table + PBA (Pending Bit Array)
 *
 *  3. INTERRUPTS — MSI / MSI-X
 *     Legacy PCI uses a shared INTx wire.  PCIe devices use MSI (Message
 *     Signalled Interrupts) or MSI-X.  Both work by writing a magic value to a
 *     magic address in host memory — no wire needed.  MSI-X extends MSI by
 *     storing per-vector address/data pairs in a table inside BAR1, allowing
 *     up to 2048 independent interrupt vectors.  QEMU's msix_notify() emulates
 *     this by injecting the interrupt directly into the guest.
 *
 * ─── RX Data Path (packet guest-ward) ────────────────────────────────────
 *
 *  Host network (tap/user) → minimal_receive_packet()
 *    → read RX descriptor from guest memory via pci_dma_read()
 *    → DMA packet bytes into guest buffer via pci_dma_write()
 *    → mark descriptor RX_DONE, advance ring head
 *    → raise MSI-X vector 0 to notify the guest driver
 *
 * ─── Key QEMU Concepts Used ───────────────────────────────────────────────
 *
 *  memory_region_init_io()  — creates a virtual MMIO trap window (no RAM)
 *  pci_register_bar()       — binds a MemoryRegion to a BAR slot
 *  msix_init()              — allocates MSI-X capability in config space
 *  msix_notify()            — injects an MSI-X interrupt into the guest
 *  pci_dma_read/write()     — DMA between device and guest RAM (IOMMU-aware)
 *  qemu_new_nic()           — connects device to QEMU's net backend (tap, etc.)
 *
 * Notes:
 * - BAR0 exposes a 4 KB MMIO region to the guest
 * - MMIO accesses are trapped and handled by callbacks
 * - memory_region_init_io(..., 0x1000) does NOT allocate 4 KB memory
 *   → It only creates a 0x1000 address window the guest can access
 * - The actual storage backing reads/writes is regs[] (64 bytes)
 */

/*
 * Standard QEMU headers required for any device model:
 *   osdep.h       — portability macros (must be first)
 *   pci.h         — PCI core API (config space helpers, DMA)
 *   pci_device.h  — PCIDevice type and PCIDeviceClass
 *   qdev-properties.h — DEFINE_NIC_PROPERTIES macro
 *   module.h      — type_init() macro
 *   error.h       — Error ** propagation
 *   memory.h      — MemoryRegion, memory_region_init_io()
 *   irq.h         — interrupt helpers
 *   net/net.h     — NICState, NICConf, qemu_new_nic()
 */
#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "exec/memory.h" /* MemoryRegion */
#include "hw/irq.h"
#include "net/net.h"

/*
 * Device identity and capability constants.
 *
 * TYPE_MINIMAL_PCIE_NIC — the string name used with -device on the QEMU
 *   command line: -device minimal-pcie-nic,netdev=...
 *
 * MSI_NUM_VECTORS  — how many MSI interrupt vectors to request.  MSI
 *   allocates a contiguous block of vectors in the guest's interrupt table.
 *
 * MSIX_NUM_VECTORS — number of MSI-X vectors.  Each has its own entry in the
 *   MSI-X table (stored in BAR1) with an independent address/data pair.
 *
 * MSIX_BAR_SIZE    — size of BAR1.  Must be large enough for:
 *   - MSI-X table: MSIX_NUM_VECTORS × 16 bytes  (at offset 0x000)
 *   - PBA:         ceil(MSIX_NUM_VECTORS/64) × 8 bytes  (at offset 0x800)
 *
 * MSIX_ENABLE      — compile-time switch: defined → use MSI-X,
 *   undefined → fall back to MSI.
 *
 * BAR0_IDX / BAR1_MSIX_IDX — which BAR slot each region occupies.
 *   A PCIe device may have up to 6 BARs (indices 0–5).
 */
#define TYPE_MINIMAL_PCIE_NIC "minimal-pcie-nic"    // qemu device name
#define MSI_NUM_VECTORS         4                   // msi max vectors
#define MSIX_NUM_VECTORS        4                   // msi-x max vectors
#define MSIX_BAR_SIZE           0x1000              // 4KB MSIX Bar size
#define MSIX_ENABLE                                 // Select MSI or MSI-X
#define BAR1_MSIX_IDX           1                   // Use BAR 1 for MSI-X
#define BAR0_IDX                0                   // Use BAR 0 for MMIO

/*
 * OBJECT_DECLARE_SIMPLE_TYPE — QEMU QOM (Qt Object Model) macro.
 *
 * Expands to:
 *   typedef struct MinimalPCIeNICState MinimalPCIeNICState;
 *   G_DEFINE_AUTOPTR_CLEANUP_FUNC(MinimalPCIeNICState, object_unref)
 *   #define MINIMAL_PCIE_NIC(obj) \
 *       OBJECT_CHECK(MinimalPCIeNICState, obj, TYPE_MINIMAL_PCIE_NIC)
 *
 * The MINIMAL_PCIE_NIC(obj) cast macro is used everywhere we have a generic
 * PCIDevice * or Object * and need to get our private state pointer.
 */
OBJECT_DECLARE_SIMPLE_TYPE(MinimalPCIeNICState, MINIMAL_PCIE_NIC)

/*
 * MinimalPCIeNICState — per-instance device state.
 *
 * QEMU allocates one of these structs for each instantiated device.
 * All fields must be initialized in minimal_pcie_nic_realize().
 *
 * parent_obj  — the embedded PCIDevice.  MUST be the very first field so that
 *               QEMU's object system can safely cast between PCIDevice * and
 *               MinimalPCIeNICState *.
 *
 * mmio        — MemoryRegion representing BAR0.
 *               memory_region_init_io() registers read/write callbacks on it.
 *               No host RAM is allocated; guest accesses are trapped.
 *
 * msix_bar    — MemoryRegion for BAR1.  msix_init() subdivides it into the
 *               MSI-X vector table (offset 0) and the PBA (offset 0x800).
 *
 * regs[]      — 64 bytes of simulated device register storage.
 *               Backing store for BAR0 MMIO reads/writes (except special-
 *               cased registers like rx_ring_base that live in dedicated
 *               fields below).
 *
 * nic / conf  — QEMU NIC state and configuration (MAC address, netdev link).
 *               qemu_new_nic() attaches us to the host networking backend.
 *
 * nc          — The active NetClientState; used to send/receive packets.
 *
 * rx_ring_base / rx_ring_size — Guest-physical address and entry count of the
 *               RX descriptor ring.  Written by the guest driver via BAR0.
 *
 * rx_head / rx_tail — Producer/consumer indices into the RX ring.
 *               rx_head: device-side index (next descriptor to fill).
 *               rx_tail: driver-side index (next descriptor to reclaim).
 */
/* Device state structure */
typedef struct MinimalPCIeNICState {
    PCIDevice parent_obj;      /* Must be first */
    MemoryRegion mmio;         /* BAR0 Device Registers */
    MemoryRegion msix_bar;      /* BAR1: MSI-X table + PBA */
    /*
     * regs[] is the backing store for BAR0 MMIO accesses that are not
     * handled by dedicated fields (rx_ring_base, tx_ring_base, etc.).
     * Expanded to 32 × 4 = 128 bytes to cover all register offsets
     * including the TX registers that reach offset 0x40.
     */
    uint32_t regs[32];         /* Simulated device registers (128 bytes) */

    NICState *nic;
    NICConf conf;

    /* RX ring state */
    uint64_t rx_ring_base;     /* guest-physical base of RX descriptor ring */
    uint32_t rx_ring_size;     /* number of RX descriptors */
    uint32_t rx_head;          /* device producer index (next desc to fill) */
    uint32_t rx_tail;          /* driver consumer index (reclaimed up to here) */

    /* TX ring state */
    uint64_t tx_ring_base;     /* guest-physical base of TX descriptor ring */
    uint32_t tx_ring_size;     /* number of TX descriptors */
    uint32_t tx_head;          /* device consumer index (next desc to process) */
    uint32_t tx_tail;          /* driver producer index (last desc written) */
} MinimalPCIeNICState;

/*
 * BAR0 register offsets (byte addresses within the 4 KB MMIO window).
 *
 * The guest driver writes these addresses to configure the RX descriptor ring
 * before it is ready to receive packets.
 *
 * REG_RX_RING_BASE — 64-bit guest-physical address of the first RX descriptor.
 * REG_RX_RING_SIZE — number of descriptors in the ring (power-of-2 typical).
 * REG_RX_TAIL      — written by the driver to return processed descriptors.
 * REG_RX_HEAD      — read by the driver to discover how far the device has
 *                    consumed into the ring (device advances this on RX).
 */
#define REG_RX_RING_BASE   0x10
#define REG_RX_RING_SIZE   0x18
#define REG_RX_TAIL        0x1C
#define REG_RX_HEAD        0x20

/*
 * rx_desc — RX descriptor layout shared between device and guest driver.
 *
 * The guest driver allocates an array of these in DMA-coherent memory and
 * programs REG_RX_RING_BASE to point to it.
 *
 * addr  — guest-physical address of the pre-allocated packet buffer.
 *         The device writes received packet bytes here via pci_dma_write().
 * len   — filled by the device with the number of bytes written.
 * flags — status bits; RX_DONE (bit 0) set by the device when the descriptor
 *         is complete and the buffer contains a valid packet.
 */
struct rx_desc {
    uint64_t addr;
    uint16_t len;
    uint16_t flags;
};

/* Descriptor flag: device sets this bit after DMA-ing a packet into addr. */
#define RX_DONE 1

/*
 * tx_desc — TX descriptor layout shared between guest driver and device.
 *
 * The guest driver fills one entry per outbound packet and kicks the device
 * by writing the updated tail index to REG_TX_TAIL.
 *
 * addr  — guest-physical address of the packet buffer filled by the driver.
 * len   — number of bytes in the packet.
 * flags — TX_READY set by driver (packet ready to send);
 *         TX_DONE  set by device after the packet has been queued to the wire.
 */
struct tx_desc {
    uint64_t addr;
    uint16_t len;
    uint16_t flags;
};

#define TX_READY  1   /* driver sets: descriptor holds a packet to transmit */
#define TX_DONE   2   /* device sets: packet has been sent to the wire */

/* BAR0 TX register offsets */
#define REG_TX_RING_BASE   0x30   /* 64-bit guest-physical address of TX ring */
#define REG_TX_RING_SIZE   0x38   /* number of TX descriptors */
#define REG_TX_TAIL        0x3C   /* driver writes new tail to kick device */
#define REG_TX_HEAD        0x40   /* driver reads to discover sent descs */

/* Forward declaration — defined after minimal_raise_irq() below. */
/* Callback; packet received from host (eg. tap or user networking) */
static ssize_t minimal_receive_packet(NetClientState *nc,
                                      const uint8_t *buf,
                                      size_t size);

/*
 * minimal_tx_kick — process outbound TX descriptors queued by the driver.
 *
 * Called when the guest driver writes REG_TX_TAIL to signal that new TX
 * descriptors have been filled.  We drain all pending descriptors from
 * tx_head up to (but not including) the new tx_tail.
 *
 * For each ready descriptor:
 *  1. pci_dma_read() reads the descriptor struct from guest memory.
 *  2. pci_dma_read() reads the packet bytes from the guest buffer.
 *  3. qemu_send_packet() delivers the frame to the host network backend.
 *  4. The descriptor is marked TX_DONE and written back.
 *  5. tx_head advances.
 *
 * A 2 KB stack buffer is sufficient for standard Ethernet frames (max 1522 B).
 */
static void minimal_tx_kick(MinimalPCIeNICState *s)
{
    uint8_t pktbuf[2048];

    if (!s->tx_ring_base || !s->tx_ring_size)
        return;

    while (s->tx_head != s->tx_tail) {
        struct tx_desc desc;
        uint64_t desc_addr = s->tx_ring_base + s->tx_head * sizeof(desc);

        // Get the TX descriptor from the guest memory
        pci_dma_read(&s->parent_obj, desc_addr, &desc, sizeof(desc));

        if (!(desc.flags & TX_READY))
            break;  /* descriptor not filled yet — stop */

        // Get the packet from the guest memory
        uint16_t len = desc.len < sizeof(pktbuf) ? desc.len : sizeof(pktbuf);
        pci_dma_read(&s->parent_obj, desc.addr, pktbuf, len);

        // Send the packet to the host network backend
        qemu_send_packet(qemu_get_queue(s->nic), pktbuf, len);

        // Mark the descriptor as done
        desc.flags = TX_DONE;
        pci_dma_write(&s->parent_obj, desc_addr, &desc, sizeof(desc));

        s->tx_head = (s->tx_head + 1) % s->tx_ring_size;
    }
}

/*
 * minimal_can_receive — tell QEMU whether this device can accept an inbound
 * packet right now.
 *
 * Returns non-zero (true) only after the guest driver has programmed the RX
 * ring, preventing QEMU from dropping frames on the floor before the driver
 * is ready to receive.
 */
static int minimal_can_receive(NetClientState *nc)
{
    MinimalPCIeNICState *s = qemu_get_nic_opaque(nc);
    int ready = s->rx_ring_size > 0 && s->rx_ring_base != 0;
    if (!ready) {
        printf("minimal_pcie_nic: [TRACE] can_receive=NO "
               "(rx_ring_base=0x%" PRIx64 " rx_ring_size=%u)\n",
               s->rx_ring_base, s->rx_ring_size);
    }
    return ready;
}

/*
 * minimal_raise_irq — deliver an interrupt to the guest.
 *
 * PCIe devices signal events (packet received, TX complete, error, …) via
 * MSI or MSI-X rather than legacy INTx wires.
 *
 * MSI-X path (MSIX_ENABLE defined):
 *   msix_enabled() checks that the guest driver has written the MSI-X Enable
 *   bit in the Message Control register of the MSI-X capability.
 *   msix_nr_vectors_allocated() guards against an out-of-range vector index.
 *   msix_notify() writes the interrupt message on behalf of the device,
 *   causing the guest to enter the ISR registered for that vector.
 *
 * MSI fallback (MSIX_ENABLE not defined):
 *   msi_notify() performs the equivalent for the simpler MSI capability.
 *
 * @s      — device state
 * @vector — MSI-X / MSI vector index to fire (0 … MSIX_NUM_VECTORS-1)
 */
static void minimal_raise_irq(MinimalPCIeNICState *s, uint32_t vector)
{
    PCIDevice *pdev = &s->parent_obj;

#ifdef MSIX_ENABLE
    if (msix_enabled(pdev)) {
        if (vector < msix_nr_vectors_allocated(pdev)) {
            printf("minimal_pcie_nic: [TRACE] MSI-X notify vector=%u\n", vector);
            msix_notify(pdev, vector);
        } else {
            printf("minimal_pcie_nic: [TRACE] ERROR invalid MSI-X vector %u "
                   "(allocated=%u)\n", vector, msix_nr_vectors_allocated(pdev));
        }
        return;
    }
    printf("minimal_pcie_nic: [TRACE] ERROR MSI-X not enabled by guest\n");
#else
    /* Fallback to MSI */
    if (msi_enabled(pdev)) {
        printf("minimal_pcie_nic: [TRACE] MSI notify vector=%u\n", vector);
        msi_notify(pdev, vector);
        return;
    }
    printf("minimal_pcie_nic: [TRACE] ERROR MSI not enabled by guest\n");
#endif

    printf("interrupts not enabled\n");
}

/*
 * minimal_init_msix — add MSI-X capability to the PCI config space.
 *
 * msix_init() does several things under the hood:
 *  1. Appends an MSI-X capability structure (0x0C bytes) to the PCI
 *     capability linked list in config space.  The capability advertises
 *     MSIX_NUM_VECTORS vectors and points to the table/PBA BARs.
 *  2. Maps the MSI-X vector table (MSIX_NUM_VECTORS × 16 B each) into the
 *     provided MemoryRegion at the given table_offset (here: 0).
 *  3. Maps the Pending Bit Array (PBA) into the same or a different BAR at
 *     pba_offset (here: 0x800).
 *
 * Arguments to msix_init():
 *   dev            — PCIDevice to add the capability to
 *   nentries       — number of MSI-X vectors (MSIX_NUM_VECTORS = 4)
 *   table_bar      — MemoryRegion that will hold the vector table
 *   table_bar_nr   — BAR index of table_bar (BAR1_MSIX_IDX = 1)
 *   table_offset   — byte offset within table_bar where the table starts (0)
 *   pba_bar        — MemoryRegion that will hold the PBA (same as table_bar)
 *   pba_bar_nr     — BAR index of pba_bar (BAR1_MSIX_IDX = 1)
 *   pba_offset     — byte offset within pba_bar for the PBA (0x800)
 *   cap_pos        — desired offset in config space for the capability (0x98),
 *                    or 0 to let QEMU choose automatically
 *   errp           — error propagation (NULL here — we handle res ourselves)
 *
 * After a successful msix_init() we mark every vector "in use" so the guest
 * driver can unmask them individually without any additional device logic.
 */
static void
minimal_init_msix(MinimalPCIeNICState *s)
{
    int i, res;

    res = msix_init(PCI_DEVICE(s), MSIX_NUM_VECTORS,
                    &s->msix_bar,
                    BAR1_MSIX_IDX,       /* BAR index */
                    0,                  /* table offset */
                    &s->msix_bar,
                    BAR1_MSIX_IDX,       /* PBA BAR index */
                    0x800,
                    0x98, NULL);

    if (res < 0) {
        return ;
    } else {
        /*
         * Mark all vectors as "used" so the guest driver can enable them
         * by writing the Masked bit in the vector table.  Without this call
         * QEMU would refuse to deliver notifications for those vectors.
         */
        for (i = 0; i < MSIX_NUM_VECTORS; i++) {
            msix_vector_use(PCI_DEVICE(s), i);
        }
    }
}


/*
 * minimal_mmio_read — BAR0 MMIO read handler.
 *
 * QEMU calls this whenever the guest CPU executes a load instruction that
 * falls within the BAR0 address window (as mapped by the guest OS).
 *
 * @opaque — pointer to MinimalPCIeNICState (set in memory_region_init_io)
 * @addr   — byte offset within BAR0 (0 … 0xFFF)
 * @size   — access width in bytes: 1, 2, 4, or 8
 *
 * Flow:
 *  1. Bounds-check: guest may attempt to read beyond regs[] (64 bytes).
 *     The BAR window is 4 KB but backing storage is only 64 bytes; return 0
 *     and log a warning for out-of-range accesses.
 *  2. Read the appropriate number of bytes from regs[] and apply
 *     little-endian byte-swap macros (le*_to_cpu) so the value is in
 *     host-native byte order before being returned to QEMU's CPU model.
 */
/* MMIO read callback */
static uint64_t minimal_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    MinimalPCIeNICState *s = opaque;
    uint64_t val = 0;

    /*
     * Special-case dynamic registers that live in dedicated fields rather
     * than in the flat regs[] array.  Return the current hardware-side value
     * directly so the driver always sees the up-to-date index.
     */
    if (addr == REG_RX_HEAD && size == 4)
        return s->rx_head;

    if (addr == REG_TX_HEAD && size == 4)
        return s->tx_head;

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

/*
 * minimal_mmio_write — BAR0 MMIO write handler.
 *
 * QEMU calls this whenever the guest CPU executes a store instruction that
 * falls within the BAR0 address window.
 *
 * @opaque — pointer to MinimalPCIeNICState
 * @addr   — byte offset within BAR0
 * @data   — value written by the guest (already zero-extended to 64 bits)
 * @size   — access width in bytes: 1, 2, 4, or 8
 *
 * Special-cased register offsets are handled first (early return):
 *
 *   REG_RX_RING_BASE — guest driver sets the DMA address of the RX ring.
 *   REG_RX_RING_SIZE — guest driver programs the number of RX descriptors.
 *   REG_RX_TAIL      — guest driver advances the tail to return processed
 *                      descriptors (not yet acted on in this skeleton).
 *
 *   offset 0x0, size 4 — test hook: writing a vector number here triggers
 *                        the corresponding MSI/MSI-X interrupt immediately,
 *                        useful for verifying interrupt delivery without
 *                        a real packet.
 *
 * For all other offsets the data is stored into regs[] with the appropriate
 * cpu_to_le* byte-swap to maintain little-endian layout in device registers.
 */
/* MMIO write callback */
static void minimal_mmio_write(void *opaque,
                               hwaddr addr,
                               uint64_t data,
                               unsigned size)
{
    MinimalPCIeNICState *s = opaque;

    if (addr == REG_RX_RING_BASE) {
        /* Guest driver sets the DMA base address of the RX descriptor ring. */
        s->rx_ring_base = data;
        printf("minimal_pcie_nic: [TRACE] RX ring base set: 0x%" PRIx64 "\n", data);
        return;
    }

    if (addr == REG_RX_RING_SIZE) {
        /* Guest driver sets how many descriptors are in the ring. */
        s->rx_ring_size = data;
        printf("minimal_pcie_nic: [TRACE] RX ring size set: %u\n", (uint32_t)data);
        /*
         * Ring is now ready. If QEMU paused reading from the tap backend
         * because can_receive() returned 0 earlier, flush the queue to
         * re-enable the tap read handler and deliver any buffered packets.
         */
        if (s->rx_ring_base && s->rx_ring_size) {
            printf("minimal_pcie_nic: [TRACE] RX ring ready — flushing queued packets\n");
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;
    }

    if (addr == REG_RX_TAIL) {
        /* Guest driver reclaims processed descriptors up to this index. */
        s->rx_tail = data;
        return;
    }

    if (addr == REG_TX_RING_BASE) {
        /* Guest driver sets the DMA base address of the TX descriptor ring. */
        s->tx_ring_base = data;
        return;
    }

    if (addr == REG_TX_RING_SIZE) {
        /* Guest driver sets how many TX descriptors are in the ring. */
        s->tx_ring_size = data;
        return;
    }

    if (addr == REG_TX_TAIL) {
        /*
         * Guest driver has filled new TX descriptors and advances the tail.
         * Process all pending descriptors up to the new tail index.
         */
        s->tx_tail = (uint32_t)data;
        minimal_tx_kick(s);
        return;
    }

    if (addr + size > sizeof(s->regs)) {
        printf("minimal_pcie_nic: MMIO write out-of-bounds addr=0x%#" PRIx64
               " size=%u\n", (uint64_t)addr, size);
        return;
    }

    printf("minimal_pcie_nic: MMIO write addr=0x%#" PRIx64
           " size=%u data=0x%llx\n",
           (uint64_t)addr, size, (unsigned long long)data);


    /* This is only for msi/msi-x testing:
     * Writing a vector number (low 8 bits of data) to offset 0x0 fires the
     * corresponding interrupt immediately, so userspace test programs can
     * verify IRQ delivery without sending a real packet.
     */
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


/*
 * minimal_mmio_ops — vtable binding MMIO callbacks to the MemoryRegion.
 *
 * Passed to memory_region_init_io() in realize().  QEMU uses this struct
 * to dispatch guest load/store instructions that hit the BAR0 window.
 *
 * DEVICE_NATIVE_ENDIAN means QEMU will NOT byte-swap data before/after
 * calling our callbacks — we perform byte-swapping ourselves using the
 * le*_to_cpu / cpu_to_le* helpers.
 */
/* MemoryRegionOps for the MMIO region */
static const MemoryRegionOps minimal_mmio_ops = {
    .read = minimal_mmio_read,
    .write = minimal_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/*
 * minimal_receive_packet — called by QEMU's net layer when a packet arrives
 * from the host network backend (tap device, user networking, etc.).
 *
 * This is the inbound data path (host → guest):
 *
 *  Step 1 — Sanity check: if the driver has not yet programmed rx_ring_size
 *            the ring is not ready; drop the packet and return 0.
 *
 *  Step 2 — Compute the guest-physical address of the next RX descriptor
 *            using the ring base address and the current head index.
 *            desc_addr = rx_ring_base + rx_head × sizeof(rx_desc)
 *
 *  Step 3 — Read the descriptor from guest memory via pci_dma_read().
 *            The descriptor tells us the guest buffer address (desc.addr)
 *            where the packet should be placed.
 *            pci_dma_read() is IOMMU-aware: if the guest uses an IOMMU the
 *            address is translated transparently.
 *
 *  Step 4 — DMA the packet payload into the guest buffer via pci_dma_write().
 *            This is the actual "DMA transfer" — bytes flow from QEMU's heap
 *            into guest RAM at the address the driver prepared.
 *
 *  Step 5 — Update the descriptor: set len = packet size, set flags = RX_DONE.
 *            Write the updated descriptor back to guest memory so the driver
 *            can poll or be interrupted to find the completed entry.
 *
 *  Step 6 — Advance rx_head modulo rx_ring_size (ring wrap-around).
 *
 *  Step 7 — Fire MSI-X vector 0 to notify the guest driver that a new packet
 *            is available.  The driver's ISR will scan the ring for RX_DONE
 *            descriptors and hand the buffers to the network stack.
 *
 * Returns the number of bytes consumed (== size on success, 0 on drop).
 */
static ssize_t minimal_receive_packet(NetClientState *nc,
                                      const uint8_t *buf,
                                      size_t size)
{
    MinimalPCIeNICState *s = qemu_get_nic_opaque(nc);
    struct rx_desc desc;
    uint64_t desc_addr;

    printf("minimal_pcie_nic: [TRACE] receive_packet size=%zu "
           "rx_ring_base=0x%" PRIx64 " rx_ring_size=%u rx_head=%u\n",
           size, s->rx_ring_base, s->rx_ring_size, s->rx_head);

    /* Step 1: Drop packet if driver hasn't initialized the RX ring yet. */
    if (!s->rx_ring_size || !s->rx_ring_base) {
        printf("minimal_pcie_nic: [TRACE] DROP: ring not ready\n");
        return 0;   /* driver not ready */
    }

    /* Step 2: Locate the next RX descriptor in the ring. */
    desc_addr = s->rx_ring_base +
                s->rx_head * sizeof(desc);

    /* Step 3: Read RX descriptor from guest memory to learn buffer address. */
    pci_dma_read(&s->parent_obj,
                 desc_addr, &desc, sizeof(desc));

    printf("minimal_pcie_nic: [TRACE] desc[%u] addr=0x%" PRIx64
           " len=%u flags=0x%x\n",
           s->rx_head, desc.addr, desc.len, desc.flags);

    /* Drop packet if the driver has not reclaimed this descriptor yet. */
    if (desc.flags & RX_DONE) {
        printf("minimal_pcie_nic: [TRACE] DROP: ring full (desc[%u] still RX_DONE)\n",
               s->rx_head);
        return 0;   /* ring full — driver is behind */
    }

    /* Step 4: DMA packet payload into the guest buffer the driver prepared. */
    pci_dma_write(&s->parent_obj,
                  desc.addr, buf, size);

    printf("minimal_pcie_nic: [TRACE] DMA write %zu bytes → guest buf 0x%" PRIx64 "\n",
           size, desc.addr);

    /* Step 5: Mark descriptor complete so the driver knows it's ready. */
    desc.len = size;
    desc.flags = RX_DONE;

    /* Write the updated descriptor back to guest memory. */
    pci_dma_write(&s->parent_obj,
                  desc_addr, &desc, sizeof(desc));

    /* Step 6: Advance the ring head (wrap around at rx_ring_size). 
       Example: if rx_head is 3 and rx_ring_size is 4, then (3+1)%4 = 0.
       Now the next descriptor is at index 0.  */
    s->rx_head = (s->rx_head + 1) % s->rx_ring_size;

    printf("minimal_pcie_nic: [TRACE] desc marked RX_DONE, rx_head now %u, firing IRQ\n",
           s->rx_head);

    /* Step 7: Interrupt the guest driver via MSI-X vector 0. */
    minimal_raise_irq(s, 0);

    printf("minimal_pcie_nic: [TRACE] RX complete — %zu bytes delivered\n", size);

    return size;
}

/*
 * net_ops — NetClientInfo vtable connecting this device to QEMU's net layer.
 *
 * .type    = NET_CLIENT_DRIVER_NIC  identifies this as a NIC (not a backend).
 * .size    = sizeof(NICState)       tells QEMU how much memory to allocate
 *             for the NICState object created by qemu_new_nic().
 * .receive = minimal_receive_packet callback invoked for every inbound frame.
 *
 * QEMU's net layer glues a "backend" (e.g., tap, user, socket) to this NIC
 * via the netdev= option on the command line.  When the backend delivers a
 * frame, QEMU calls net_ops.receive().
 */
static NetClientInfo net_ops = {
    .type        = NET_CLIENT_DRIVER_NIC,
    .size        = sizeof(NICState),
    .can_receive = minimal_can_receive,
    .receive     = minimal_receive_packet,
};

/*
 * minimal_pcie_nic_realize — device instantiation callback.
 *
 * QEMU calls this once per device instance when the machine is started (or
 * when the user hot-plugs the device).  This is where all one-time setup
 * happens: config space, BARs, interrupts, and the network backend.
 *
 * The sequence mirrors what real PCIe NIC firmware or hardware init does:
 *
 *  1. Set PCI config space identity fields (Vendor/Device ID, class, rev).
 *  2. Zero the simulated register file.
 *  3. Enable Memory Space decoding and Bus Mastering (required for DMA).
 *  4. Create the BAR0 MMIO region and bind it to our read/write callbacks.
 *  5. Create the BAR1 region and initialise MSI-X on top of it.
 *  6. Create the QEMU NIC and attach it to the netdev backend.
 */
/* Realize function: called when device is instantiated */
static void minimal_pcie_nic_realize(PCIDevice *pdev, Error **errp)
{
    MinimalPCIeNICState *s = MINIMAL_PCIE_NIC(pdev);
    uint8_t *macaddr;

    printf("minimal_pcie_nic: realize called (host log)\n");

    /* Step 1: Populate PCI config space identity fields.
     * These appear in the guest's lspci output and are used by the driver
     * to match this device (vendor 0x1af4 = VirtIO / Red Hat, reused here).
     */
    pci_config_set_vendor_id(pdev->config, 0x1af4);
    pci_config_set_device_id(pdev->config, 0x10f1);

    /* Class: Ethernet controller — tells the OS what kind of device this is. */
    pci_config_set_class(pdev->config, PCI_CLASS_NETWORK_ETHERNET);

    /* Revision ID — device hardware revision exposed to the guest. */
    pci_config_set_revision(pdev->config, 0x01);

    /* Step 2: Zero the simulated register file before the guest reads it. */
    memset(s->regs, 0, sizeof(s->regs));

    /* Step 3: Set Command register bits.
     *   PCI_COMMAND_MEMORY — enables decoding of Memory Space BAR accesses.
     *   PCI_COMMAND_MASTER — enables the device to initiate DMA (bus master).
     * Without MASTER set, pci_dma_read/write() would be refused by the bus.
     */
    uint16_t cmd = PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    pci_set_word(pdev->config + PCI_COMMAND, cmd);

    /* Step 4a: Initialise BAR0 MMIO region.
     *
     * memory_region_init_io() creates a virtual address window (NOT RAM).
     * Size 0x1000 = 4 KB.  When the guest reads/writes into this window
     * after the OS has programmed BAR0, QEMU dispatches to minimal_mmio_ops.
     */
    memory_region_init_io(&s->mmio, OBJECT(s), &minimal_mmio_ops, s,
                          "minimal-pcie-mmio", 0x1000);

    /* Step 4b: Register BAR0 with the PCI core.
     * pci_register_bar() writes the BAR size and type into config space.
     * The guest OS (during PCI enumeration) reads the size, allocates an
     * address range, and writes the base address back into BAR0.  From that
     * point, guest accesses to [base, base+0x1000) hit our callbacks.
     */
    pci_register_bar(pdev, BAR0_IDX , PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    /* Step 5a: Create a plain (no callbacks) MemoryRegion for BAR1.
     * msix_init() will subdivide this region into the vector table and PBA
     * sub-regions, mapping their own internal callbacks into it.
     */
    memory_region_init(&s->msix_bar, OBJECT(s), "minimal-msix-bar", MSIX_BAR_SIZE);

    /* Register BAR1 so the guest can map it. */
    pci_register_bar(pdev, BAR1_MSIX_IDX, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->msix_bar);

#ifdef MSIX_ENABLE
    /* Step 5b: Install MSI-X capability and prepare vector table in BAR1. */
    minimal_init_msix(s);
#else
    /* Step 5b (alt): Install MSI capability instead.
     * msi_init() appends the MSI capability structure to the config space
     * capability list and allocates the requested number of vectors.
     *   offset 0        — auto-choose position in config space
     *   4 vectors       — allocate 4 MSI vectors
     *   false           — allow 64-bit message addresses
     *   true            — enable per-vector masking (MSI Mask register)
     */
    if (msi_init(pdev,
                 0,      /* offset in config space */
                 4,      /* number of MSI vectors */
                 false,  /* 32-bit address */
                 true,   /* per-vector masking enabled */
                 errp) < 0) {
        return;
    }
#endif

    /* Step 6a: Ensure the MAC address is set (use QEMU default if not given
     * by the user via -device ...,mac=XX:XX:XX:XX:XX:XX).
     */
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    macaddr = s->conf.macaddr.a;

    /* Step 6b: Create the QEMU NIC object and attach it to the net backend.
     *
     * qemu_new_nic() allocates a NICState, links it to the NICConf (which
     * holds the MAC address and the netdev= reference from the command line),
     * and registers our net_ops callbacks with QEMU's network subsystem.
     *
     * After this call, the host networking backend (tap, user, …) will call
     * minimal_receive_packet() for every frame destined for our MAC.
     */
    s->nic = qemu_new_nic(&net_ops,
                      &s->conf,                    // default config
                      object_get_typename(OBJECT(s)),      // NIC name
                      DEVICE(pdev)->id,        // unique NIC ID
                      &PCI_DEVICE(s)->qdev.mem_reentrancy_guard,
                      s);

    /* Format the MAC address string displayed in QEMU's info network output. */
    qemu_format_nic_info_str(qemu_get_queue(s->nic), macaddr);

}

/*
 * minimal_pcie_nic_uninit — device teardown callback (called on hot-unplug
 * or QEMU shutdown).
 *
 * Mirror of realize: release resources in reverse order.
 *   qemu_del_nic()  — detaches from the net backend and frees NICState.
 *   msix_uninit() / msi_uninit() — removes the capability from config space
 *     and frees internal QEMU state for the interrupt mechanism.
 *
 * QEMU automatically destroys MemoryRegions registered as BARs after this
 * callback returns, so we do not need to call memory_region_cleanup() here.
 */
static void minimal_pcie_nic_uninit(PCIDevice *pdev)
{
    MinimalPCIeNICState *s = MINIMAL_PCIE_NIC(pdev);

    /* Clean up NIC */
    qemu_del_nic(s->nic);

    /* Clean up MSI/MSI-X */
#ifdef MSIX_ENABLE
    msix_uninit(pdev, &s->msix_bar, &s->msix_bar);
#else
    msi_uninit(pdev);
#endif
    printf("pcie nic un-init\n");
}

/*
 * minimal_pcie_nic_properties — device properties exposed on the command line.
 *
 * DEFINE_NIC_PROPERTIES expands to properties for:
 *   netdev=<id>   — the -netdev backend to attach to
 *   mac=<addr>    — override the MAC address
 *   bootindex=<n> — firmware boot order hint
 *
 * DEFINE_PROP_END_OF_LIST() terminates the array (required sentinel).
 */
/* Device properties */
static Property minimal_pcie_nic_properties[] = {
    DEFINE_NIC_PROPERTIES(MinimalPCIeNICState, conf),
    DEFINE_PROP_END_OF_LIST(),
};

/*
 * minimal_pcie_nic_class_init — QOM class initialisation callback.
 *
 * Called once at QEMU startup (not per-instance) to fill in the class vtable.
 *
 * PCIDeviceClass fields:
 *   .realize    — per-instance init (minimal_pcie_nic_realize)
 *   .exit       — per-instance teardown (minimal_pcie_nic_uninit)
 *   .vendor_id / .device_id — default IDs (also set per-instance in realize,
 *                 but the class values are used by the PCI subsystem before
 *                 realize is called, e.g., for device matching).
 *   .class_id   — PCI class code (Ethernet controller)
 *   .revision   — PCI revision
 *
 * DeviceClass fields:
 *   .categories — used by the QEMU device browser / help output
 *   .desc       — human-readable description shown by `info qtree`
 *   props       — command-line properties (netdev=, mac=, …)
 */
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
    
    /* Register device properties (netdev, mac, etc.) */
    device_class_set_props(dc, minimal_pcie_nic_properties);
}

/*
 * minimal_pcie_nic_register_types — register the device type with QEMU's QOM.
 *
 * TypeInfo describes the type to QOM:
 *   .name          — string key used with -device and OBJECT_CHECK casts
 *   .parent        — we inherit from TYPE_PCI_DEVICE
 *   .instance_size — QEMU allocates this many bytes per device instance
 *                    (must equal sizeof(MinimalPCIeNICState))
 *   .class_init    — called once to populate the class vtable
 *   .interfaces    — declares PCIe compliance (required for PCIe capabilities
 *                    like MSI-X; without INTERFACE_PCIE_DEVICE the device
 *                    would be treated as a legacy PCI device)
 *
 * type_register_static() inserts the TypeInfo into QEMU's global type
 * registry so that -device minimal-pcie-nic will find and instantiate it.
 */
/* Type registration */
static void minimal_pcie_nic_register_types(void)
{
    /* Add supported interface*/
    static const InterfaceInfo interfaces[] = {
        { INTERFACE_PCIE_DEVICE },  /* marks device as PCIe (not legacy PCI) */
        { }                         /* sentinel — terminates the array */
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

/*
 * type_init — register the type-registration function with QEMU's module
 * initialisation framework.
 *
 * QEMU calls all functions registered via type_init() during startup, before
 * any device is realised.  This ensures that TYPE_MINIMAL_PCIE_NIC is in the
 * type registry by the time -device minimal-pcie-nic is processed.
 */
/* Initialize the type at QEMU startup */
type_init(minimal_pcie_nic_register_types);