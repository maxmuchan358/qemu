/*
 * QTest coverage for custom crashdump table verification devices.
 *
 * The tests mirror the kdump dump tables by validating the whole dumped PCI,
 * MMIO, and PIO regions rather than a handful of device-specific registers.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"
#include "standard-headers/linux/pci_regs.h"

#define TEST_CMDLINE \
    "-M q35 -nodefaults " \
    "-device custom-crashdump-test,addr=04.0,profile=1,instance-id=42 " \
    "-device custom-crashdump-net,addr=05.0,instance-id=1 " \
    "-device custom-crashdump-stor,addr=06.0,instance-id=2 " \
    "-device pcie-root-port,id=crash_rp0,bus=pcie.0,chassis=7,slot=7,addr=07.0 " \
    "-device custom-crashdump-pcie,bus=crash_rp0,addr=00.0,instance-id=3"

typedef struct PatternSpec {
    char device_tag;
    char space_tag;
    uint32_t variant;
    uint32_t salt;
    uint32_t len;
} PatternSpec;

static uint32_t kdmp_pattern_signature(char device_tag, char space_tag,
                                       uint32_t salt)
{
    return 0x4b44554dU ^ ((uint32_t)(uint8_t)device_tag << 24)
           ^ ((uint32_t)(uint8_t)space_tag << 16) ^ salt;
}

static uint8_t kdmp_pattern_byte(const PatternSpec *spec, uint32_t offset)
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
        return (uint8_t)spec->device_tag;
    }
    if (offset == 5) {
        return (uint8_t)spec->space_tag;
    }
    if (offset == 6) {
        return spec->variant & 0xff;
    }
    if (offset == 7) {
        return (spec->variant >> 8) & 0xff;
    }
    if (offset >= 8 && offset < 12) {
        return (spec->len >> ((offset - 8) * 8)) & 0xff;
    }
    if (offset >= 12 && offset < 16) {
        uint32_t signature = kdmp_pattern_signature(spec->device_tag,
                                                    spec->space_tag,
                                                    spec->salt);
        return (signature >> ((offset - 12) * 8)) & 0xff;
    }

    return (uint8_t)(((offset * 0x3dU) ^ (offset >> 8)
                      ^ (uint8_t)spec->device_tag
                      ^ ((uint8_t)spec->space_tag << 1)
                      ^ spec->variant ^ spec->salt) & 0xff);
}

static uint32_t kdmp_pattern_value(const PatternSpec *spec, uint32_t offset,
                                   uint32_t size)
{
    uint32_t value = 0;
    uint32_t index;

    for (index = 0; index < size; index++) {
        value |= (uint32_t)kdmp_pattern_byte(spec, offset + index) << (index * 8);
    }

    return value;
}

static void assert_config_region(QPCIDevice *dev, uint32_t cfg_offset,
                                 const PatternSpec *spec)
{
    uint32_t offset;

    for (offset = 0; offset < spec->len; offset++) {
        g_assert_cmphex(qpci_config_readb(dev, cfg_offset + offset), ==,
                        kdmp_pattern_byte(spec, offset));
    }
    for (offset = 0; offset + 1 < spec->len; offset += 2) {
        g_assert_cmphex(qpci_config_readw(dev, cfg_offset + offset), ==,
                        kdmp_pattern_value(spec, offset, 2));
    }
    for (offset = 0; offset + 3 < spec->len; offset += 4) {
        g_assert_cmphex(qpci_config_readl(dev, cfg_offset + offset), ==,
                        kdmp_pattern_value(spec, offset, 4));
    }
}

static void assert_bar_region(QPCIDevice *dev, QPCIBar bar,
                              const PatternSpec *spec)
{
    uint32_t offset;

    for (offset = 0; offset < spec->len; offset++) {
        g_assert_cmphex(qpci_io_readb(dev, bar, offset), ==,
                        kdmp_pattern_byte(spec, offset));
    }
    for (offset = 0; offset + 1 < spec->len; offset += 2) {
        g_assert_cmphex(qpci_io_readw(dev, bar, offset), ==,
                        kdmp_pattern_value(spec, offset, 2));
    }
    for (offset = 0; offset + 3 < spec->len; offset += 4) {
        g_assert_cmphex(qpci_io_readl(dev, bar, offset), ==,
                        kdmp_pattern_value(spec, offset, 4));
    }
}

static void test_custom_test_device(void)
{
    QTestState *qts;
    QPCIBus *pcibus;
    QPCIDevice *dev;
    QPCIBar mmio;
    QPCIBar pio;
    PatternSpec cfg = { 'T', 'C', 42, 1, 0xc0 };
    PatternSpec mmio_spec = { 'T', 'M', 42, 1, 0x400 };
    PatternSpec pio_spec = { 'T', 'P', 42, 1, 0x80 };

    qts = qtest_init(TEST_CMDLINE);
    pcibus = qpci_new_pc(qts, NULL);
    dev = qpci_device_find(pcibus, QPCI_DEVFN(0x4, 0x0));
    g_assert_nonnull(dev);

    qpci_device_enable(dev);
    mmio = qpci_iomap(dev, 0, NULL);
    pio = qpci_iomap(dev, 1, NULL);

    assert_config_region(dev, 0x40, &cfg);
    assert_bar_region(dev, mmio, &mmio_spec);
    assert_bar_region(dev, pio, &pio_spec);

    qpci_iounmap(dev, mmio);
    qpci_iounmap(dev, pio);
    g_free(dev);
    qpci_free_pc(pcibus);
    qtest_quit(qts);
}

static void test_custom_net_device(void)
{
    QTestState *qts;
    QPCIBus *pcibus;
    QPCIDevice *dev;
    QPCIBar mmio;
    QPCIBar pio;

    qts = qtest_init(TEST_CMDLINE);
    pcibus = qpci_new_pc(qts, NULL);
    dev = qpci_device_find(pcibus, QPCI_DEVFN(0x5, 0x0));
    g_assert_nonnull(dev);

    qpci_device_enable(dev);
    mmio = qpci_iomap(dev, 0, NULL);
    pio = qpci_iomap(dev, 1, NULL);

    {
        PatternSpec cfg = { 'N', 'C', 1, 0, 0xc0 };
        PatternSpec mmio_spec = { 'N', 'M', 1, 0, 0x200 };
        PatternSpec pio_spec = { 'N', 'P', 1, 0, 0x40 };

        assert_config_region(dev, 0x40, &cfg);
        assert_bar_region(dev, mmio, &mmio_spec);
        assert_bar_region(dev, pio, &pio_spec);
    }

    qpci_iounmap(dev, mmio);
    qpci_iounmap(dev, pio);
    g_free(dev);
    qpci_free_pc(pcibus);
    qtest_quit(qts);
}

static void test_custom_stor_device(void)
{
    QTestState *qts;
    QPCIBus *pcibus;
    QPCIDevice *dev;
    QPCIBar mmio;

    qts = qtest_init(TEST_CMDLINE);
    pcibus = qpci_new_pc(qts, NULL);
    dev = qpci_device_find(pcibus, QPCI_DEVFN(0x6, 0x0));
    g_assert_nonnull(dev);

    qpci_device_enable(dev);
    mmio = qpci_iomap(dev, 0, NULL);

    {
        PatternSpec cfg = { 'S', 'C', 2, 0, 0xc0 };
        PatternSpec mmio_spec = { 'S', 'M', 2, 0, 0x400 };

        assert_config_region(dev, 0x40, &cfg);
        assert_bar_region(dev, mmio, &mmio_spec);
    }

    qpci_iounmap(dev, mmio);
    g_free(dev);
    qpci_free_pc(pcibus);
    qtest_quit(qts);
}

static void test_custom_pcie_device(void)
{
    QTestState *qts;
    QPCIBus *pcibus;
    QPCIDevice *dev;
    uint8_t cap_ptr;
    uint32_t bar0;

    qts = qtest_init(TEST_CMDLINE);
    pcibus = qpci_new_pc(qts, NULL);
    g_assert_cmpint(qpci_secondary_buses_init(pcibus), >=, 1);
    dev = qpci_device_find(pcibus, QPCI_DEVFN(0x20, 0x0));
    g_assert_nonnull(dev);

    qpci_device_enable(dev);

    cap_ptr = qpci_config_readb(dev, PCI_CAPABILITY_LIST);
    g_assert_cmphex(cap_ptr, ==, 0x80);
    g_assert_cmphex(qpci_config_readb(dev, cap_ptr + PCI_CAP_LIST_ID), ==,
                    PCI_CAP_ID_EXP);

    bar0 = qpci_config_readl(dev, PCI_BASE_ADDRESS_0);
    g_assert_cmphex(bar0 & PCI_BASE_ADDRESS_MEM_TYPE_MASK, ==,
                    PCI_BASE_ADDRESS_MEM_TYPE_64);

    g_free(dev);
    qpci_free_pc(pcibus);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/custom-crashdump/test-device", test_custom_test_device);
    qtest_add_func("/custom-crashdump/net-device", test_custom_net_device);
    qtest_add_func("/custom-crashdump/stor-device", test_custom_stor_device);
    qtest_add_func("/custom-crashdump/pcie-device", test_custom_pcie_device);

    return g_test_run();
}