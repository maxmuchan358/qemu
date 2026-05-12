/*
 * QEMU custom crashdump storage controller diagnostic device
 *
 * Simulates an NVMe-like storage controller register surface for crashdump
 * validation:
 * - Controller capabilities, version, configuration, and status (0x00-0x3c)
 * - Extended controller registers (0x40-0x7c)
 * - Queue doorbells and firmware scratchpad (0x200-0x27c)
 *
 * vendor=0x1d5f  device=0xd002
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_CUSTOM_STOR_DIAG "custom-crashdump-stor"

#define CUSTOM_STOR_VENDOR_ID   0x1d5f
#define CUSTOM_STOR_DEVICE_ID   0xd002

/* MMIO register offsets (BAR0, 1024 bytes) */
#define STOR_MMIO_CAP           0x000   /* controller capabilities */
#define STOR_MMIO_VS            0x004   /* version */
#define STOR_MMIO_INTMS         0x008   /* interrupt mask set */
#define STOR_MMIO_INTMC         0x00c   /* interrupt mask clear */
#define STOR_MMIO_CC            0x010   /* controller configuration */
#define STOR_MMIO_RSVD14        0x014
#define STOR_MMIO_CSTS          0x018   /* controller status */
#define STOR_MMIO_NSSR          0x01c   /* NVM subsystem reset */
#define STOR_MMIO_AQA           0x020   /* admin queue attributes */
#define STOR_MMIO_ASQ_LO        0x024   /* admin SQ base address lo */
#define STOR_MMIO_ASQ_HI        0x028   /* admin SQ base address hi */
#define STOR_MMIO_ACQ_LO        0x02c   /* admin CQ base address lo */
#define STOR_MMIO_ACQ_HI        0x030   /* admin CQ base address hi */
#define STOR_MMIO_CMBLOC        0x034   /* CMB location */
#define STOR_MMIO_CMBSZ         0x038   /* CMB size */
#define STOR_MMIO_BPINFO        0x03c   /* boot partition info */
#define STOR_MMIO_EXT_BASE      0x040   /* extended registers (16 dwords) */
#define STOR_MMIO_SCRATCHPAD    0x200   /* queue doorbells / scratchpad (32 dwords) */

#define STOR_BASE_REG_COUNT     16   /* 0x00-0x3c */
#define STOR_EXT_REG_COUNT      16   /* 0x40-0x7c */
#define STOR_SCRATCH_COUNT      32   /* 0x200-0x27c */
#define STOR_MMIO_SIZE          0x400

/* CAP field values */
#define STOR_CAP_MQES           63u           /* max queue entries: 64 */
#define STOR_CAP_CQR            (1u << 16)    /* contiguous queues required */
#define STOR_CAP_TO             (72u << 24)   /* timeout 72 * 500ms */
#define STOR_CAP_CSS_NVM        (1u << 6)

/* VS: version 1.4.0 */
#define STOR_VS_1_4_0           0x00010400u

/* CC fields */
#define STOR_CC_IOCQES_SHIFT    20
#define STOR_CC_IOSQES_SHIFT    16
#define STOR_CC_MPS_SHIFT       7
#define STOR_CC_CSS_NVM         0u
#define STOR_CC_DEFAULT \
    ((4u << STOR_CC_IOCQES_SHIFT) | (6u << STOR_CC_IOSQES_SHIFT))

/* CSTS fields */
#define STOR_CSTS_RDY           (1u << 0)
#define STOR_CSTS_CFS           (1u << 1)

typedef struct CustomStorDiagState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    uint32_t instance_id;

    /* Base registers (0x00-0x3c) */
    uint32_t cap;
    uint32_t vs;
    uint32_t intms;
    uint32_t intmc;
    uint32_t cc;
    uint32_t rsvd14;
    uint32_t csts;
    uint32_t nssr;
    uint32_t aqa;
    uint32_t asq_lo;
    uint32_t asq_hi;
    uint32_t acq_lo;
    uint32_t acq_hi;
    uint32_t cmbloc;
    uint32_t cmbsz;
    uint32_t bpinfo;

    /* Extended registers (0x40-0x7c) */
    uint32_t ext_regs[STOR_EXT_REG_COUNT];

    /* Queue doorbells / scratchpad (0x200-0x27c) */
    uint32_t scratchpad[STOR_SCRATCH_COUNT];
} CustomStorDiagState;

OBJECT_DECLARE_SIMPLE_TYPE(CustomStorDiagState, CUSTOM_STOR_DIAG)

static const Property custom_stor_properties[] = {
    DEFINE_PROP_UINT32("instance-id", CustomStorDiagState, instance_id, 0),
};

static void custom_stor_init_patterns(CustomStorDiagState *s, PCIDevice *pdev)
{
    uint32_t id = s->instance_id;
    unsigned int i;

    s->cap    = STOR_CAP_MQES | STOR_CAP_CQR | STOR_CAP_TO | STOR_CAP_CSS_NVM;
    s->vs     = STOR_VS_1_4_0;
    s->intms  = 0x00000000u;
    s->intmc  = 0x00000000u;
    s->cc     = STOR_CC_DEFAULT;
    s->rsvd14 = 0x00000000u;
    s->csts   = STOR_CSTS_RDY;       /* report as ready */
    s->nssr   = 0x00000000u;
    s->aqa    = 0x003f003fu;          /* ASQ=64, ACQ=64 */
    s->asq_lo = 0x00000000u;
    s->asq_hi = 0x00000000u;
    s->acq_lo = 0x00000000u;
    s->acq_hi = 0x00000000u;
    s->cmbloc = 0x00000000u;
    s->cmbsz  = 0x00000000u;
    s->bpinfo = 0x00000000u;

    for (i = 0; i < STOR_EXT_REG_COUNT; i++)
        s->ext_regs[i] = 0xE0020000u | (i << 8) | (id & 0xff);

    /* Scratchpad: simulate doorbell pattern (even=SQ tail, odd=CQ head) */
    for (i = 0; i < STOR_SCRATCH_COUNT; i++) {
        if (i % 2 == 0)
            s->scratchpad[i] = 0xAB000000u | (i << 8) | (id & 0xff);
        else
            s->scratchpad[i] = 0xCD000000u | (i << 8) | (id & 0xff);
    }

    /* PCI config space: write diagnostic pattern at offset 0x40+ */
    pci_set_long(&pdev->config[0x40], 0xD0020000u | id);
    pci_set_long(&pdev->config[0x44], CUSTOM_STOR_VENDOR_ID | (CUSTOM_STOR_DEVICE_ID << 16));
    pci_set_long(&pdev->config[0x48], id);
    pci_set_long(&pdev->config[0x4c], STOR_VS_1_4_0);
}

static uint32_t custom_stor_mmio_read32(CustomStorDiagState *s, hwaddr addr)
{
    unsigned int idx;

    switch (addr) {
    case STOR_MMIO_CAP:     return s->cap;
    case STOR_MMIO_VS:      return s->vs;
    case STOR_MMIO_INTMS:   return s->intms;
    case STOR_MMIO_INTMC:   return s->intmc;
    case STOR_MMIO_CC:      return s->cc;
    case STOR_MMIO_RSVD14:  return s->rsvd14;
    case STOR_MMIO_CSTS:    return s->csts;
    case STOR_MMIO_NSSR:    return s->nssr;
    case STOR_MMIO_AQA:     return s->aqa;
    case STOR_MMIO_ASQ_LO:  return s->asq_lo;
    case STOR_MMIO_ASQ_HI:  return s->asq_hi;
    case STOR_MMIO_ACQ_LO:  return s->acq_lo;
    case STOR_MMIO_ACQ_HI:  return s->acq_hi;
    case STOR_MMIO_CMBLOC:  return s->cmbloc;
    case STOR_MMIO_CMBSZ:   return s->cmbsz;
    case STOR_MMIO_BPINFO:  return s->bpinfo;
    default:
        break;
    }

    if (addr >= STOR_MMIO_EXT_BASE &&
        addr < STOR_MMIO_EXT_BASE + STOR_EXT_REG_COUNT * sizeof(uint32_t)) {
        idx = (addr - STOR_MMIO_EXT_BASE) >> 2;
        return s->ext_regs[idx];
    }

    if (addr >= STOR_MMIO_SCRATCHPAD &&
        addr < STOR_MMIO_SCRATCHPAD + STOR_SCRATCH_COUNT * sizeof(uint32_t)) {
        idx = (addr - STOR_MMIO_SCRATCHPAD) >> 2;
        return s->scratchpad[idx];
    }

    return 0xffffffff;
}

static void custom_stor_mmio_write32(CustomStorDiagState *s, hwaddr addr,
                                     uint32_t val)
{
    unsigned int idx;

    switch (addr) {
    case STOR_MMIO_CAP:     s->cap    = val; return;
    case STOR_MMIO_VS:      s->vs     = val; return;
    case STOR_MMIO_INTMS:   s->intms  = val; return;
    case STOR_MMIO_INTMC:   s->intmc  = val; return;
    case STOR_MMIO_CC:      s->cc     = val; return;
    case STOR_MMIO_RSVD14:  s->rsvd14 = val; return;
    case STOR_MMIO_CSTS:    s->csts   = val; return;
    case STOR_MMIO_NSSR:    s->nssr   = val; return;
    case STOR_MMIO_AQA:     s->aqa    = val; return;
    case STOR_MMIO_ASQ_LO:  s->asq_lo = val; return;
    case STOR_MMIO_ASQ_HI:  s->asq_hi = val; return;
    case STOR_MMIO_ACQ_LO:  s->acq_lo = val; return;
    case STOR_MMIO_ACQ_HI:  s->acq_hi = val; return;
    case STOR_MMIO_CMBLOC:  s->cmbloc = val; return;
    case STOR_MMIO_CMBSZ:   s->cmbsz  = val; return;
    case STOR_MMIO_BPINFO:  s->bpinfo = val; return;
    default:
        break;
    }

    if (addr >= STOR_MMIO_EXT_BASE &&
        addr < STOR_MMIO_EXT_BASE + STOR_EXT_REG_COUNT * sizeof(uint32_t)) {
        idx = (addr - STOR_MMIO_EXT_BASE) >> 2;
        s->ext_regs[idx] = val;
        return;
    }

    if (addr >= STOR_MMIO_SCRATCHPAD &&
        addr < STOR_MMIO_SCRATCHPAD + STOR_SCRATCH_COUNT * sizeof(uint32_t)) {
        idx = (addr - STOR_MMIO_SCRATCHPAD) >> 2;
        s->scratchpad[idx] = val;
    }
}

static uint64_t custom_stor_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    CustomStorDiagState *s = opaque;

    if (size != 4)
        return 0xffffffff;
    return custom_stor_mmio_read32(s, addr);
}

static void custom_stor_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    CustomStorDiagState *s = opaque;

    if (size != 4)
        return;
    custom_stor_mmio_write32(s, addr, (uint32_t)val);
}

static const MemoryRegionOps custom_stor_mmio_ops = {
    .read = custom_stor_mmio_read,
    .write = custom_stor_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl  = { .min_access_size = 4, .max_access_size = 4 },
};

static void custom_stor_realize(PCIDevice *pdev, Error **errp)
{
    CustomStorDiagState *s = CUSTOM_STOR_DIAG(pdev);

    pdev->config[PCI_INTERRUPT_PIN] = 0;
    custom_stor_init_patterns(s, pdev);

    memory_region_init_io(&s->mmio, OBJECT(s), &custom_stor_mmio_ops, s,
                          "custom-stor-mmio", STOR_MMIO_SIZE);

    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void custom_stor_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize   = custom_stor_realize;
    k->vendor_id = CUSTOM_STOR_VENDOR_ID;
    k->device_id = CUSTOM_STOR_DEVICE_ID;
    k->revision  = 0x1;
    k->class_id  = PCI_CLASS_STORAGE_EXPRESS;

    dc->desc = "Custom crashdump storage controller diagnostic device";
    device_class_set_props(dc, custom_stor_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo custom_stor_type_info = {
    .name          = TYPE_CUSTOM_STOR_DIAG,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(CustomStorDiagState),
    .class_init    = custom_stor_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void custom_stor_register_types(void)
{
    type_register_static(&custom_stor_type_info);
}

type_init(custom_stor_register_types)
