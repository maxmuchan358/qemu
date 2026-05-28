/*
 * QEMU custom crashdump storage verification device
 *
 * Returns deterministic bytes for the PCI config and MMIO regions dumped by
 * the kdump table for the custom storage device.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_CUSTOM_STOR_DIAG "custom-crashdump-stor"

#define CUSTOM_STOR_VENDOR_ID   0x1d5f
#define CUSTOM_STOR_DEVICE_ID   0xd002

#define CUSTOM_STOR_MMIO_SIZE 0x400

#define CUSTOM_STOR_CFG_OFFSET 0x40
#define CUSTOM_STOR_CFG_SIZE   (PCI_CONFIG_SPACE_SIZE - CUSTOM_STOR_CFG_OFFSET)

typedef struct CustomStorDiagState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    uint32_t instance_id;
    uint8_t mmio_data[CUSTOM_STOR_MMIO_SIZE];
} CustomStorDiagState;

OBJECT_DECLARE_SIMPLE_TYPE(CustomStorDiagState, CUSTOM_STOR_DIAG)

static uint32_t kdmp_pattern_signature(char device_tag, char space_tag,
                                       uint32_t salt)
{
    return 0x4b44554dU ^ ((uint32_t)(uint8_t)device_tag << 24)
           ^ ((uint32_t)(uint8_t)space_tag << 16) ^ salt;
}

static uint8_t kdmp_pattern_byte(char device_tag, char space_tag,
                                 uint32_t variant, uint32_t salt,
                                 uint32_t offset)
{
    if (offset == 0) {
        return 'K';
    }
    if (offset == 1) {
        return 'D';
    }
    if (offset == 2) {
        return 'M';
    }
    if (offset == 3) {
        return 'P';
    }
    if (offset == 4) {
        return (uint8_t)device_tag;
    }
    if (offset == 5) {
        return (uint8_t)space_tag;
    }
    if (offset == 6) {
        return variant & 0xff;
    }
    if (offset == 7) {
        return (variant >> 8) & 0xff;
    }

    return (uint8_t)(((offset * 0x3dU) ^ (offset >> 8)
                      ^ (uint8_t)device_tag ^ ((uint8_t)space_tag << 1)
                      ^ variant ^ salt) & 0xff);
}

static const Property custom_stor_properties[] = {
    DEFINE_PROP_UINT32("instance-id", CustomStorDiagState, instance_id, 0),
};

static void kdmp_fill_region(uint8_t *buf, size_t len, char device_tag,
                             char space_tag, uint32_t variant, uint32_t salt)
{
    size_t offset;

    memset(buf, 0, len);
    for (offset = 0; offset < len; offset++) {
        buf[offset] = kdmp_pattern_byte(device_tag, space_tag,
                                        variant, salt, offset);
    }

    if (len >= 12) {
        stl_le_p(buf + 8, len);
    }
    if (len >= 16) {
        stl_le_p(buf + 12, kdmp_pattern_signature(device_tag, space_tag,
                                                 salt));
    }
}

static uint64_t kdmp_region_read(const uint8_t *buf, size_t len,
                                 hwaddr addr, unsigned size)
{
    uint64_t value = 0;
    unsigned int index;

    if (size != 1 && size != 2 && size != 4) {
        return UINT64_MAX;
    }
    if (addr > len || size > len - addr) {
        return UINT64_MAX;
    }

    for (index = 0; index < size; index++) {
        value |= (uint64_t)buf[addr + index] << (index * 8);
    }

    return value;
}

static void kdmp_region_write(uint8_t *buf, size_t len, hwaddr addr,
                              uint64_t value, unsigned size)
{
    unsigned int index;

    if (size != 1 && size != 2 && size != 4) {
        return;
    }
    if (addr > len || size > len - addr) {
        return;
    }

    for (index = 0; index < size; index++) {
        buf[addr + index] = (value >> (index * 8)) & 0xff;
    }
}

static void custom_stor_init_patterns(CustomStorDiagState *s, PCIDevice *pdev)
{
    kdmp_fill_region(&pdev->config[CUSTOM_STOR_CFG_OFFSET], CUSTOM_STOR_CFG_SIZE,
                     'S', 'C', s->instance_id, 0);
    kdmp_fill_region(s->mmio_data, sizeof(s->mmio_data), 'S', 'M',
                     s->instance_id, 0);
}

static uint64_t custom_stor_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    CustomStorDiagState *s = opaque;

    return kdmp_region_read(s->mmio_data, sizeof(s->mmio_data), addr, size);
}

static void custom_stor_mmio_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    CustomStorDiagState *s = opaque;

    kdmp_region_write(s->mmio_data, sizeof(s->mmio_data), addr, value, size);
}

static const MemoryRegionOps custom_stor_mmio_ops = {
    .read = custom_stor_mmio_read,
    .write = custom_stor_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl  = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

static void custom_stor_realize(PCIDevice *pdev, Error **errp)
{
    CustomStorDiagState *s = CUSTOM_STOR_DIAG(pdev);

    pdev->config[PCI_INTERRUPT_PIN] = 0;
    custom_stor_init_patterns(s, pdev);

    memory_region_init_io(&s->mmio, OBJECT(s), &custom_stor_mmio_ops, s,
                          "custom-stor-mmio", CUSTOM_STOR_MMIO_SIZE);

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
