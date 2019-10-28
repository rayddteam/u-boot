// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2015 Marvell International Ltd.
 *
 * MVEBU USB HOST xHCI Controller
 */

#include <common.h>
#include <dm.h>
#include <fdtdec.h>
#include <generic-phy.h>
#include <usb.h>
#include <power/regulator.h>
#include <asm/gpio.h>

#include "xhci.h"

#define	MTK_XHCI_IP_PW_CTR0	0x00
#define 	CTRL0_IP_SW_RST		(1 << 0)
#define	MTK_XHCI_IP_PW_CTR1	0x04
#define 	CTRL1_IP_HOST_PDN	(1 << 0)
#define	MTK_XHCI_IP_PW_CTR2	0x08
#define 	CTRL2_IP_DEV_PDN	(1 << 0)
#define	MTK_XHCI_IP_PW_CTR3	0x0c
#define	MTK_XHCI_IP_PW_STS1	0x10
#define 	STS1_IP_SLEEP_STS	(1 << 30)
#define 	STS1_XHCI_RST		(1 << 11)
#define 	STS1_SYS125_RST		(1 << 10)
#define 	STS1_REF_RST		(1 << 8)
#define 	STS1_SYSPLL_STABLE	(1 << 0)
#define	MTK_XHCI_IP_PW_STS2	0x14
#define	MTK_XHCI_IP_XHCI_CAP	0x24
#define 	CAP_U3_PORT_NUM(p)	((p) & 0xff)
#define 	CAP_U2_PORT_NUM(p)	(((p) >> 8) & 0xff)
#define	MTK_XHCI_U3_CTRL_P(x)	(((x) * 8) + 0x30)
#define 	CTRL_PORT_HOST_SEL	(1 << 2)
#define 	CTRL_PORT_PDN	(1 << 1)
#define 	CTRL_PORT_DIS	(1 << 0)
#define	MTK_XHCI_U2_CTRL_P(x)	(((x) * 8) + 0x50)
#define	MTK_XHCI_U2_PHY_PLL	0x7c
#define 	CTRL_U2_FORCE_PLL_STB	(1 << 28)

struct mtk_xhci_platdata {
	fdt_addr_t hcd_base;
	fdt_addr_t ippc_base;
	int u3ports;
	int u2ports;
};

/**
 * Contains pointers to register base addresses
 * for the usb controller.
 */
struct mtk_xhci {
	struct xhci_ctrl ctrl;	/* Needs to come first in this struct! */
	struct usb_platdata usb_plat;
	struct xhci_hccr *hcd;
	struct phy phy;
};

#define	IPPCRD4(p, x)    readl((p)->ippc_base + (x))
#define	IPPCWR4(p, x, v) writel((v), (p)->ippc_base + (x))

static void
mtk_xhci_enable_core(struct mtk_xhci_platdata *plat)
{
	u32 val;
	int i;

	val = IPPCRD4(plat, MTK_XHCI_IP_XHCI_CAP);
	plat->u3ports = CAP_U3_PORT_NUM(val);
	plat->u2ports = CAP_U2_PORT_NUM(val);

	/* Clear reset bit. */
	IPPCWR4(plat, MTK_XHCI_IP_PW_CTR1, IPPCRD4(plat, MTK_XHCI_IP_PW_CTR1) &
	    ~CTRL1_IP_HOST_PDN);

	for (i = 0; i < plat->u3ports; i++) {
		IPPCWR4(plat, MTK_XHCI_U3_CTRL_P(i),
		    (IPPCRD4(plat, MTK_XHCI_U3_CTRL_P(i)) &
		    ~(CTRL_PORT_PDN|CTRL_PORT_DIS)) | CTRL_PORT_HOST_SEL);
	}

	for (i = 0; i < plat->u2ports; i++) {
		IPPCWR4(plat, MTK_XHCI_U2_CTRL_P(i),
		    (IPPCRD4(plat, MTK_XHCI_U2_CTRL_P(i)) &
		    ~(CTRL_PORT_PDN|CTRL_PORT_DIS)) | CTRL_PORT_HOST_SEL);
	}
	while (IPPCRD4(plat, MTK_XHCI_IP_PW_STS1) & (STS1_XHCI_RST|STS1_SYS125_RST|
	    STS1_REF_RST|STS1_SYSPLL_STABLE))
		mdelay(1);

	IPPCWR4(plat, MTK_XHCI_IP_PW_CTR0, IPPCRD4(plat, MTK_XHCI_IP_PW_CTR0) &
	    ~CTRL0_IP_SW_RST);

	mdelay(10);
//	//__le32 ip_pw_sts1;	0x10 6. wait to down bits 0,8,10,11
//	DELAY(10000);
}

static int xhci_usb_probe(struct udevice *dev)
{
	struct mtk_xhci_platdata *plat = dev_get_platdata(dev);
	struct mtk_xhci *ctx = dev_get_priv(dev);
	struct xhci_hcor *hcor;
	struct phy *phy;
	int i, len, ret;
	struct udevice *regulator;

	ret = device_get_supply_regulator(dev, "vbus-supply", &regulator);
	if (!ret) {
		ret = regulator_set_enable(regulator, true);
		if (ret) {
			printf("Failed to turn ON the VBUS regulator\n");
			return ret;
		}
	}

	mtk_xhci_enable_core(plat);

	len = plat->u3ports + plat->u2ports;

	for (i = 0; i < len; i ++) {
		printf("%s:%d generic_phy_get_by_index(%d)\n", __func__, __LINE__, i);
		ret = generic_phy_get_by_index(dev, i, &ctx->phy);
		if (ret)
			return ret;

		printf("%s:%d generic_phy_init\n", __func__, __LINE__);
		ret = generic_phy_init(&ctx->phy);
		if (ret)
			return ret;

		printf("%s:%d generic_phy_power_on\n", __func__, __LINE__);
		ret = generic_phy_power_on(&ctx->phy);
		if (ret)
			return ret;
	}

	ctx->hcd = (struct xhci_hccr *)plat->hcd_base;
	len = HC_LENGTH(xhci_readl(&ctx->hcd->cr_capbase));
	hcor = (struct xhci_hcor *)((uintptr_t)ctx->hcd + len);

	return xhci_register(dev, ctx->hcd, hcor);
}

static int xhci_usb_ofdata_to_platdata(struct udevice *dev)
{
	struct mtk_xhci_platdata *plat = dev_get_platdata(dev);

//	plat->hcd_base = devfdt_get_addr(dev);
	plat->hcd_base = dev_read_addr_name(dev, "mac");
	if (plat->hcd_base == FDT_ADDR_T_NONE) {
		debug("Can't get the XHCI register base address\n");
		return -ENXIO;
	}

	plat->ippc_base = dev_read_addr_name(dev, "ippc");
	if (plat->ippc_base == FDT_ADDR_T_NONE) {
		debug("Can't get the XHCI register base address\n");
		return -ENXIO;
	}

	return 0;
}

static const struct udevice_id xhci_usb_ids[] = {
	{ .compatible = "mediatek,mtk-xhci" },
	{ }
};

U_BOOT_DRIVER(usb_xhci) = {
	.name	= "xhci_mtk",
	.id	= UCLASS_USB,
	.of_match = xhci_usb_ids,
	.ofdata_to_platdata = xhci_usb_ofdata_to_platdata,
	.probe = xhci_usb_probe,
	.remove = xhci_deregister,
	.ops	= &xhci_usb_ops,
	.platdata_auto_alloc_size = sizeof(struct mtk_xhci_platdata),
	.priv_auto_alloc_size = sizeof(struct mtk_xhci),
	.flags	= DM_FLAG_ALLOC_PRIV_DMA,
};
