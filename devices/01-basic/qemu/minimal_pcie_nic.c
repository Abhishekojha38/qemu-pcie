/*
 * minimal_pcie_nic.c
 *
 * Minimal skeleton PCIe NIC-like device for QEMU 9.2.0
 *
 * - Shows up in PCI enumeration as a Network controller
 * - No BARs, no MMIO, no interrupts
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "qapi/error.h"

/* Type name */
#define TYPE_MINIMAL_PCIE_NIC "minimal-pcie-nic"

OBJECT_DECLARE_SIMPLE_TYPE(MinimalPCIeNICState, MINIMAL_PCIE_NIC)

typedef struct MinimalPCIeNICState {
    PCIDevice parent_obj;
    /* Add device state here when needed */
} MinimalPCIeNICState;


/* Realize: called when a device instance is created */
static void minimal_pcie_nic_realize(PCIDevice *pdev, Error **errp)
{
    /* Host-side log */
   printf("minimal_pcie_nic: realize called (host log)");

    /* Set vendor/device IDs (example IDs) */
    pci_config_set_vendor_id(pdev->config, 0x1234);
    pci_config_set_device_id(pdev->config, 0x11E8);

    /* Set class to Network controller (Ethernet) */
    pci_config_set_class(pdev->config, PCI_CLASS_NETWORK_ETHERNET);

    /* No BARs or MMIO regions yet */
}

/* Class init: set class-level callbacks and metadata */
static void minimal_pcie_nic_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = minimal_pcie_nic_realize;
    k->vendor_id = 0x1234;
    k->device_id = 0x11E8;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;

}

static void minimal_pcie_nic_register_types(void)
{
    static const InterfaceInfo interfaces[] = {
        { INTERFACE_PCIE_DEVICE },
        { }
    };

    /* Type info and registration */
    static const TypeInfo minimal_pcie_nic_info = {
        .name          = TYPE_MINIMAL_PCIE_NIC,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(MinimalPCIeNICState),
        .class_init    = minimal_pcie_nic_class_init,
        .interfaces    = interfaces,
    };

    type_register_static(&minimal_pcie_nic_info);
}

type_init(minimal_pcie_nic_register_types);
