/*
 * QEMU custom crashdump test PCI device
 *
 * Provides a stable register surface for crashdump validation:
 * - Full PCI config space patterns
 * - BAR0 MMIO register bank
 * - BAR1 I/O port register bank
 * - MMIO and PIO index/data pairs
 * - MMIO table registers
 * - Queue/mailbox style live registers
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_CUSTOM_CRASHDUMP_TEST "custom-crashdump-test"

#define CUSTOM_VENDOR_ID 0x1d5f
#define CUSTOM_DEVICE_ID 0xcafe

#define CUSTOM_CFG40_VALUE  0xc001d00dU
#define CUSTOM_MMIO_VALUE   0xa55a5aa5U
#define CUSTOM_IOPORT_VALUE 0x5aa55aa5U

#define CUSTOM_MMIO_SIZE            0x400
#define CUSTOM_PIO_SIZE             0x80
#define CUSTOM_MMIO_REG_COUNT       64
#define CUSTOM_PIO_REG_COUNT        16
#define CUSTOM_MMIO_INDEX_COUNT     32
#define CUSTOM_PIO_INDEX_COUNT      16
#define CUSTOM_MMIO_TABLE_COUNT     32
#define CUSTOM_QUEUE_REG_COUNT      8

#define CUSTOM_MMIO_INDEX_SEL       0x100
#define CUSTOM_MMIO_INDEX_DATA      0x104
#define CUSTOM_MMIO_TABLE_BASE      0x200
#define CUSTOM_MMIO_QUEUE_BASE      0x300

#define CUSTOM_PIO_INDEX_SEL        0x40
#define CUSTOM_PIO_INDEX_DATA       0x44

#define CUSTOM_CFG_PATTERN_BASE     0xc001d000U
#define CUSTOM_MMIO_PATTERN_BASE    0xa55a5000U
#define CUSTOM_PIO_PATTERN_BASE     0x5aa55000U
#define CUSTOM_MMIO_INDEX_BASE      0xface1000U
#define CUSTOM_PIO_INDEX_BASE       0xbeef2000U
#define CUSTOM_MMIO_TABLE_BASEVAL   0x13570000U
#define CUSTOM_QUEUE_BASEVAL        0x24680000U

#define CUSTOM_PROFILE_GENERIC      0U

typedef struct CustomPatternConfig {
    uint32_t cfg40_value;
    uint32_t mmio_value;
    uint32_t ioport_value;
    uint32_t cfg_pattern_base;
    uint32_t mmio_pattern_base;
    uint32_t pio_pattern_base;
    uint32_t mmio_index_base;
    uint32_t pio_index_base;
    uint32_t mmio_table_base;
    uint32_t queue_base;
} CustomPatternConfig;

typedef struct CustomCrashdumpTestState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion pio;
    uint32_t profile;
    uint32_t instance_id;
    uint32_t mmio_regs[CUSTOM_MMIO_REG_COUNT];
    uint32_t pio_regs[CUSTOM_PIO_REG_COUNT];
    uint32_t mmio_indexed[CUSTOM_MMIO_INDEX_COUNT];
    uint32_t pio_indexed[CUSTOM_PIO_INDEX_COUNT];
    uint32_t mmio_table[CUSTOM_MMIO_TABLE_COUNT];
    uint32_t queue_regs[CUSTOM_QUEUE_REG_COUNT];
    uint32_t mmio_index;
    uint32_t pio_index;
} CustomCrashdumpTestState;

OBJECT_DECLARE_SIMPLE_TYPE(CustomCrashdumpTestState, CUSTOM_CRASHDUMP_TEST)

static const Property custom_crashdump_properties[] = {
    DEFINE_PROP_UINT32("profile", CustomCrashdumpTestState, profile,
                       CUSTOM_PROFILE_GENERIC),
    DEFINE_PROP_UINT32("instance-id", CustomCrashdumpTestState, instance_id, 0),
};

static CustomPatternConfig custom_pattern_config(const CustomCrashdumpTestState *s)
{
    uint32_t profile_offset = s->profile << 16;
    uint32_t instance_offset = s->instance_id << 8;

    return (CustomPatternConfig) {
        .cfg40_value = CUSTOM_CFG40_VALUE + profile_offset + instance_offset,
        .mmio_value = CUSTOM_MMIO_VALUE + profile_offset + instance_offset,
        .ioport_value = CUSTOM_IOPORT_VALUE + profile_offset + instance_offset,
        .cfg_pattern_base = CUSTOM_CFG_PATTERN_BASE + profile_offset + instance_offset,
        .mmio_pattern_base = CUSTOM_MMIO_PATTERN_BASE + profile_offset + instance_offset,
        .pio_pattern_base = CUSTOM_PIO_PATTERN_BASE + profile_offset + instance_offset,
        .mmio_index_base = CUSTOM_MMIO_INDEX_BASE + profile_offset + instance_offset,
        .pio_index_base = CUSTOM_PIO_INDEX_BASE + profile_offset + instance_offset,
        .mmio_table_base = CUSTOM_MMIO_TABLE_BASEVAL + profile_offset + instance_offset,
        .queue_base = CUSTOM_QUEUE_BASEVAL + profile_offset + instance_offset,
    };
}

static void custom_crashdump_init_patterns(CustomCrashdumpTestState *s,
                                           PCIDevice *pdev)
{
    unsigned int index;
    CustomPatternConfig cfg = custom_pattern_config(s);

    for (index = 0; index < CUSTOM_MMIO_REG_COUNT; index++) {
        s->mmio_regs[index] = cfg.mmio_pattern_base + (index * 0x111U);
    }
    s->mmio_regs[0] = cfg.mmio_value;

    for (index = 0; index < CUSTOM_PIO_REG_COUNT; index++) {
        s->pio_regs[index] = cfg.pio_pattern_base + (index * 0x101U);
    }
    s->pio_regs[0] = cfg.ioport_value;

    for (index = 0; index < CUSTOM_MMIO_INDEX_COUNT; index++) {
        s->mmio_indexed[index] = cfg.mmio_index_base + (index * 0x21U);
    }

    for (index = 0; index < CUSTOM_PIO_INDEX_COUNT; index++) {
        s->pio_indexed[index] = cfg.pio_index_base + (index * 0x31U);
    }

    for (index = 0; index < CUSTOM_MMIO_TABLE_COUNT; index++) {
        s->mmio_table[index] = cfg.mmio_table_base + (index * 0x41U);
    }

    for (index = 0; index < CUSTOM_QUEUE_REG_COUNT; index++) {
        s->queue_regs[index] = cfg.queue_base + (index * 0x51U);
    }

    for (index = 0x40; index < 0x100; index += sizeof(uint32_t)) {
        pci_set_long(&pdev->config[index], cfg.cfg_pattern_base + index);
    }
    pci_set_long(&pdev->config[0x40], cfg.cfg40_value);
    pci_set_long(&pdev->config[0x44], s->profile);
    pci_set_long(&pdev->config[0x48], s->instance_id);
    pci_set_long(&pdev->config[0x4c], (s->profile << 16) | (s->instance_id & 0xffffU));
}

static uint64_t custom_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    CustomCrashdumpTestState *s = opaque;
    unsigned int index;

    if (size != 4) {
        return 0xffffffff;
    }

    if (addr < (CUSTOM_MMIO_REG_COUNT * sizeof(uint32_t))) {
        index = addr >> 2;
        return s->mmio_regs[index];
    }

    if (addr == CUSTOM_MMIO_INDEX_SEL) {
        return s->mmio_index;
    }

    if (addr == CUSTOM_MMIO_INDEX_DATA) {
        return s->mmio_indexed[s->mmio_index % CUSTOM_MMIO_INDEX_COUNT];
    }

    if (addr >= CUSTOM_MMIO_TABLE_BASE &&
        addr < CUSTOM_MMIO_TABLE_BASE + (CUSTOM_MMIO_TABLE_COUNT * sizeof(uint32_t))) {
        index = (addr - CUSTOM_MMIO_TABLE_BASE) >> 2;
        return s->mmio_table[index];
    }

    if (addr >= CUSTOM_MMIO_QUEUE_BASE &&
        addr < CUSTOM_MMIO_QUEUE_BASE + (CUSTOM_QUEUE_REG_COUNT * sizeof(uint32_t))) {
        index = (addr - CUSTOM_MMIO_QUEUE_BASE) >> 2;
        return s->queue_regs[index];
    }

    return 0xffffffff;
}

static void custom_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    CustomCrashdumpTestState *s = opaque;
    unsigned int index;

    if (size != 4) {
        return;
    }

    if (addr < (CUSTOM_MMIO_REG_COUNT * sizeof(uint32_t))) {
        index = addr >> 2;
        s->mmio_regs[index] = (uint32_t)val;
        return;
    }

    if (addr == CUSTOM_MMIO_INDEX_SEL) {
        s->mmio_index = (uint32_t)val;
        return;
    }

    if (addr == CUSTOM_MMIO_INDEX_DATA) {
        s->mmio_indexed[s->mmio_index % CUSTOM_MMIO_INDEX_COUNT] = (uint32_t)val;
        return;
    }

    if (addr >= CUSTOM_MMIO_TABLE_BASE &&
        addr < CUSTOM_MMIO_TABLE_BASE + (CUSTOM_MMIO_TABLE_COUNT * sizeof(uint32_t))) {
        index = (addr - CUSTOM_MMIO_TABLE_BASE) >> 2;
        s->mmio_table[index] = (uint32_t)val;
        return;
    }

    if (addr >= CUSTOM_MMIO_QUEUE_BASE &&
        addr < CUSTOM_MMIO_QUEUE_BASE + (CUSTOM_QUEUE_REG_COUNT * sizeof(uint32_t))) {
        index = (addr - CUSTOM_MMIO_QUEUE_BASE) >> 2;
        s->queue_regs[index] = (uint32_t)val;
    }
}

static uint64_t custom_pio_read(void *opaque, hwaddr addr, unsigned size)
{
    CustomCrashdumpTestState *s = opaque;
    unsigned int index;

    if (size != 4) {
        return 0xffffffff;
    }

    if (addr < (CUSTOM_PIO_REG_COUNT * sizeof(uint32_t))) {
        index = addr >> 2;
        return s->pio_regs[index];
    }

    if (addr == CUSTOM_PIO_INDEX_SEL) {
        return s->pio_index;
    }

    if (addr == CUSTOM_PIO_INDEX_DATA) {
        return s->pio_indexed[s->pio_index % CUSTOM_PIO_INDEX_COUNT];
    }

    return 0xffffffff;
}

static void custom_pio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    CustomCrashdumpTestState *s = opaque;
    unsigned int index;

    if (size != 4) {
        return;
    }

    if (addr < (CUSTOM_PIO_REG_COUNT * sizeof(uint32_t))) {
        index = addr >> 2;
        s->pio_regs[index] = (uint32_t)val;
        return;
    }

    if (addr == CUSTOM_PIO_INDEX_SEL) {
        s->pio_index = (uint32_t)val;
        return;
    }

    if (addr == CUSTOM_PIO_INDEX_DATA) {
        s->pio_indexed[s->pio_index % CUSTOM_PIO_INDEX_COUNT] = (uint32_t)val;
    }
}

static const MemoryRegionOps custom_mmio_ops = {
    .read = custom_mmio_read,
    .write = custom_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps custom_pio_ops = {
    .read = custom_pio_read,
    .write = custom_pio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void custom_crashdump_realize(PCIDevice *pdev, Error **errp)
{
    CustomCrashdumpTestState *s = CUSTOM_CRASHDUMP_TEST(pdev);

    pdev->config[PCI_INTERRUPT_PIN] = 0;

    custom_crashdump_init_patterns(s, pdev);

    memory_region_init_io(&s->mmio, OBJECT(s), &custom_mmio_ops, s,
                          "custom-crashdump-mmio", CUSTOM_MMIO_SIZE);
    memory_region_init_io(&s->pio, OBJECT(s), &custom_pio_ops, s,
                          "custom-crashdump-pio", CUSTOM_PIO_SIZE);

    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->pio);
}

static void custom_crashdump_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = custom_crashdump_realize;
    k->vendor_id = CUSTOM_VENDOR_ID;
    k->device_id = CUSTOM_DEVICE_ID;
    k->revision = 0x1;
    k->class_id = PCI_CLASS_OTHERS;
    
    dc->desc = "Custom crashdump register test device";
    device_class_set_props(dc, custom_crashdump_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo custom_crashdump_type_info = {
    .name = TYPE_CUSTOM_CRASHDUMP_TEST,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(CustomCrashdumpTestState),
    .class_init = custom_crashdump_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void custom_crashdump_register_types(void)
{
    type_register_static(&custom_crashdump_type_info);
}

type_init(custom_crashdump_register_types)
