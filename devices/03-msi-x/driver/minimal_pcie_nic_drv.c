// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/io.h>

#define DRV_NAME "minimal_pcie_nic_drv"
#define VENDOR_ID 0x1af4
#define DEVICE_ID 0x10f1

struct minimal_dev {
    struct pci_dev *pdev;
    void __iomem *bar0;    // MMIO registers
    void __iomem *bar1;    // MSI-X table/PBA (optional mapping)
    int irq;
};

static irqreturn_t minimal_irq_handler(int irq, void *dev_id)
{
    struct minimal_dev *mdev = dev_id;

    pr_info(DRV_NAME ": MSI-X interrupt received\n");

    /* Acknowledge device here if needed (BAR0 register write) */
    return IRQ_HANDLED;
}

static int minimal_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
    struct minimal_dev *mdev;
    int ret;

    pr_info(DRV_NAME ": probe\n");

    mdev = devm_kzalloc(&pdev->dev, sizeof(*mdev), GFP_KERNEL);
    if (!mdev)
        return -ENOMEM;

    mdev->pdev = pdev;
    pci_set_drvdata(pdev, mdev);

    /* Enable PCI device and bus-mastering */
    ret = pci_enable_device(pdev);
    if (ret)
        return ret;

    pci_set_master(pdev);

    /* Enable MSI-X (1 vector) */
    ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
    if (ret < 0) {
        dev_err(&pdev->dev, "MSI-X enable failed\n");
        goto err_disable;
    }

    mdev->irq = pci_irq_vector(pdev, 0);

    /* Request IRQ */
    ret = devm_request_irq(&pdev->dev, mdev->irq,
                           minimal_irq_handler,
                           0, DRV_NAME, mdev);
    if (ret) {
        dev_err(&pdev->dev, "IRQ request failed\n");
        goto err_irq;
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

    pr_info(DRV_NAME ": BAR0=%p BAR1=%p IRQ=%d\n",
            mdev->bar0, mdev->bar1, mdev->irq);

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

    pr_info(DRV_NAME ": remove\n");

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
MODULE_DESCRIPTION("Minimal PCIe NIC driver with MSI-X");
MODULE_LICENSE("GPL");
