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
#include <dm/pinctrl.h>
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

#define	K_GBL_1			0x014
#define		MTK_ROOT_PORT	(4 << 12)
#define	PCIE_KCNT_2		0x014
#define	PCIE_L1_ENTRY_LAT	GENMASK(15,11)
#define	PCIE_L0S_ENTRY_LAT	GENMASK(23,16)

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
#define PCI_CSR_ASPM_L0S_EN(x)	BIT(2 + (x) * 8)

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
#define	FAKE_ROOT_BUS(bdf, p)	(PCI_BUS(bdf) - (p)->bus_offset)

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
	int bus_offset;
	struct reset_ctl_bulk resetbulk;
};

struct mtk_pcie {
	void __iomem *base;
	struct clk free_ck;
	struct mtk_pcie_port port;
};


static int mtk_pcie_check_cfg_cpld(struct mtk_pcie_port *port)
{
	u32 val;
	int i;

	for (i = 0; i < 100; i ++) {
		val = readl(port->base + PCIE_APP_TLP_REQ);
		if (!(val & APP_CFG_REQ))
			break;
		mdelay(1);
	}
	if (i == 100) {
		printf("%s: timed out\n", __func__);
		return PCIBIOS_SET_FAILED;
	}

	if (readl(port->base + PCIE_APP_TLP_REQ) & APP_CPL_STATUS) {
		printf("%s: not ready\n", __func__);
		return PCIBIOS_SET_FAILED;
	}

	return PCIBIOS_SUCCESSFUL;
}

static int mtk_pcie_hw_rd_cfg(struct mtk_pcie_port *port, u32 bdf,
			      int where, int size, u32 *val)
{
	u32 tmp;
	if (PCI_SLOT(bdf) != 0)
		return PCIBIOS_SUCCESSFUL;

	if (port->bus_offset == -1)
		port->bus_offset = PCI_BUS(bdf);

	/* Write PCIe configuration transaction header for Cfgrd */
	writel(CFG_HEADER_DW0(CFG_WRRD_TYPE_0, CFG_RD_FMT),
	       port->base + PCIE_CFG_HEADER0);
	writel(CFG_HEADER_DW1(where, size), port->base + PCIE_CFG_HEADER1);
	writel(CFG_HEADER_DW2(where, PCI_FUNC(bdf), PCI_SLOT(bdf), FAKE_ROOT_BUS(bdf, port)),
	       port->base + PCIE_CFG_HEADER2);

	/* Trigger h/w to transmit Cfgrd TLP */
	tmp = readl(port->base + PCIE_APP_TLP_REQ);
	tmp |= APP_CFG_REQ;
	writel(tmp, port->base + PCIE_APP_TLP_REQ);

	/* Check completion status */
	if (mtk_pcie_check_cfg_cpld(port)) {
		return 0;
	}

	/* Read cpld payload of Cfgrd */
	*val = readl(port->base + PCIE_CFG_RDATA);

	if (size == 1)
		*val = (*val >> (8 * (where & 3))) & 0xff;
	else if (size == 2)
		*val = (*val >> (8 * (where & 3))) & 0xffff;

	return PCIBIOS_SUCCESSFUL;
}

static int mtk_pcie_hw_wr_cfg(struct mtk_pcie_port *port, u32 bdf,
			      int where, int size, u32 val)
{
	/* Write PCIe configuration transaction header for Cfgwr */
	writel(CFG_HEADER_DW0(CFG_WRRD_TYPE_0, CFG_WR_FMT),
	       port->base + PCIE_CFG_HEADER0);
	writel(CFG_HEADER_DW1(where, size), port->base + PCIE_CFG_HEADER1);
	writel(CFG_HEADER_DW2(where, PCI_FUNC(bdf), PCI_SLOT(bdf), FAKE_ROOT_BUS(bdf, port)),
	       port->base + PCIE_CFG_HEADER2);

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

static int mtk_pcie_config_read(struct udevice *bus, pci_dev_t bdf,
				uint where, ulong *val,
				enum pci_size_t size)
{
	struct mtk_pcie *pcie = dev_get_priv(bus);
	struct mtk_pcie_port *port = &pcie->port;
	int ret;
	int sz;

	switch (size) {
	case PCI_SIZE_8:
		sz = 1;
		*val = 0xff;
		break;
	case PCI_SIZE_16:
		sz = 2;
		*val = 0xffff;
		break;
	case PCI_SIZE_32:
		sz = 4;
		*val = 0xffffffff;
		break;
	default:
		return -1;
	}

	ret = mtk_pcie_hw_rd_cfg(port, bdf, where, sz, (u32 *)val);

	return ret;
}

static int mtk_pcie_config_write(struct udevice *bus, pci_dev_t bdf,
				 uint where, ulong val,
				 enum pci_size_t size)
{
	struct mtk_pcie *pcie = dev_get_priv(bus);
	struct mtk_pcie_port *port = &pcie->port;
	int sz;

	switch (size) {
	case PCI_SIZE_8:
		sz = 1;
		break;
	case PCI_SIZE_16:
		sz = 2;
		break;
	case PCI_SIZE_32:
		sz = 4;
		break;
	default:
		return -1;
	}

	return mtk_pcie_hw_wr_cfg(port, bdf, where, sz, val);
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

struct mtk_pcie_port *port_saved[2] = {NULL, NULL};

static int mtk_pcie_startup_port_v2(struct mtk_pcie_port *port)
{
	struct mtk_pcie *pcie = port->pcie;
	u32 val;
	size_t size;

	int unit = ((u32)port->base == 0x1a143000)?0:1;

#if 1
if (unit == 1) {
	/* XXX */
#define PCIESYS_BASE		0x1a100800
#define PCIESYS_SYSCFG1		(PCIESYS_BASE + 0x14)
#define SYSCFG1()		*((volatile u32 *)PCIESYS_SYSCFG1)
#define SYS_CFG_SATA_MSK	(1 << 31)
#define SYS_CFG_PCI_EN		(0 << 31)
	SYSCFG1() = (SYSCFG1() & ~SYS_CFG_SATA_MSK) | SYS_CFG_PCI_EN;
	mdelay(100);
}
#endif

	/* MT7622 platforms need to enable LTSSM and ASPM from PCIe subsys */
	if (pcie->base) {
		val = readl(pcie->base + PCIE_SYS_CFG_V2);
		val |= PCIE_CSR_LTSSM_EN(unit);
		val |= PCIE_CSR_ASPM_L1_EN(unit);
		val |= PCI_CSR_ASPM_L0S_EN(unit);
		writel(val, pcie->base + PCIE_SYS_CFG_V2);
		/* correct the class ID for mt7622 */
		val = readl(port->base + K_GBL_1);
		val |= MTK_ROOT_PORT;
		writel(val, port->base + K_GBL_1);

		val = PCI_VENDOR_ID_MEDIATEK;
		writew(val, port->base + PCIE_CONF_VEND_ID);

		val = PCI_CLASS_BRIDGE_PCI;
		writew(val, port->base + PCIE_CONF_CLASS_ID);
	}
	port->bus_offset = -1;

	/* set l1_entry_latency and l0s entry latency to 4us */
	val = readl(port->base + PCIE_KCNT_2);
	val &= ~(PCIE_L1_ENTRY_LAT | PCIE_L0S_ENTRY_LAT);
	val |= 0x00088000;
	writel(val, port->base + PCIE_KCNT_2);


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
	mdelay(100);
	val = readl(port->base + PCIE_LINK_STATUS_V2);
	if (!(val & PCIE_PORT_LINKUP_V2))
		return -ETIMEDOUT;

	/* Set INTx mask */
//	val = readl(port->base + PCIE_INT_MASK);
//	val &= ~INTX_MASK;
//	writel(val, port->base + PCIE_INT_MASK);
	writel(0xffffffff, port->base + PCIE_INT_MASK);

	/* Set AHB to PCIe translation windows */
	struct { u32 start; u32 end; } *mem, m = {0x20000000, 0x2fffffff};
	mem = &m;
	size = mem->end - mem->start;
	size = 0x08000000 - 1;
	mem->start = 0x20000000 + unit * 0x08000000;
	val = lower_32_bits(mem->start) | AHB2PCIE_SIZE(fls(size));
	writel(val, port->base + PCIE_AHB_TRANS_BASE0_L);

	val = upper_32_bits(mem->start);
	val = 0;
	writel(val, port->base + PCIE_AHB_TRANS_BASE0_H);

	/* Set PCIe to AXI translation memory space.*/
	val = fls(0xffffffff) | WIN_ENABLE;
	writel(val, port->base + PCIE_AXI_WINDOW0);

	port_saved[unit] = port;
	mdelay(300);

	return 0;
}

static int mtk_pcie_enable_port(struct mtk_pcie_port *port)
{
	int err = 0;

	err = clk_enable(&port->sys_ck);
	if (err)
		goto exit;

	err = clk_enable(&port->ahb_ck);
	if (err)
		goto exit;

	err = clk_enable(&port->aux_ck);
	if (err)
		goto exit;

	err = clk_enable(&port->axi_ck);
	if (err)
		goto exit;

	err = clk_enable(&port->obff_ck);
	if (err)
		goto exit;

	err = clk_enable(&port->pipe_ck);
	if (err)
		goto exit;

#if 0
	err = reset_assert_bulk(&port->resetbulk);
	if (err)
		goto exit;
	udelay(100);
	err = reset_deassert_bulk(&port->resetbulk);
	if (err)
		goto exit;
	udelay(100);
#endif

	if (!mtk_pcie_startup_port_v2(port))
		return (0);

	pr_err("Port link down\n");
exit:
	clk_disable(&port->sys_ck);
	clk_disable(&port->ahb_ck);
	clk_disable(&port->aux_ck);
	clk_disable(&port->axi_ck);
	clk_disable(&port->obff_ck);
	clk_disable(&port->pipe_ck);
	mtk_pcie_port_free(port);
	return (err);
}

static int mtk_pcie_parse_port(struct udevice *dev)
{
	struct mtk_pcie *pcie = dev_get_priv(dev);
	struct mtk_pcie_port *port = &pcie->port;
	int err;

	port->base = dev_remap_addr_name(dev, "port");
	if (!port->base)
		return -ENOENT;

	err = clk_get_by_name(dev, "sys_ck", &port->sys_ck);
	if (err)
		return err;
	err = clk_get_by_name(dev, "ahb_ck", &port->ahb_ck);
	if (err)
		return err;
	err = clk_get_by_name(dev, "aux_ck", &port->aux_ck);
	if (err)
		return err;
	err = clk_get_by_name(dev, "axi_ck", &port->axi_ck);
	if (err)
		return err;
	err = clk_get_by_name(dev, "obff_ck", &port->obff_ck);
	if (err)
		return err;
	err = clk_get_by_name(dev, "pipe_ck", &port->pipe_ck);
	if (err)
		return err;
#if 1
	err = reset_get_bulk(dev, &port->resetbulk);
	if (err)
		return err;
#endif
	port->pcie = pcie;

	return 0;
}

static int mtk_pcie_probe(struct udevice *dev)
{
	struct mtk_pcie *pcie = dev_get_priv(dev);
	ofnode subnode;
	int err;

	pcie->base = dev_remap_addr_name(dev, "subsys");
	if (!pcie->base)
		return -ENOENT;

	dev_for_each_subnode(subnode, dev) {
		if (!ofnode_is_available(subnode))
			continue;

		err = mtk_pcie_parse_port(dev);
		if (err)
			return err;
	}

#ifdef CONFIG_PINCTRL
        pinctrl_select_state(dev, "default");
#endif

	/* enable each port, and then check link status */
	err = mtk_pcie_enable_port(&pcie->port);

	return (err);
}

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

