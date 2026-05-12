/*
 * QEMU custom crashdump NIC diagnostic device
 *
 * Simulates a NIC-style register surface for crashdump validation:
 * - MAC address registers
 * - Link status and PHY state
 * - Rx/Tx statistics counters
 * - Error counters
 * - Indirect index/data register pair (MMIO)
 * - I/O port command/status/DMA registers (BAR1)
 *
 * vendor=0x1d5f  device=0xd001
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_CUSTOM_NET_DIAG "custom-crashdump-net"

#define CUSTOM_NET_VENDOR_ID    0x1d5f
#define CUSTOM_NET_DEVICE_ID    0xd001

/* MMIO register offsets (BAR0, 512 bytes) */
#define NET_MMIO_MAC_LO         0x000
#define NET_MMIO_MAC_HI         0x004
#define NET_MMIO_LINK_STATUS    0x008
#define NET_MMIO_INT_STATUS     0x00c
#define NET_MMIO_RX_GOOD_LO     0x010
#define NET_MMIO_RX_GOOD_HI     0x014
#define NET_MMIO_RX_BYTES_LO    0x018
#define NET_MMIO_RX_BYTES_HI    0x01c
#define NET_MMIO_RX_ERRORS      0x020
#define NET_MMIO_RX_DROPS       0x024
#define NET_MMIO_RX_OVERRUNS    0x028
#define NET_MMIO_RX_FIFO_ERR    0x02c
#define NET_MMIO_TX_GOOD_LO     0x030
#define NET_MMIO_TX_GOOD_HI     0x034
#define NET_MMIO_TX_BYTES_LO    0x038
#define NET_MMIO_TX_BYTES_HI    0x03c
#define NET_MMIO_TX_ERRORS      0x040
#define NET_MMIO_TX_DROPS       0x044
#define NET_MMIO_TX_RETRIES     0x048
#define NET_MMIO_TX_COLLISION   0x04c
#define NET_MMIO_ERR_CRC        0x050
#define NET_MMIO_ERR_FRAME      0x054
#define NET_MMIO_ERR_OVERFLOW   0x058
#define NET_MMIO_ERR_UNDERFLOW  0x05c
#define NET_MMIO_CTRL           0x060
#define NET_MMIO_FW_VERSION     0x064
#define NET_MMIO_PHY_ID         0x068
#define NET_MMIO_PHY_STATUS     0x06c
#define NET_MMIO_IDX_SEL        0x100
#define NET_MMIO_IDX_DATA       0x104

#define NET_MMIO_DIRECT_COUNT   28   /* dwords 0x000-0x06c */
#define NET_IDX_COUNT           8
#define NET_MMIO_SIZE           0x200
#define NET_PIO_SIZE            0x40
#define NET_PIO_COUNT           16

/* link_status bit definitions */
#define NET_LINK_SPEED_1G       1000
#define NET_LINK_UP             (1u << 16)
#define NET_LINK_FULL_DUPLEX    (1u << 17)

/* ctrl bit definitions */
#define NET_CTRL_ENABLE         (1u << 0)
#define NET_CTRL_PROMISC        (1u << 1)
#define NET_CTRL_LOOPBACK       (1u << 2)

typedef struct CustomNetDiagState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion pio;
    uint32_t instance_id;

    /* Direct MMIO registers (0x000-0x06c) */
    uint32_t mac_lo;
    uint32_t mac_hi;
    uint32_t link_status;
    uint32_t int_status;
    uint32_t rx_good_lo;
    uint32_t rx_good_hi;
    uint32_t rx_bytes_lo;
    uint32_t rx_bytes_hi;
    uint32_t rx_errors;
    uint32_t rx_drops;
    uint32_t rx_overruns;
    uint32_t rx_fifo_err;
    uint32_t tx_good_lo;
    uint32_t tx_good_hi;
    uint32_t tx_bytes_lo;
    uint32_t tx_bytes_hi;
    uint32_t tx_errors;
    uint32_t tx_drops;
    uint32_t tx_retries;
    uint32_t tx_collision;
    uint32_t err_crc;
    uint32_t err_frame;
    uint32_t err_overflow;
    uint32_t err_underflow;
    uint32_t ctrl;
    uint32_t fw_version;
    uint32_t phy_id;
    uint32_t phy_status;

    /* Indirect registers (accessed via IDX_SEL/IDX_DATA at 0x100/0x104) */
    uint32_t idx_sel;
    uint32_t idx_data[NET_IDX_COUNT];

    /* PIO registers (BAR1, 64 bytes) */
    uint32_t pio_regs[NET_PIO_COUNT];
} CustomNetDiagState;

OBJECT_DECLARE_SIMPLE_TYPE(CustomNetDiagState, CUSTOM_NET_DIAG)

static const Property custom_net_properties[] = {
    DEFINE_PROP_UINT32("instance-id", CustomNetDiagState, instance_id, 0),
};

static void custom_net_init_patterns(CustomNetDiagState *s, PCIDevice *pdev)
{
    uint32_t id = s->instance_id;
    unsigned int i;

    /* MAC: locally administered, instance-specific */
    s->mac_lo        = 0xAABBCC00u | (id & 0xff);
    s->mac_hi        = 0x0000DD00u | ((id >> 8) & 0xff);
    s->link_status   = NET_LINK_UP | NET_LINK_FULL_DUPLEX | NET_LINK_SPEED_1G;
    s->int_status    = 0x00000000u;

    /* Simulated packet counts - distinctive patterns */
    s->rx_good_lo    = 0xC0010000u | id;
    s->rx_good_hi    = 0x00000000u;
    s->rx_bytes_lo   = 0xC0020000u | id;
    s->rx_bytes_hi   = 0x00000001u;
    s->rx_errors     = 0x00000000u;
    s->rx_drops      = 0x00000000u;
    s->rx_overruns   = 0x00000000u;
    s->rx_fifo_err   = 0x00000000u;
    s->tx_good_lo    = 0xC0030000u | id;
    s->tx_good_hi    = 0x00000000u;
    s->tx_bytes_lo   = 0xC0040000u | id;
    s->tx_bytes_hi   = 0x00000000u;
    s->tx_errors     = 0x00000000u;
    s->tx_drops      = 0x00000000u;
    s->tx_retries    = 0x00000000u;
    s->tx_collision  = 0x00000000u;
    s->err_crc       = 0x00000000u;
    s->err_frame     = 0x00000000u;
    s->err_overflow  = 0x00000000u;
    s->err_underflow = 0x00000000u;
    s->ctrl          = NET_CTRL_ENABLE;
    s->fw_version    = 0x01020300u | (id & 0xff);   /* v1.2.3.id */
    s->phy_id        = 0xBCBC0000u | id;
    s->phy_status    = 0x00000314u;  /* link ok, full-duplex, 1G */

    s->idx_sel = 0;
    for (i = 0; i < NET_IDX_COUNT; i++)
        s->idx_data[i] = 0xDDDD0000u | (i << 8) | (id & 0xff);

    /* PIO: cmd=idle, status=ready, rest are diagnostic patterns */
    s->pio_regs[0] = 0x00000000u;           /* cmd: idle */
    s->pio_regs[1] = 0x00000001u;           /* status: ready */
    s->pio_regs[2] = 0x00000000u;           /* dma_lo */
    s->pio_regs[3] = 0x00000000u;           /* dma_hi */
    s->pio_regs[4] = 0x00000000u;           /* desc_head */
    s->pio_regs[5] = 0x00000000u;           /* desc_tail */
    for (i = 6; i < NET_PIO_COUNT; i++)
        s->pio_regs[i] = 0xF00D0000u | (i << 8) | (id & 0xff);

    /* PCI config space: write diagnostic pattern at offset 0x40+ */
    pci_set_long(&pdev->config[0x40], 0xD0010000u | id);
    pci_set_long(&pdev->config[0x44], CUSTOM_NET_VENDOR_ID | (CUSTOM_NET_DEVICE_ID << 16));
    pci_set_long(&pdev->config[0x48], id);
}

static uint32_t *custom_net_mmio_reg(CustomNetDiagState *s, hwaddr addr)
{
    switch (addr) {
    case NET_MMIO_MAC_LO:        return &s->mac_lo;
    case NET_MMIO_MAC_HI:        return &s->mac_hi;
    case NET_MMIO_LINK_STATUS:   return &s->link_status;
    case NET_MMIO_INT_STATUS:    return &s->int_status;
    case NET_MMIO_RX_GOOD_LO:    return &s->rx_good_lo;
    case NET_MMIO_RX_GOOD_HI:    return &s->rx_good_hi;
    case NET_MMIO_RX_BYTES_LO:   return &s->rx_bytes_lo;
    case NET_MMIO_RX_BYTES_HI:   return &s->rx_bytes_hi;
    case NET_MMIO_RX_ERRORS:     return &s->rx_errors;
    case NET_MMIO_RX_DROPS:      return &s->rx_drops;
    case NET_MMIO_RX_OVERRUNS:   return &s->rx_overruns;
    case NET_MMIO_RX_FIFO_ERR:   return &s->rx_fifo_err;
    case NET_MMIO_TX_GOOD_LO:    return &s->tx_good_lo;
    case NET_MMIO_TX_GOOD_HI:    return &s->tx_good_hi;
    case NET_MMIO_TX_BYTES_LO:   return &s->tx_bytes_lo;
    case NET_MMIO_TX_BYTES_HI:   return &s->tx_bytes_hi;
    case NET_MMIO_TX_ERRORS:     return &s->tx_errors;
    case NET_MMIO_TX_DROPS:      return &s->tx_drops;
    case NET_MMIO_TX_RETRIES:    return &s->tx_retries;
    case NET_MMIO_TX_COLLISION:  return &s->tx_collision;
    case NET_MMIO_ERR_CRC:       return &s->err_crc;
    case NET_MMIO_ERR_FRAME:     return &s->err_frame;
    case NET_MMIO_ERR_OVERFLOW:  return &s->err_overflow;
    case NET_MMIO_ERR_UNDERFLOW: return &s->err_underflow;
    case NET_MMIO_CTRL:          return &s->ctrl;
    case NET_MMIO_FW_VERSION:    return &s->fw_version;
    case NET_MMIO_PHY_ID:        return &s->phy_id;
    case NET_MMIO_PHY_STATUS:    return &s->phy_status;
    case NET_MMIO_IDX_SEL:       return &s->idx_sel;
    default:                     return NULL;
    }
}

static uint64_t custom_net_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    CustomNetDiagState *s = opaque;
    uint32_t *reg;

    if (size != 4)
        return 0xffffffff;

    if (addr == NET_MMIO_IDX_DATA)
        return s->idx_data[s->idx_sel % NET_IDX_COUNT];

    reg = custom_net_mmio_reg(s, addr);
    return reg ? *reg : 0xffffffff;
}

static void custom_net_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    CustomNetDiagState *s = opaque;
    uint32_t *reg;

    if (size != 4)
        return;

    if (addr == NET_MMIO_IDX_DATA) {
        s->idx_data[s->idx_sel % NET_IDX_COUNT] = (uint32_t)val;
        return;
    }

    reg = custom_net_mmio_reg(s, addr);
    if (reg)
        *reg = (uint32_t)val;
}

static uint64_t custom_net_pio_read(void *opaque, hwaddr addr, unsigned size)
{
    CustomNetDiagState *s = opaque;
    unsigned int index;

    if (size != 4)
        return 0xffffffff;

    if (addr < NET_PIO_SIZE) {
        index = addr >> 2;
        return s->pio_regs[index];
    }

    return 0xffffffff;
}

static void custom_net_pio_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    CustomNetDiagState *s = opaque;
    unsigned int index;

    if (size != 4)
        return;

    if (addr < NET_PIO_SIZE) {
        index = addr >> 2;
        s->pio_regs[index] = (uint32_t)val;
    }
}

static const MemoryRegionOps custom_net_mmio_ops = {
    .read = custom_net_mmio_read,
    .write = custom_net_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl  = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps custom_net_pio_ops = {
    .read = custom_net_pio_read,
    .write = custom_net_pio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl  = { .min_access_size = 4, .max_access_size = 4 },
};

static void custom_net_realize(PCIDevice *pdev, Error **errp)
{
    CustomNetDiagState *s = CUSTOM_NET_DIAG(pdev);

    pdev->config[PCI_INTERRUPT_PIN] = 0;
    custom_net_init_patterns(s, pdev);

    memory_region_init_io(&s->mmio, OBJECT(s), &custom_net_mmio_ops, s,
                          "custom-net-mmio", NET_MMIO_SIZE);
    memory_region_init_io(&s->pio, OBJECT(s), &custom_net_pio_ops, s,
                          "custom-net-pio", NET_PIO_SIZE);

    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->pio);
}

static void custom_net_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize   = custom_net_realize;
    k->vendor_id = CUSTOM_NET_VENDOR_ID;
    k->device_id = CUSTOM_NET_DEVICE_ID;
    k->revision  = 0x1;
    k->class_id  = PCI_CLASS_NETWORK_ETHERNET;

    dc->desc = "Custom crashdump NIC diagnostic device";
    device_class_set_props(dc, custom_net_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo custom_net_type_info = {
    .name          = TYPE_CUSTOM_NET_DIAG,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(CustomNetDiagState),
    .class_init    = custom_net_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void custom_net_register_types(void)
{
    type_register_static(&custom_net_type_info);
}

type_init(custom_net_register_types)
