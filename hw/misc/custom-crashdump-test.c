/*
 * QEMU custom crashdump test PCI device
 *
 * Provides representative register access paths for crashdump testing:
 * - PCI config dword (offset 0x40)
 * - BAR0 MMIO dword (offset 0x0)
 * - BAR1 I/O port dword (offset 0x0)
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_CUSTOM_CRASHDUMP_TEST "custom-crashdump-test"

#define CUSTOM_VENDOR_ID 0x1d5f
#define CUSTOM_DEVICE_ID 0xcafe

#define CUSTOM_CFG40_VALUE  0xc001d00dU
#define CUSTOM_MMIO_VALUE   0xa55a5aa5U
#define CUSTOM_IOPORT_VALUE 0x5aa55aa5U

typedef struct CustomCrashdumpTestState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion pio;
    uint32_t mmio_val;
    uint32_t pio_val;
} CustomCrashdumpTestState;

OBJECT_DECLARE_SIMPLE_TYPE(CustomCrashdumpTestState, CUSTOM_CRASHDUMP_TEST)

static uint64_t custom_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    CustomCrashdumpTestState *s = opaque;

    if (addr == 0 && size == 4) {
        return s->mmio_val;
    }

    return 0xffffffff;
}

static void custom_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    CustomCrashdumpTestState *s = opaque;

    if (addr == 0 && size == 4) {
        s->mmio_val = (uint32_t)val;
    }
}

static uint64_t custom_pio_read(void *opaque, hwaddr addr, unsigned size)
{
    CustomCrashdumpTestState *s = opaque;

    if (addr == 0 && size == 4) {
        return s->pio_val;
    }

    return 0xffffffff;
}

static void custom_pio_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    CustomCrashdumpTestState *s = opaque;

    if (addr == 0 && size == 4) {
        s->pio_val = (uint32_t)val;
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

    s->mmio_val = CUSTOM_MMIO_VALUE;
    s->pio_val = CUSTOM_IOPORT_VALUE;

    pci_set_long(&pdev->config[0x40], CUSTOM_CFG40_VALUE);

    memory_region_init_io(&s->mmio, OBJECT(s), &custom_mmio_ops, s,
                          "custom-crashdump-mmio", 0x1000);
    memory_region_init_io(&s->pio, OBJECT(s), &custom_pio_ops, s,
                          "custom-crashdump-pio", 0x20);

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
