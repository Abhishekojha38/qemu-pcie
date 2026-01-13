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

/* Ring Configurations */
#define REG_RX_RING_BASE   0x10
#define REG_RX_RING_SIZE   0x18
#define REG_RX_TAIL        0x1C
#define REG_RX_HEAD        0x20

#define RX_RING_SIZE        16
#define RX_BUF_SIZE         2048
#define RX_DONE             1

/* QEMU NIC reads and writes exactly this layout using PCIe DMA */
struct rx_desc {
    u64 addr;   // where NIC must DMA the packet
    u16 len;    // length written by NIC
    u16 flags;  // DONE bit from NIC
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
};

static int minimal_open(struct net_device *ndev)
{
    netif_start_queue(ndev);
    return 0;
}

static int minimal_stop(struct net_device *ndev)
{
    netif_stop_queue(ndev);
    return 0;
}

static netdev_tx_t minimal_start_xmit(struct sk_buff *skb,
                                      struct net_device *ndev)
{
    /* Later you will DMA this into QEMU */
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
    int i;

    /*FIXME*/
    for (i = 0; i < RX_RING_SIZE; i++) {
        if (mdev->rx_ring[i].flags & RX_DONE) {
            pr_info("minimal_nic: RX packet len=%u\n",
                    mdev->rx_ring[i].len);

            /* mark buffer free again */
            mdev->rx_ring[i].flags = 0;
        }
    }
    return IRQ_HANDLED;
}

static int minimal_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
    struct minimal_dev *mdev;
    struct net_device *ndev;
    int ret, nvec, i;

    pr_info(DRV_NAME ": probe\n");

    ndev = alloc_etherdev(0);
    if (!ndev)
        return -ENOMEM;

    mdev = devm_kzalloc(&pdev->dev, sizeof(*mdev), GFP_KERNEL);
    if (!mdev)
        return -ENOMEM;

    mdev->pdev = pdev;
    pci_set_drvdata(pdev, mdev);

    mdev->netdev = ndev;
    ndev->netdev_ops = &minimal_netdev_ops;
    ndev->min_mtu = 68;
    ndev->max_mtu = 1500;

    eth_hw_addr_random(ndev);

    SET_NETDEV_DEV(ndev, &pdev->dev);

    ret = register_netdev(ndev);
    if (ret) {
        free_netdev(ndev);
        return ret;
    }

    pr_info(DRV_NAME ": registered netdev %s\n", ndev->name);



    /* Enable PCI device and bus-mastering */
    pr_info(DRV_NAME ": PCI enable device\n");
    ret = pci_enable_device(pdev);
    if (ret) {
        unregister_netdev(ndev);
        free_netdev(ndev);
        return ret;
    }

    pci_set_master(pdev);

#ifdef MSIX_ENABLE
    /* Request MSI-X vectors explicitly */
    mdev->nvec_irq = pci_alloc_irq_vectors(pdev,
                                           1,    // min vectors
                                           MAX_MSIX_VECTORS,    // max vectors
                                           PCI_IRQ_MSIX);
#else
    /* Fallback to MSI with per-vector masking */
    mdev->nvec_irq = pci_alloc_irq_vectors(pdev,
                                1,
                                MAX_MSI_VECTORS,
                                PCI_IRQ_MSI);
#endif

    if (mdev->nvec_irq < 0)
        return mdev->nvec_irq;

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
        goto err_region0;

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

    /* Here are 16 empty buffers of 2048 bytes each */
    for (i = 0; i < RX_RING_SIZE; i++) {
        mdev->rx_bufs[i] = dma_alloc_coherent(&pdev->dev,
                RX_BUF_SIZE,
                &mdev->rx_bufs_dma[i],
                GFP_KERNEL);

        mdev->rx_ring[i].addr = mdev->rx_bufs_dma[i];
        mdev->rx_ring[i].len = RX_BUF_SIZE;
        mdev->rx_ring[i].flags = 0;
    }

    /* Program device */
    writel(mdev->rx_ring_dma, mdev->bar0 + REG_RX_RING_BASE);
    writel(RX_RING_SIZE,     mdev->bar0 + REG_RX_RING_SIZE);
    writel(RX_RING_SIZE-1,   mdev->bar0 + REG_RX_TAIL);

    pr_info(DRV_NAME ": BAR0=%p BAR1=%p IRQ Vector Number=%d\n",
            mdev->bar0, mdev->bar1, mdev->nvec_irq);

    return 0;

err_region1:
    pci_release_region(pdev, 1);
err_region0:
    pci_iounmap(pdev, mdev->bar0);
    pci_release_region(pdev, 0);
err_irq:
    pci_free_irq_vectors(pdev);
err_disable:
    pci_disable_device(pdev);
    return ret;
}

/* Remove */
static void minimal_remove(struct pci_dev *pdev)
{
    struct minimal_dev *mdev = pci_get_drvdata(pdev);
    int i;

    if (mdev->netdev) {
        unregister_netdev(mdev->netdev);
        free_netdev(mdev->netdev);
    }
    pr_info(DRV_NAME ": remove\n");
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
