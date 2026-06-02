// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#define DRV_NAME            "minimal_pcie_nic_drv"
#define VENDOR_ID           0x1af4
#define DEVICE_ID           0x10f1
#define MAX_MSI_VECTORS     4 // define in qemu pci device minimal_pci_nic
#define MAX_MSIX_VECTORS    4 // define in qemu pci device minimal_pci_nic

#define MSIX_ENABLE

/*
 * Compile-time trace toggle.
 * Controlled via EXTRA_CFLAGS in the Yocto recipe (-DTRACE_PCIE_NIC).
 * Comment out or remove from the recipe to silence all [TRACE] messages
 * once the data path is confirmed working.
 */
#ifdef TRACE_PCIE_NIC
#define tdbg(ndev, fmt, ...) \
    netdev_info((ndev), "[TRACE] " fmt, ##__VA_ARGS__)
#else
#define tdbg(ndev, fmt, ...) do {} while (0)
#endif

/* Ring Configurations */
#define REG_RX_RING_BASE   0x10
#define REG_RX_RING_SIZE   0x18
#define REG_RX_TAIL        0x1C
#define REG_RX_HEAD        0x20

#define RX_RING_SIZE        16
#define RX_BUF_SIZE         2048
#define RX_DONE             1

#define REG_TX_RING_BASE   0x30
#define REG_TX_RING_SIZE   0x38
#define REG_TX_TAIL        0x3C
#define REG_TX_HEAD        0x40

#define TX_RING_SIZE        16
#define TX_BUF_SIZE         2048
#define TX_READY            1
#define TX_DONE             2

/* QEMU NIC reads and writes exactly this layout using PCIe DMA */
struct rx_desc {
    u64 addr;   // where NIC must DMA the packet
    u16 len;    // length written by NIC
    u16 flags;  // DONE bit from NIC
};

struct tx_desc {
    u64 addr;   // guest-physical address of the outbound packet buffer
    u16 len;    // number of bytes to transmit
    u16 flags;  // TX_READY set by driver; TX_DONE set by device
};

struct minimal_dev {
    struct pci_dev *pdev;
    void __iomem *bar0;    // MMIO registers
    void __iomem *bar1;    // MSI-X table/PBA (optional mapping)
    int nvec_irq;
    struct net_device *netdev;

    struct rx_desc *rx_ring;   // Virtual address where Linux sees the descriptor ring
    dma_addr_t rx_ring_dma;     // Physical address QEMU NIC uses to access ring

    void *rx_bufs[RX_RING_SIZE];    // Actual packet buffers linux will use to access data
    dma_addr_t rx_bufs_dma[RX_RING_SIZE];   // Physical addresses of those buffers
    u32 rx_clean;               // SW consumer index (next descriptor to process)
    struct napi_struct napi;    // NAPI instance for RX bottom-half processing

    struct tx_desc *tx_ring;    // Virtual address of TX descriptor ring
    dma_addr_t tx_ring_dma;     // Physical address QEMU NIC uses to access TX ring

    void *tx_bufs[TX_RING_SIZE];      // TX packet buffers
    dma_addr_t tx_bufs_dma[TX_RING_SIZE];
    u32 tx_head;                // SW copy: next descriptor to reclaim after TX_DONE
    u32 tx_tail;                // SW copy: next slot to fill
};

static int minimal_open(struct net_device *ndev)
{
    struct minimal_dev *mdev = netdev_priv(ndev);
    napi_enable(&mdev->napi);
    netif_start_queue(ndev);
    return 0;
}

static int minimal_stop(struct net_device *ndev)
{
    struct minimal_dev *mdev = netdev_priv(ndev);
    netif_stop_queue(ndev);
    napi_disable(&mdev->napi);
    return 0;
}

static netdev_tx_t minimal_start_xmit(struct sk_buff *skb,
                                      struct net_device *ndev)
{
    struct minimal_dev *mdev = netdev_priv(ndev);
    u32 slot = mdev->tx_tail;

    if (skb->len > TX_BUF_SIZE) {
        dev_kfree_skb(skb);
        ndev->stats.tx_dropped++;
        return NETDEV_TX_OK;
    }

    /* Copy payload into the pre-allocated DMA-coherent buffer for this slot. */
    memcpy(mdev->tx_bufs[slot], skb->data, skb->len);

    /* Fill the TX descriptor — addr and len are already cached from probe. */
    mdev->tx_ring[slot].addr  = mdev->tx_bufs_dma[slot];
    mdev->tx_ring[slot].len   = skb->len;
    mdev->tx_ring[slot].flags = TX_READY;

    /* Advance the SW tail and kick the device by writing the new index. */
    mdev->tx_tail = (mdev->tx_tail + 1) % TX_RING_SIZE;
    writel(mdev->tx_tail, mdev->bar0 + REG_TX_TAIL);

    ndev->stats.tx_packets++;
    ndev->stats.tx_bytes += skb->len;

    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}

static const struct net_device_ops minimal_netdev_ops = {
    .ndo_open       = minimal_open,
    .ndo_stop       = minimal_stop,
    .ndo_start_xmit = minimal_start_xmit,
};

static irqreturn_t minimal_irq_handler(int irq, void *dev_id)
{
    struct minimal_dev *mdev = dev_id;
    tdbg(mdev->netdev, "IRQ fired (irq=%d), scheduling NAPI\n", irq);
    /*
     * Offload RX processing to the NAPI poll function (bottom-half).
     * napi_schedule_irqoff() is safe here because we are already in
     * hard-IRQ context with local IRQs disabled.
     */
    napi_schedule_irqoff(&mdev->napi);
    return IRQ_HANDLED;
}

/*
 * minimal_napi_poll — NAPI bottom-half: drain the RX ring up to @budget frames.
 *
 * Called from softirq context after the ISR schedules us.  We process up to
 * @budget completed RX descriptors per call so we yield the CPU fairly between
 * multiple devices and other softirq work.
 *
 * Per descriptor:
 *  1. Check RX_DONE flag; stop if not yet filled by the device.
 *  2. Allocate an skb and copy the packet bytes from the DMA buffer.
 *  3. Let eth_type_trans() detect the L3 protocol and strip the Ethernet header.
 *  4. Pass to napi_gro_receive() for GRO coalescing before stack delivery.
 *  5. Re-arm the descriptor (clear flags, restore full buffer length) so QEMU
 *     can reuse the slot for the next incoming packet.
 *
 * After draining (or hitting budget), write rx_clean to REG_RX_TAIL so the
 * QEMU device knows which descriptors are available again.
 *
 * If work_done < budget we have drained the ring; call napi_complete_done()
 * to re-arm interrupts and exit polling mode.
 */
static int minimal_napi_poll(struct napi_struct *napi, int budget)
{
    struct minimal_dev *mdev = container_of(napi, struct minimal_dev, napi);
    u32 head = readl(mdev->bar0 + REG_RX_HEAD);
    u32 i    = mdev->rx_clean;
    int work_done = 0;

    tdbg(mdev->netdev, "NAPI poll: rx_clean=%u rx_head=%u budget=%d\n",
         i, head, budget);

    while (i != head && work_done < budget) {
        struct rx_desc *desc = &mdev->rx_ring[i];

        tdbg(mdev->netdev, "  desc[%u] flags=0x%x len=%u\n",
             i, desc->flags, desc->len);

        if (!(desc->flags & RX_DONE)) {
            tdbg(mdev->netdev, "  desc[%u] not done yet — stop\n", i);
            break;
        }

        u16 len = desc->len;
        struct sk_buff *skb = netdev_alloc_skb_ip_align(mdev->netdev, len);
        if (likely(skb)) {
            memcpy(skb_put(skb, len), mdev->rx_bufs[i], len);
            skb->protocol = eth_type_trans(skb, mdev->netdev);
            mdev->netdev->stats.rx_packets++;
            mdev->netdev->stats.rx_bytes += len;
            tdbg(mdev->netdev, "  desc[%u] → netstack len=%u proto=0x%04x\n",
                 i, len, ntohs(skb->protocol));
            napi_gro_receive(napi, skb);
        } else {
            netdev_warn(mdev->netdev, "  desc[%u] skb alloc failed, dropping\n", i);
            mdev->netdev->stats.rx_dropped++;
        }

        /* Re-arm: clear the done flag and restore the full buffer length. */
        desc->len   = RX_BUF_SIZE;
        desc->flags = 0;

        i = (i + 1) % RX_RING_SIZE;
        work_done++;
    }

    /* Persist the updated consumer pointer and inform the device. */
    mdev->rx_clean = i;
    writel(mdev->rx_clean, mdev->bar0 + REG_RX_TAIL);

    tdbg(mdev->netdev, "NAPI poll done: work_done=%d rx_clean now %u\n",
         work_done, mdev->rx_clean);

    /* If we processed fewer than budget packets the ring is drained. */
    if (work_done < budget)
        napi_complete_done(napi, work_done);

    return work_done;
}

static int minimal_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
    struct minimal_dev *mdev;
    struct net_device *ndev;
    int ret, i;

    pr_info(DRV_NAME ": probe\n");

    ndev = alloc_etherdev(sizeof(*mdev));
    if (!ndev)
        return -ENOMEM;

    mdev = netdev_priv(ndev);
    mdev->pdev = pdev;
    mdev->netdev = ndev;
    pci_set_drvdata(pdev, ndev);  /* store ndev; mdev = netdev_priv(ndev) */

    /*
     * Register NAPI immediately after allocating mdev — before any hardware
     * is touched.  qemu_flush_queued_packets() (called when the driver writes
     * REG_RX_RING_SIZE) can cause QEMU to fire an MSI-X interrupt synchronously,
     * which schedules NAPI.  If napi->poll is still NULL at that point the
     * kernel crashes with pc=0x0.  Registering early ensures the poll pointer
     * is valid before any interrupt can arrive.
     */
    netif_napi_add(ndev, &mdev->napi, minimal_napi_poll);

    ndev->netdev_ops = &minimal_netdev_ops;
    ndev->min_mtu = 68;
    ndev->max_mtu = 1500;

    eth_hw_addr_random(ndev);
    SET_NETDEV_DEV(ndev, &pdev->dev);

    /* Enable PCI device and bus-mastering before any hardware access */
    pr_info(DRV_NAME ": PCI enable device\n");
    ret = pci_enable_device(pdev);
    if (ret)
        goto err_free_netdev;

    pci_set_master(pdev);

    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    if (ret) {
        dev_err(&pdev->dev, "Cannot set 64-bit DMA mask\n");
        goto err_disable;
    }

#ifdef MSIX_ENABLE
    /* Request MSI-X vectors explicitly */
    mdev->nvec_irq = pci_alloc_irq_vectors(pdev,
                                           1,
                                           MAX_MSIX_VECTORS,
                                           PCI_IRQ_MSIX);
#else
    /* Fallback to MSI with per-vector masking */
    mdev->nvec_irq = pci_alloc_irq_vectors(pdev,
                                1,
                                MAX_MSI_VECTORS,
                                PCI_IRQ_MSI);
#endif

    if (mdev->nvec_irq < 0) {
        ret = mdev->nvec_irq;
        goto err_disable;
    }

    for (i = 0; i < mdev->nvec_irq; i++) {
        int irq = pci_irq_vector(pdev, i);

        ret = devm_request_irq(&pdev->dev,
                               irq,
                               minimal_irq_handler,
                               0,
                               DRV_NAME,
                               mdev);
        if (ret) {
            dev_err(&pdev->dev, "IRQ %d request failed\n", i);
            goto err_irq;
        }
    }

    /* Map BAR0 (device MMIO) */
    ret = pci_request_region(pdev, 0, DRV_NAME);
    if (ret)
        goto err_irq;

    mdev->bar0 = pci_iomap(pdev, 0, 0);
    if (!mdev->bar0) {
        ret = -ENOMEM;
        goto err_region0;
    }

    /* Map BAR1 (MSI-X table/PBA) */
    ret = pci_request_region(pdev, 1, DRV_NAME);
    if (ret)
        goto err_unmap_bar0;

    mdev->bar1 = pci_iomap(pdev, 1, 0);
    if (!mdev->bar1) {
        ret = -ENOMEM;
        goto err_region1;
    }

    /* Allocate RX ring
     * 1. Allocates memory
     * 2. Makes it visible to the device
     * Returns the physical DMA address
     */
    mdev->rx_ring = dma_alloc_coherent(&pdev->dev,
            sizeof(struct rx_desc) * RX_RING_SIZE,
            &mdev->rx_ring_dma, GFP_KERNEL);

    pr_info("RX ring VA=%p  DMA=%pad  size=%zu\n",
        mdev->rx_ring,
        &mdev->rx_ring_dma,
        sizeof(struct rx_desc) * RX_RING_SIZE);

    /* Allocate 16 packet buffers of 2048 bytes each */
    for (i = 0; i < RX_RING_SIZE; i++) {
        mdev->rx_bufs[i] = dma_alloc_coherent(&pdev->dev,
                RX_BUF_SIZE,
                &mdev->rx_bufs_dma[i],
                GFP_KERNEL);

        mdev->rx_ring[i].addr = mdev->rx_bufs_dma[i];
        mdev->rx_ring[i].len = RX_BUF_SIZE;
        mdev->rx_ring[i].flags = 0;

        pr_info("RX[%02d] desc_va=%p  buf_va=%p  buf_dma=%pad\n",
            i,
            &mdev->rx_ring[i],
            mdev->rx_bufs[i],
            &mdev->rx_bufs_dma[i]);
    }

    /* Program device: use writeq for the 64-bit ring base address */
    writeq(mdev->rx_ring_dma,  mdev->bar0 + REG_RX_RING_BASE);
    writel(RX_RING_SIZE,       mdev->bar0 + REG_RX_RING_SIZE);
    writel(0,                  mdev->bar0 + REG_RX_TAIL);   /* driver starts consuming from index 0 */
    mdev->rx_clean = 0;

    pr_info(DRV_NAME ": RX ring registered: dma=0x%llx size=%u\n",
            (unsigned long long)mdev->rx_ring_dma, RX_RING_SIZE);

    /* Allocate TX ring */
    mdev->tx_ring = dma_alloc_coherent(&pdev->dev,
            sizeof(struct tx_desc) * TX_RING_SIZE,
            &mdev->tx_ring_dma, GFP_KERNEL);
    if (!mdev->tx_ring) {
        ret = -ENOMEM;
        goto err_dma;
    }

    for (i = 0; i < TX_RING_SIZE; i++) {
        mdev->tx_bufs[i] = dma_alloc_coherent(&pdev->dev,
                TX_BUF_SIZE,
                &mdev->tx_bufs_dma[i],
                GFP_KERNEL);
        if (!mdev->tx_bufs[i]) {
            ret = -ENOMEM;
            goto err_dma;
        }
        mdev->tx_ring[i].addr  = mdev->tx_bufs_dma[i];
        mdev->tx_ring[i].len   = 0;
        mdev->tx_ring[i].flags = 0;
    }

    mdev->tx_head = 0;
    mdev->tx_tail = 0;

    /* Tell device the TX ring location and size */
    writeq(mdev->tx_ring_dma, mdev->bar0 + REG_TX_RING_BASE);
    writel(TX_RING_SIZE,       mdev->bar0 + REG_TX_RING_SIZE);

    pr_info(DRV_NAME ": TX ring registered: dma=0x%llx size=%u\n",
            (unsigned long long)mdev->tx_ring_dma, TX_RING_SIZE);

    pr_info(DRV_NAME ": BAR0=%p BAR1=%p IRQ vectors=%d\n",
            mdev->bar0, mdev->bar1, mdev->nvec_irq);

    /* Register netdev last, after all hardware resources are ready */
    ret = register_netdev(ndev);
    if (ret)
        goto err_dma;

    pr_info(DRV_NAME ": registered netdev %s\n", ndev->name);

    return 0;

err_dma:
    netif_napi_del(&mdev->napi);
    for (i = 0; i < TX_RING_SIZE; i++) {
        if (mdev->tx_bufs[i])
            dma_free_coherent(&pdev->dev, TX_BUF_SIZE,
                              mdev->tx_bufs[i], mdev->tx_bufs_dma[i]);
    }
    if (mdev->tx_ring)
        dma_free_coherent(&pdev->dev,
                          sizeof(struct tx_desc) * TX_RING_SIZE,
                          mdev->tx_ring, mdev->tx_ring_dma);
    for (i = 0; i < RX_RING_SIZE; i++) {
        if (mdev->rx_bufs[i])
            dma_free_coherent(&pdev->dev, RX_BUF_SIZE,
                              mdev->rx_bufs[i], mdev->rx_bufs_dma[i]);
    }
    if (mdev->rx_ring)
        dma_free_coherent(&pdev->dev,
                          sizeof(struct rx_desc) * RX_RING_SIZE,
                          mdev->rx_ring, mdev->rx_ring_dma);
err_region1:
    if (mdev->bar1)
        pci_iounmap(pdev, mdev->bar1);
    pci_release_region(pdev, 1);
err_unmap_bar0:
    pci_iounmap(pdev, mdev->bar0);
err_region0:
    pci_release_region(pdev, 0);
err_irq:
    pci_free_irq_vectors(pdev);
err_disable:
    pci_disable_device(pdev);
err_free_netdev:
    free_netdev(ndev);
    return ret;
}

/* Remove */
static void minimal_remove(struct pci_dev *pdev)
{
    struct net_device *ndev = pci_get_drvdata(pdev);
    struct minimal_dev *mdev = netdev_priv(ndev);
    int i;

    /* Unregister first so no new traffic arrives while we tear down. */
    unregister_netdev(ndev);
    netif_napi_del(&mdev->napi);

    pr_info(DRV_NAME ": remove\n");

    for (i = 0; i < TX_RING_SIZE; i++) {
        if (mdev->tx_bufs[i])
            dma_free_coherent(&pdev->dev, TX_BUF_SIZE,
                              mdev->tx_bufs[i],
                              mdev->tx_bufs_dma[i]);
    }
    if (mdev->tx_ring)
        dma_free_coherent(&pdev->dev,
                          sizeof(struct tx_desc) * TX_RING_SIZE,
                          mdev->tx_ring,
                          mdev->tx_ring_dma);

    for (i = 0; i < RX_RING_SIZE; i++) {
        if (mdev->rx_bufs[i])
            dma_free_coherent(&pdev->dev, RX_BUF_SIZE,
                              mdev->rx_bufs[i],
                              mdev->rx_bufs_dma[i]);
    }

    if (mdev->rx_ring)
        dma_free_coherent(&pdev->dev,
                          sizeof(struct rx_desc) * RX_RING_SIZE,
                          mdev->rx_ring,
                          mdev->rx_ring_dma);

    if (mdev->bar1)
        pci_iounmap(pdev, mdev->bar1);
    pci_release_region(pdev, 1);

    if (mdev->bar0)
        pci_iounmap(pdev, mdev->bar0);
    pci_release_region(pdev, 0);

    pci_free_irq_vectors(pdev);
    pci_disable_device(pdev);

    /* free_netdev last: mdev is embedded via netdev_priv, freed with ndev. */
    free_netdev(ndev);
}

/* PCI ID Table */
static const struct pci_device_id minimal_pci_ids[] = {
    { PCI_DEVICE(VENDOR_ID, DEVICE_ID) },
    { }
};
MODULE_DEVICE_TABLE(pci, minimal_pci_ids);

/* PCI Driver */
static struct pci_driver minimal_pci_driver = {
    .name     = DRV_NAME,
    .id_table = minimal_pci_ids,
    .probe    = minimal_probe,
    .remove   = minimal_remove,
};

module_pci_driver(minimal_pci_driver);

MODULE_AUTHOR("Abhishek Ojha");
MODULE_DESCRIPTION("Minimal PCIe NIC driver");
MODULE_LICENSE("GPL");
