// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek PCIe host controller driver.
 *
 * Copyright (c) 2017-2019 MediaTek Inc.
 * Author: Ryder Lee <ryder.lee@mediatek.com>
 *	   Honghui Zhang <honghui.zhang@mediatek.com>
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <generic-phy.h>
#include <pci.h>
#include <reset.h>
#include <asm/io.h>
#include <linux/iopoll.h>
#include <linux/list.h>

/* PCIe shared registers */
#define PCIE_SYS_CFG		0x00
#define PCIE_INT_ENABLE		0x0c
#define PCIE_CFG_ADDR		0x20
#define PCIE_CFG_DATA		0x24

/* PCIe per port registers */
#define PCIE_BAR0_SETUP		0x10
#define PCIE_CLASS		0x34
#define PCIE_LINK_STATUS	0x50

#define PCIE_PORT_INT_EN(x)	BIT(20 + (x))
#define PCIE_PORT_PERST(x)	BIT(1 + (x))
#define PCIE_PORT_LINKUP	BIT(0)
#define PCIE_BAR_MAP_MAX	GENMASK(31, 16)

#define PCIE_BAR_ENABLE		BIT(0)
#define PCIE_REVISION_ID	BIT(0)
#define PCIE_CLASS_CODE		(0x60400 << 8)
#define PCIE_CONF_REG(regn)	(((regn) & GENMASK(7, 2)) | \
				((((regn) >> 8) & GENMASK(3, 0)) << 24))
#define PCIE_CONF_ADDR(regn, bdf) \
				(PCIE_CONF_REG(regn) | (bdf))

/* MediaTek specific configuration registers */
#define PCIE_FTS_NUM		0x70c
#define PCIE_FTS_NUM_MASK	GENMASK(15, 8)
#define PCIE_FTS_NUM_L0(x)	((x) & 0xff << 8)

#define PCIE_FC_CREDIT		0x73c
#define PCIE_FC_CREDIT_MASK	(GENMASK(31, 31) | GENMASK(28, 16))
#define PCIE_FC_CREDIT_VAL(x)	((x) << 16)

/* PCIe V2 share registers */
#define	PCI_VENDOR_ID_MEDIATEK	0x14c3
#define PCIE_SYS_CFG_V2		0x0
#define PCIE_CSR_LTSSM_EN(x)	BIT(0 + (x) * 8)
#define PCIE_CSR_ASPM_L1_EN(x)	BIT(1 + (x) * 8)

/* PCIe V2 per-port registers */
#define PCIE_MSI_VECTOR		0x0c0

#define PCIE_CONF_VEND_ID	0x100
#define PCIE_CONF_CLASS_ID	0x106

#define PCIE_INT_MASK		0x420
#define INTX_MASK		GENMASK(19, 16)
#define INTX_SHIFT		16
#define PCIE_INT_STATUS		0x424
#define MSI_STATUS		BIT(23)
#define PCIE_IMSI_STATUS	0x42c
#define PCIE_IMSI_ADDR		0x430
#define MSI_MASK		BIT(23)
#define MTK_MSI_IRQS_NUM	32

#define PCIE_AHB_TRANS_BASE0_L	0x438
#define PCIE_AHB_TRANS_BASE0_H	0x43c
#define AHB2PCIE_SIZE(x)	((x) & GENMASK(4, 0))
#define PCIE_AXI_WINDOW0	0x448
#define WIN_ENABLE		BIT(7)

/* PCIe V2 configuration transaction header */
#define PCIE_CFG_HEADER0	0x460
#define PCIE_CFG_HEADER1	0x464
#define PCIE_CFG_HEADER2	0x468
#define PCIE_CFG_WDATA		0x470
#define PCIE_APP_TLP_REQ	0x488
#define PCIE_CFG_RDATA		0x48c
#define APP_CFG_REQ		BIT(0)
#define APP_CPL_STATUS		GENMASK(7, 5)

#define CFG_WRRD_TYPE_0		4
#define CFG_WR_FMT		2
#define CFG_RD_FMT		0

#define CFG_DW0_LENGTH(length)	((length) & GENMASK(9, 0))
#define CFG_DW0_TYPE(type)	(((type) << 24) & GENMASK(28, 24))
#define CFG_DW0_FMT(fmt)	(((fmt) << 29) & GENMASK(31, 29))
#define CFG_DW2_REGN(regn)	((regn) & GENMASK(11, 2))
#define CFG_DW2_FUN(fun)	(((fun) << 16) & GENMASK(18, 16))
#define CFG_DW2_DEV(dev)	(((dev) << 19) & GENMASK(23, 19))
#define CFG_DW2_BUS(bus)	(((bus) << 24) & GENMASK(31, 24))
#define CFG_HEADER_DW0(type, fmt) \
	(CFG_DW0_LENGTH(1) | CFG_DW0_TYPE(type) | CFG_DW0_FMT(fmt))
#define CFG_HEADER_DW1(where, size) \
	(GENMASK(((size) - 1), 0) << ((where) & 0x3))
#define CFG_HEADER_DW2(regn, fun, dev, bus) \
	(CFG_DW2_REGN(regn) | CFG_DW2_FUN(fun) | \
	CFG_DW2_DEV(dev) | CFG_DW2_BUS(bus))

#define PCIE_RST_CTRL		0x510
#define PCIE_PHY_RSTB		BIT(0)
#define PCIE_PIPE_SRSTB		BIT(1)
#define PCIE_MAC_SRSTB		BIT(2)
#define PCIE_CRSTB		BIT(3)
#define PCIE_PERSTB		BIT(8)
#define PCIE_LINKDOWN_RST_EN	GENMASK(15, 13)
#define PCIE_LINK_STATUS_V2	0x804
#define PCIE_PORT_LINKUP_V2	BIT(10)

#define	PCIBIOS_SET_FAILED	-1
#define	PCIBIOS_SUCCESSFUL	0
#define	PCI_SLOT(x)	PCI_DEV(x)

struct mtk_pcie_port {
	void __iomem *base;
	struct list_head list;
	struct mtk_pcie *pcie;
	struct reset_ctl reset;
	struct clk sys_ck;
	struct clk ahb_ck;
	struct clk aux_ck;
	struct clk axi_ck;
	struct clk obff_ck;
	struct clk pipe_ck;
	struct phy phy;
	u32 slot;
};

struct mtk_pcie {
	void __iomem *base;
	struct clk free_ck;
	struct list_head ports;
};

static int mtk_pcie_config_address(struct udevice *udev, pci_dev_t bdf,
				   uint offset, void **paddress)
{
	struct mtk_pcie *pcie = dev_get_priv(udev);

	writel(PCIE_CONF_ADDR(offset, bdf), pcie->base + PCIE_CFG_ADDR);
	*paddress = pcie->base + PCIE_CFG_DATA + (offset & 3);

	return 0;
}

static int mtk_pcie_read_config(struct udevice *bus, pci_dev_t bdf,
				uint offset, ulong *valuep,
				enum pci_size_t size)
{
	return pci_generic_mmap_read_config(bus, mtk_pcie_config_address,
					    bdf, offset, valuep, size);
}

static int mtk_pcie_write_config(struct udevice *bus, pci_dev_t bdf,
				 uint offset, ulong value,
				 enum pci_size_t size)
{
	return pci_generic_mmap_write_config(bus, mtk_pcie_config_address,
					     bdf, offset, value, size);
}

static const struct dm_pci_ops mtk_pcie_ops = {
	.read_config	= mtk_pcie_read_config,
	.write_config	= mtk_pcie_write_config,
};

static int mtk_pcie_check_cfg_cpld(struct mtk_pcie_port *port)
{
	u32 val;
	int err, i;

	for (i = 0; i < 100; i ++) {
		val = readl(port->base + PCIE_APP_TLP_REQ);
		if (!(val & APP_CFG_REQ))
			break;
		mdelay(1);
	}
//	if (i == 100)
//		return PCIBIOS_SET_FAILED;

		mdelay(10);
//	if (readl(port->base + PCIE_APP_TLP_REQ) & APP_CPL_STATUS)
//		return PCIBIOS_SET_FAILED;

	return PCIBIOS_SUCCESSFUL;
}

static int mtk_pcie_hw_rd_cfg(struct mtk_pcie_port *port, u32 bus, u32 devfn,
			      int where, int size, u32 *val)
{
	u32 tmp;

	/* Write PCIe configuration transaction header for Cfgrd */
	writel(CFG_HEADER_DW0(CFG_WRRD_TYPE_0, CFG_RD_FMT),
	       port->base + PCIE_CFG_HEADER0);
	writel(CFG_HEADER_DW1(where, size), port->base + PCIE_CFG_HEADER1);
	writel(CFG_HEADER_DW2(where, PCI_FUNC(devfn), PCI_SLOT(devfn), bus),
	       port->base + PCIE_CFG_HEADER2);

	/* Trigger h/w to transmit Cfgrd TLP */
	tmp = readl(port->base + PCIE_APP_TLP_REQ);
	tmp |= APP_CFG_REQ;
	writel(tmp, port->base + PCIE_APP_TLP_REQ);

	/* Check completion status */
	if (mtk_pcie_check_cfg_cpld(port))
		return PCIBIOS_SET_FAILED;

	/* Read cpld payload of Cfgrd */
	*val = readl(port->base + PCIE_CFG_RDATA);
	printf("%s:%d RD: %08x ret: %08x\n", __func__, __LINE__, CFG_HEADER_DW2(where, PCI_FUNC(devfn), PCI_SLOT(devfn), bus), *val);

	if (size == 1)
		*val = (*val >> (8 * (where & 3))) & 0xff;
	else if (size == 2)
		*val = (*val >> (8 * (where & 3))) & 0xffff;

	return PCIBIOS_SUCCESSFUL;
}

static int mtk_pcie_hw_wr_cfg(struct mtk_pcie_port *port, u32 bus, u32 devfn,
			      int where, int size, u32 val)
{
	/* Write PCIe configuration transaction header for Cfgwr */
	writel(CFG_HEADER_DW0(CFG_WRRD_TYPE_0, CFG_WR_FMT),
	       port->base + PCIE_CFG_HEADER0);
	writel(CFG_HEADER_DW1(where, size), port->base + PCIE_CFG_HEADER1);
	writel(CFG_HEADER_DW2(where, PCI_FUNC(devfn), PCI_SLOT(devfn), bus),
	       port->base + PCIE_CFG_HEADER2);
	printf("%s:%d WR: %08x, %08x (%08x)\n", __func__, __LINE__, CFG_HEADER_DW2(where, PCI_FUNC(devfn), PCI_SLOT(devfn), bus), val, val << 8 * (where & 3));

	/* Write Cfgwr data */
	val = val << 8 * (where & 3);
	writel(val, port->base + PCIE_CFG_WDATA);

	/* Trigger h/w to transmit Cfgwr TLP */
	val = readl(port->base + PCIE_APP_TLP_REQ);
	val |= APP_CFG_REQ;
	writel(val, port->base + PCIE_APP_TLP_REQ);

	/* Check completion status */
	return mtk_pcie_check_cfg_cpld(port);
}

static struct mtk_pcie_port *mtk_pcie_find_port(struct udevice *bus,
						unsigned int devfn)
{
	struct mtk_pcie *pcie = dev_get_priv(bus);
	struct mtk_pcie_port *port;
	struct pci_dev *dev = NULL;

	/*
	 * Walk the bus hierarchy to get the devfn value
	 * of the port in the root bus.
	 */
//	while (bus && bus->number) {
//		dev = bus->self;
//		bus = dev->bus;
//		devfn = dev->devfn;
//	}

	list_for_each_entry(port, &pcie->ports, list) {
		printf("port->slot = %d, PCI_SLOT(devfn) = %d\n", port->slot, PCI_SLOT(devfn));
		if (port->slot == PCI_SLOT(devfn))
			return port;
	}

	return NULL;
}

static int mtk_pcie_config_read(struct udevice *bus, pci_dev_t devfn,
				uint where, ulong *val,
				enum pci_size_t size)
//static int mtk_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
//				int where, int size, u32 *val)
{
//	struct mtk_pcie *pcie = dev_get_priv(bus);
	struct mtk_pcie_port *port;
//	u32 bn = bus->number;
	int ret;

	port = mtk_pcie_find_port(bus, devfn);
	if (!port) {
		*val = ~0;
		return -1;
	}

	ret = mtk_pcie_hw_rd_cfg(port, port->slot, devfn, where, size, (u32 *)val);
	if (ret)
		*val = ~0;

	return ret;
}

static int mtk_pcie_config_write(struct udevice *bus, pci_dev_t devfn,
				 uint where, ulong val,
				 enum pci_size_t size)
//static int mtk_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
//				 int where, int size, u32 val)
{
//	struct mtk_pcie *pcie = dev_get_priv(bus);
	struct mtk_pcie_port *port;
//	u32 bn = bus->number;

	port = mtk_pcie_find_port(bus, devfn);
	if (!port)
		return -1;

	return mtk_pcie_hw_wr_cfg(port, port->slot, devfn, where, size, val);
}

static const struct dm_pci_ops mtk_pcie_ops_v2 = {
	.read_config  = mtk_pcie_config_read,
	.write_config = mtk_pcie_config_write,
};


static void mtk_pcie_port_free(struct mtk_pcie_port *port)
{
	list_del(&port->list);
	free(port);
}

static int mtk_pcie_startup_port(struct mtk_pcie_port *port)
{
	struct mtk_pcie *pcie = port->pcie;
	u32 slot = PCI_DEV(port->slot << 11);
//	u32 val;
//	int err;

	/* assert port PERST_N */
	setbits_le32(pcie->base + PCIE_SYS_CFG, PCIE_PORT_PERST(port->slot));
	/* de-assert port PERST_N */
	clrbits_le32(pcie->base + PCIE_SYS_CFG, PCIE_PORT_PERST(port->slot));

	/* 100ms timeout value should be enough for Gen1/2 training */
//	err = readl_poll_timeout(port->base + PCIE_LINK_STATUS, val,
//				 !!(val & PCIE_PORT_LINKUP), 100000);
//	if (err)
//		return -ETIMEDOUT;
mdelay(100);

	/* disable interrupt */
	clrbits_le32(pcie->base + PCIE_INT_ENABLE,
		     PCIE_PORT_INT_EN(port->slot));

	/* map to all DDR region. We need to set it before cfg operation. */
	writel(PCIE_BAR_MAP_MAX | PCIE_BAR_ENABLE,
	       port->base + PCIE_BAR0_SETUP);

	/* configure class code and revision ID */
	writel(PCIE_CLASS_CODE | PCIE_REVISION_ID, port->base + PCIE_CLASS);

	/* configure FC credit */
	writel(PCIE_CONF_ADDR(PCIE_FC_CREDIT, slot),
	       pcie->base + PCIE_CFG_ADDR);
	clrsetbits_le32(pcie->base + PCIE_CFG_DATA, PCIE_FC_CREDIT_MASK,
			PCIE_FC_CREDIT_VAL(0x806c));

	/* configure RC FTS number to 250 when it leaves L0s */
	writel(PCIE_CONF_ADDR(PCIE_FTS_NUM, slot), pcie->base + PCIE_CFG_ADDR);
	clrsetbits_le32(pcie->base + PCIE_CFG_DATA, PCIE_FTS_NUM_MASK,
			PCIE_FTS_NUM_L0(0x50));

	return 0;
}

static int mtk_pcie_startup_port_v2(struct mtk_pcie_port *port)
{
	struct mtk_pcie *pcie = port->pcie;
//	struct resource *mem = &pcie->mem;
//	const struct mtk_pcie_soc *soc = port->pcie->soc;
	u32 val;
	size_t size;
//	int err;

	/* MT7622 platforms need to enable LTSSM and ASPM from PCIe subsys */
	if (pcie->base) {
	printf("%s:%d\n", __func__, __LINE__);
		val = readl(pcie->base + PCIE_SYS_CFG_V2);
		val |= PCIE_CSR_LTSSM_EN(port->slot) |
		       PCIE_CSR_ASPM_L1_EN(port->slot);
		writel(val, pcie->base + PCIE_SYS_CFG_V2);
	}

	printf("%s:%d\n", __func__, __LINE__);
	/* Assert all reset signals */
	writel(0, port->base + PCIE_RST_CTRL);

	/*
	 * Enable PCIe link down reset, if link status changed from link up to
	 * link down, this will reset MAC control registers and configuration
	 * space.
	 */
	writel(PCIE_LINKDOWN_RST_EN, port->base + PCIE_RST_CTRL);

	/* De-assert PHY, PE, PIPE, MAC and configuration reset	*/
	val = readl(port->base + PCIE_RST_CTRL);
	val |= PCIE_PHY_RSTB | PCIE_PERSTB | PCIE_PIPE_SRSTB |
	       PCIE_MAC_SRSTB | PCIE_CRSTB;
	writel(val, port->base + PCIE_RST_CTRL);

	printf("%s:%d\n", __func__, __LINE__);
	/* Set up vendor ID and class code */
#if 1 // MT7622 if (soc->need_fix_class_id) {
		val = PCI_VENDOR_ID_MEDIATEK;
		writew(val, port->base + PCIE_CONF_VEND_ID);

		val = PCI_CLASS_BRIDGE_PCI;
		writew(val, port->base + PCIE_CONF_CLASS_ID);
#endif // }

	/* 100ms timeout value should be enough for Gen1/2 training */
//	err = readl_poll_timeout(port->base + PCIE_LINK_STATUS_V2, val,
//				 !!(val & PCIE_PORT_LINKUP_V2), 20,
//				 100 * USEC_PER_MSEC);
//	if (err)
//		return -ETIMEDOUT;
mdelay(100);

	printf("%s:%d\n", __func__, __LINE__);
	/* Set INTx mask */
	val = readl(port->base + PCIE_INT_MASK);
	val &= ~INTX_MASK;
	writel(val, port->base + PCIE_INT_MASK);

	printf("%s:%d\n", __func__, __LINE__);
	/* Set AHB to PCIe translation windows */
	struct { u32 start; u32 end; } *mem, m = {0x20000000, 0x2fffffff};
	mem = &m;
	size = mem->end - mem->start;
	val = lower_32_bits(mem->start) | AHB2PCIE_SIZE(fls(size));
	writel(val, port->base + PCIE_AHB_TRANS_BASE0_L);

	val = upper_32_bits(mem->start);
	writel(val, port->base + PCIE_AHB_TRANS_BASE0_H);

	/* Set PCIe to AXI translation memory space.*/
	val = fls(0xffffffff) | WIN_ENABLE;
	writel(val, port->base + PCIE_AXI_WINDOW0);

	printf("%s:%d\n", __func__, __LINE__);
	return 0;
}

static void mtk_pcie_enable_port(struct mtk_pcie_port *port)
{
	int err;

#if 0
	err = clk_enable(&port->sys_ck);
	if (err)
		goto exit;
	err = reset_assert(&port->reset);
	if (err)
		goto exit;

	err = reset_deassert(&port->reset);
	if (err)
		goto exit;
#endif

//	printf("%s:%d\n", __func__, __LINE__);
//	err = generic_phy_init(&port->phy);
//	if (err)
//		goto exit;
//
//	printf("%s:%d\n", __func__, __LINE__);
//	err = generic_phy_power_on(&port->phy);
//	if (err)
//		goto exit;

	printf("%s:%d\n", __func__, __LINE__);
	if (!mtk_pcie_startup_port_v2(port))
		return;

	pr_err("Port%d link down\n", port->slot);
exit:
	mtk_pcie_port_free(port);
}

static int mtk_pcie_parse_port(struct udevice *dev, u32 slot)
{
	struct mtk_pcie *pcie = dev_get_priv(dev);
	struct mtk_pcie_port *port;
	char name[10];
	int err;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	snprintf(name, sizeof(name), "port%d", slot);
	port->base = dev_remap_addr_name(dev, name);
	if (!port->base)
		return -ENOENT;

	printf("%s:%d\n", __func__, __LINE__);

#define	GET_CLK(c, s) \
	snprintf(name, sizeof(name), #c "%d", (s)); \
	err = clk_get_by_name(dev, name, &port-> c); \
	if (err) \
		return err; \
	err = clk_enable(&port-> c);

	GET_CLK(sys_ck, slot)
	GET_CLK(ahb_ck, slot)
	GET_CLK(aux_ck, slot)
	GET_CLK(axi_ck, slot)
	GET_CLK(obff_ck, slot)
	GET_CLK(pipe_ck, slot)


#if 0
	err = reset_get_by_index(dev, slot, &port->reset);
	if (err)
		return err;
#endif

	printf("%s:%d\n", __func__, __LINE__);
//	err = generic_phy_get_by_index(dev, slot, &port->phy);
//	if (err)
//		return err;

	port->slot = slot;
	port->pcie = pcie;

	printf("%s:%d add port %s\n", __func__, __LINE__, name);
	INIT_LIST_HEAD(&port->list);
	list_add_tail(&port->list, &pcie->ports);

	return 0;
}

static int mtk_pcie_probe(struct udevice *dev)
{
	struct mtk_pcie *pcie = dev_get_priv(dev);
	struct mtk_pcie_port *port, *tmp;
	ofnode subnode;
	int err;

	INIT_LIST_HEAD(&pcie->ports);
printf("%s:%d\n", __func__, __LINE__);
	pcie->base = dev_remap_addr_name(dev, "subsys");
	if (!pcie->base)
		return -ENOENT;

//printf("%s:%d\n", __func__, __LINE__);
	//err = clk_get_by_name(dev, "free_ck", &pcie->free_ck);
//	err = clk_get_by_name(dev, "sys_ck0", &pcie->free_ck);
//	if (err)
//		return err;

//printf("%s:%d\n", __func__, __LINE__);
//	/* enable top level clock */
//	err = clk_enable(&pcie->free_ck);
//	if (err)
//		return err;

printf("%s:%d\n", __func__, __LINE__);
	dev_for_each_subnode(subnode, dev) {
		struct fdt_pci_addr addr;
		u32 slot = 0;

		if (!ofnode_is_available(subnode))
			continue;

		err = ofnode_read_pci_addr(subnode, 0, "reg", &addr);
		if (err)
			return err;

		slot = PCI_DEV(addr.phys_hi);

		err = mtk_pcie_parse_port(dev, slot);
		if (err)
			return err;
	}

printf("%s:%d\n", __func__, __LINE__);
	/* enable each port, and then check link status */
	list_for_each_entry_safe(port, tmp, &pcie->ports, list)
		mtk_pcie_enable_port(port);

printf("%s:%d\n", __func__, __LINE__);
	return 0;
}

static const struct udevice_id mtk_pcie_ids[] = {
	{ .compatible = "mediatek,mt7623-pcie", },
	{ }
};

U_BOOT_DRIVER(pcie_mediatek) = {
	.name	= "pcie_mediatek",
	.id	= UCLASS_PCI,
	.of_match = mtk_pcie_ids,
	.ops	= &mtk_pcie_ops,
	.probe	= mtk_pcie_probe,
	.priv_auto_alloc_size = sizeof(struct mtk_pcie),
};

static const struct udevice_id mtk_pcie_ids_v2[] = {
	{ .compatible = "mediatek,mt7622-pcie", },
	{ }
};

U_BOOT_DRIVER(pcie_mediatek_v2) = {
	.name	= "pcie_mediatek",
	.id	= UCLASS_PCI,
	.of_match = mtk_pcie_ids_v2,
	.ops	= &mtk_pcie_ops_v2,
	.probe	= mtk_pcie_probe,
	.priv_auto_alloc_size = sizeof(struct mtk_pcie),
};

