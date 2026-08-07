// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/vmalloc.h>

#define DRV_NAME            "minimal_pcie_nic_drv"
#define VENDOR_ID           0x1af4
#define DEVICE_ID           0x10f1
#define MAX_MSI_VECTORS     4 // define in qemu pci device minimal_pci_nic
#define MAX_MSIX_VECTORS    4 // define in qemu pci device minimal_pci_nic

#define MSIX_ENABLE


/* Ring Configurations */
#define REG_RX_RING_BASE   0x10
#define REG_RX_RING_SIZE   0x18
#define REG_RX_TAIL        0x1C
#define REG_RX_HEAD        0x20

#define RX_RING_SIZE        128
#define RX_BUF_SIZE         2048
#define RX_DONE             1

#define REG_TX_RING_BASE   0x30
#define REG_TX_RING_SIZE   0x38
#define REG_TX_TAIL        0x3C
#define REG_TX_HEAD        0x40

#define TX_RING_SIZE        128
#define TX_BUF_SIZE         2048
#define TX_READY            1
#define TX_DONE             2

/* QEMU NIC reads and writes exactly this layout using PCIe DMA */

struct tx_buff {
    void *buf;   // virtual address of the packet buffer
    dma_addr_t dma;  // physical address for DMA
};

struct rx_buff {
    void *buf;   // virtual address of the packet buffer
    dma_addr_t dma;  // physical address for DMA
};
struct rx_desc {
    dma_addr_t addr;   // where NIC must DMA the packet
    u16 len;            // length written by NIC
    u16 flags;          // DONE bit from NIC
};

struct tx_desc {
    dma_addr_t addr;   // guest-physical address of the outbound packet buffer
    u16 len;            // number of bytes to transmit
    u16 flags;          // TX_READY set by driver; TX_DONE set by device
};

struct tx_ring {
    // Virtual address of the TX descriptor ring, use count to track the number of descriptors
    void *desc;
    struct tx_buff *tx_bufs;  // array of TX buffers (virtual + DMA addresses)
    unsigned int count;                           // number of descriptors in the ring
    u32 head;                            // next descriptor to reclaim
    u32 tail;                            // next slot to fill
};

struct rx_ring {
    // Virtual address of the RX descriptor ring, use count to track the number of descriptors
    void *desc;
    struct rx_buff *rx_bufs;  // array of RX buffers (virtual + DMA addresses)
    unsigned int count;                           // number of descriptors in the ring
    u32 head;                            // next descriptor to reclaim
    u32 tail;                            // next slot to fill
};

struct minimal_dev {
    struct pci_dev *pdev;
    void __iomem *bar0;    // MMIO registers
    void __iomem *bar1;    // MSI-X table/PBA (optional mapping)
    int nvec_irq;
    struct net_device *netdev;
    struct napi_struct napi;    // NAPI instance for RX bottom-half processing

    struct rx_ring rx_ring;     // RX descriptor ring + per-buffer bookkeeping
    dma_addr_t rx_ring_dma;     // Physical address QEMU NIC uses to access RX ring

    struct tx_ring tx_ring;     // TX descriptor ring + per-buffer bookkeeping
    dma_addr_t tx_ring_dma;     // Physical address QEMU NIC uses to access TX ring
};

/*
 * minimal_setup_rx_resources — allocate and program the RX descriptor ring.
 *
 * Allocates the coherent RX ring plus one DMA buffer per descriptor, wires the
 * buffer addresses into the ring, then hands the ring base/size to the device
 * and resets the software consumer index.  Called from minimal_open() so the
 * data-path resources only exist while the interface is administratively up.
 */
static int minimal_setup_rx_resources(struct minimal_dev *mdev)
{
    struct pci_dev *pdev = mdev->pdev;
    struct rx_ring *rx = &mdev->rx_ring;
    struct rx_desc *descs;
    int i;

    rx->count = RX_RING_SIZE;

    /* Per-descriptor buffer bookkeeping (virtual + DMA addresses). */
    rx->rx_bufs = kcalloc(rx->count, sizeof(*rx->rx_bufs), GFP_KERNEL);
    if (!rx->rx_bufs)
        return -ENOMEM;

    /* Allocate RX descriptor ring
     * 1. Allocates memory
     * 2. Makes it visible to the device
     * Returns the physical DMA address
     */
    rx->desc = dma_alloc_coherent(&pdev->dev,
            sizeof(struct rx_desc) * rx->count,
            &mdev->rx_ring_dma, GFP_KERNEL);
    if (!rx->desc)
        goto err_free_bufs_array;

    descs = rx->desc;

    /* Allocate one packet buffer of RX_BUF_SIZE bytes per descriptor */
    for (i = 0; i < rx->count; i++) {
        rx->rx_bufs[i].buf = dma_alloc_coherent(&pdev->dev,
                RX_BUF_SIZE,
                &rx->rx_bufs[i].dma,
                GFP_KERNEL);
        if (!rx->rx_bufs[i].buf)
            goto err_free_bufs;

        descs[i].addr  = rx->rx_bufs[i].dma;
        descs[i].len   = RX_BUF_SIZE;
        descs[i].flags = 0;
    }

    /* Program device: use writeq for the 64-bit ring base address */
    writeq(mdev->rx_ring_dma,  mdev->bar0 + REG_RX_RING_BASE);
    writel(rx->count,          mdev->bar0 + REG_RX_RING_SIZE);
    writel(0,                  mdev->bar0 + REG_RX_TAIL);   /* driver starts consuming from index 0 */
    rx->head = 0;
    rx->tail = 0;

    return 0;

err_free_bufs:
    while (--i >= 0) {
        dma_free_coherent(&pdev->dev, RX_BUF_SIZE,
                          rx->rx_bufs[i].buf, rx->rx_bufs[i].dma);
        rx->rx_bufs[i].buf = NULL;
    }
    dma_free_coherent(&pdev->dev,
                      sizeof(struct rx_desc) * rx->count,
                      rx->desc, mdev->rx_ring_dma);
    rx->desc = NULL;
err_free_bufs_array:
    kfree(rx->rx_bufs);
    rx->rx_bufs = NULL;
    return -ENOMEM;
}

/* minimal_free_rx_resources — release everything minimal_setup_rx_resources got. */
static void minimal_free_rx_resources(struct minimal_dev *mdev)
{
    struct pci_dev *pdev = mdev->pdev;
    struct rx_ring *rx = &mdev->rx_ring;
    int i;

    if (rx->rx_bufs) {
        for (i = 0; i < rx->count; i++) {
            if (rx->rx_bufs[i].buf)
                dma_free_coherent(&pdev->dev, RX_BUF_SIZE,
                                  rx->rx_bufs[i].buf, rx->rx_bufs[i].dma);
        }
        kfree(rx->rx_bufs);
        rx->rx_bufs = NULL;
    }

    if (rx->desc) {
        dma_free_coherent(&pdev->dev,
                          sizeof(struct rx_desc) * rx->count,
                          rx->desc, mdev->rx_ring_dma);
        rx->desc = NULL;
    }
}

/*
 * minimal_setup_tx_resources — allocate and program the TX descriptor ring.
 *
 * Allocates the coherent TX ring plus one DMA buffer per descriptor and hands
 * the ring base/size to the device.  Outbound packets are copied into these
 * pre-allocated buffers in minimal_start_xmit(), mirroring the RX path.
 */
static int minimal_setup_tx_resources(struct minimal_dev *mdev)
{
    struct pci_dev *pdev = mdev->pdev;
    struct tx_ring *tx = &mdev->tx_ring;
    struct tx_desc *descs;
    int i;

    tx->count = TX_RING_SIZE;

    /* Per-descriptor buffer bookkeeping (virtual + DMA addresses). */
    tx->tx_bufs = kcalloc(tx->count, sizeof(*tx->tx_bufs), GFP_KERNEL);
    if (!tx->tx_bufs)
        return -ENOMEM;

    /* Allocate TX descriptor ring */
    tx->desc = dma_alloc_coherent(&pdev->dev,
            sizeof(struct tx_desc) * tx->count,
            &mdev->tx_ring_dma, GFP_KERNEL);
    if (!tx->desc)
        goto err_free_bufs_array;

    descs = tx->desc;

    /* Allocate one packet buffer of TX_BUF_SIZE bytes per descriptor */
    for (i = 0; i < tx->count; i++) {
        tx->tx_bufs[i].buf = dma_alloc_coherent(&pdev->dev,
                TX_BUF_SIZE,
                &tx->tx_bufs[i].dma,
                GFP_KERNEL);
        if (!tx->tx_bufs[i].buf)
            goto err_free_bufs;

        descs[i].addr  = tx->tx_bufs[i].dma;
        descs[i].len   = 0;
        descs[i].flags = 0;
    }

    tx->head = 0;
    tx->tail = 0;

    /* Tell device the TX ring location and size */
    writeq(mdev->tx_ring_dma, mdev->bar0 + REG_TX_RING_BASE);
    writel(tx->count,          mdev->bar0 + REG_TX_RING_SIZE);

    return 0;

err_free_bufs:
    while (--i >= 0) {
        dma_free_coherent(&pdev->dev, TX_BUF_SIZE,
                          tx->tx_bufs[i].buf, tx->tx_bufs[i].dma);
        tx->tx_bufs[i].buf = NULL;
    }
    dma_free_coherent(&pdev->dev,
                      sizeof(struct tx_desc) * tx->count,
                      tx->desc, mdev->tx_ring_dma);
    tx->desc = NULL;
err_free_bufs_array:
    kfree(tx->tx_bufs);
    tx->tx_bufs = NULL;
    return -ENOMEM;
}

/* minimal_free_tx_resources — release everything minimal_setup_tx_resources got. */
static void minimal_free_tx_resources(struct minimal_dev *mdev)
{
    struct pci_dev *pdev = mdev->pdev;
    struct tx_ring *tx = &mdev->tx_ring;
    int i;

    if (tx->tx_bufs) {
        for (i = 0; i < tx->count; i++) {
            if (tx->tx_bufs[i].buf)
                dma_free_coherent(&pdev->dev, TX_BUF_SIZE,
                                  tx->tx_bufs[i].buf, tx->tx_bufs[i].dma);
        }
        kfree(tx->tx_bufs);
        tx->tx_bufs = NULL;
    }

    if (tx->desc) {
        dma_free_coherent(&pdev->dev,
                          sizeof(struct tx_desc) * tx->count,
                          tx->desc, mdev->tx_ring_dma);
        tx->desc = NULL;
    }
}

static int minimal_open(struct net_device *ndev)
{
    struct minimal_dev *mdev = netdev_priv(ndev);
    int ret;

    /* Allocate the RX and TX data-path resources on interface up. */
    ret = minimal_setup_rx_resources(mdev);
    if (ret)
        return ret;

    ret = minimal_setup_tx_resources(mdev);
    if (ret) {
        minimal_free_rx_resources(mdev);
        return ret;
    }

    napi_enable(&mdev->napi);
    netif_start_queue(ndev);
    return 0;
}

static int minimal_stop(struct net_device *ndev)
{
    struct minimal_dev *mdev = netdev_priv(ndev);
    netif_stop_queue(ndev);
    napi_disable(&mdev->napi);

    /* Release the resources allocated in minimal_open(). */
    minimal_free_tx_resources(mdev);
    minimal_free_rx_resources(mdev);
    return 0;
}

static netdev_tx_t minimal_start_xmit(struct sk_buff *skb,
                                      struct net_device *ndev)
{
    struct minimal_dev *mdev = netdev_priv(ndev);
    struct tx_ring *tx = &mdev->tx_ring;
    struct tx_desc *descs = tx->desc;
    struct tx_desc *desc;
    u32 slot;

    /* Reclaim any slots the device has finished transmitting. */
    while (tx->head != tx->tail) {
        struct tx_desc *hdesc = &descs[tx->head];

        if (!(hdesc->flags & TX_DONE))
            break;

        hdesc->flags = 0;
        tx->head = (tx->head + 1) % tx->count;
    }

    /* Stop the queue if the ring is full (one slot kept as sentinel). */
    if ((tx->tail + 1) % tx->count == tx->head) {
        netif_stop_queue(ndev);
        return NETDEV_TX_BUSY;
    }

    if (skb->len > TX_BUF_SIZE) {
        dev_kfree_skb(skb);
        ndev->stats.tx_dropped++;
        return NETDEV_TX_OK;
    }

    slot = tx->tail;

    /* Copy the packet into the pre-allocated coherent TX buffer. */
    memcpy(tx->tx_bufs[slot].buf, skb->data, skb->len);

    desc        = &descs[slot];
    desc->addr  = tx->tx_bufs[slot].dma;
    desc->len   = skb->len;
    desc->flags = TX_READY;

    /* Advance the SW tail and kick the device by writing the new index. */
    tx->tail = (tx->tail + 1) % tx->count;
    writel(tx->tail, mdev->bar0 + REG_TX_TAIL);

    ndev->stats.tx_packets++;
    ndev->stats.tx_bytes += skb->len;

    /* Data already copied into the DMA buffer — the skb can be freed now. */
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
 * After draining (or hitting budget), write rx->tail to REG_RX_TAIL so the
 * QEMU device knows which descriptors are available again.
 *
 * If work_done < budget we have drained the ring; call napi_complete_done()
 * to re-arm interrupts and exit polling mode.
 */
static int minimal_napi_poll(struct napi_struct *napi, int budget)
{
    struct minimal_dev *mdev = container_of(napi, struct minimal_dev, napi);
    struct rx_ring *rx = &mdev->rx_ring;
    struct rx_desc *descs = rx->desc;
    u32 head = readl(mdev->bar0 + REG_RX_HEAD);
    u32 i    = rx->tail;
    int work_done = 0;

    rx->head = head;

    while (i != head && work_done < budget) {
        struct rx_desc *desc = &descs[i];

        if (!(desc->flags & RX_DONE)) {
            pr_err(DRV_NAME ": [TRACE]   desc[%u] not done yet — stop\n", i);
            break;
        }

        u16 len = desc->len;
        struct sk_buff *skb = netdev_alloc_skb_ip_align(mdev->netdev, len);
        if (likely(skb)) {
            memcpy(skb_put(skb, len), rx->rx_bufs[i].buf, len);
            skb->protocol = eth_type_trans(skb, mdev->netdev);
            mdev->netdev->stats.rx_packets++;
            mdev->netdev->stats.rx_bytes += len;
            napi_gro_receive(napi, skb);
        } else {
            netdev_warn(mdev->netdev, "  desc[%u] skb alloc failed, dropping\n", i);
            mdev->netdev->stats.rx_dropped++;
        }

        /* Re-arm: clear the done flag and restore the full buffer length. */
        desc->len   = RX_BUF_SIZE;
        desc->flags = 0;

        i = (i + 1) % rx->count;
        work_done++;
    }

    /* Persist the updated consumer pointer and inform the device. */
    rx->tail = i;
    writel(rx->tail, mdev->bar0 + REG_RX_TAIL);

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

    /*
     * This check is to only showcase the MSI/MSI-x functionality. In real * world we can directly pass both MSI and MSIX flag in one call.
     */
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

    /*
     * Register netdev last, after the BAR mappings and IRQs are ready.
     * The RX/TX DMA rings and buffers are allocated on demand in
     * minimal_open() and released in minimal_stop().
     */
    ret = register_netdev(ndev);
    if (ret)
        goto err_unmap_bar1;

    pr_info(DRV_NAME ": registered netdev %s\n", ndev->name);

    return 0;

err_unmap_bar1:
    pci_iounmap(pdev, mdev->bar1);
err_region1:
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
    netif_napi_del(&mdev->napi);
    free_netdev(ndev);
    return ret;
}

/* Remove */
static void minimal_remove(struct pci_dev *pdev)
{
    struct net_device *ndev = pci_get_drvdata(pdev);
    struct minimal_dev *mdev = netdev_priv(ndev);

    /*
     * Unregister first so no new traffic arrives while we tear down.
     * unregister_netdev() brings the interface down, invoking minimal_stop()
     * for any open interface, which frees the RX/TX DMA rings and buffers.
     */
    unregister_netdev(ndev);
    netif_napi_del(&mdev->napi);

    pr_info(DRV_NAME ": remove\n");

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
