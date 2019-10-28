/*
 * MediaTek AHCI SATA driver
 *
 * Copyright (c) 2017 MediaTek Inc.
 * Author: Ryder Lee <ryder.lee@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <common.h>
#include <ahci.h>
#include <dm.h>
#include <generic-phy.h>

#define DRV_NAME		"ahci-mtk"

#define SYS_CFG			0x14
#define SYS_CFG_SATA_MSK	GENMASK(31, 30)
#define SYS_CFG_SATA_EN		BIT(31)

struct mtk_ahci_plat {
	struct regmap *mode;
	u32 force_port_map;
	struct phy phy;
};

static int mtk_ahci_ofdata_to_platdata(struct udevice *dev)
{
#if 0 // example
	struct fsl_ahci_priv *priv = dev_get_priv(dev);

	priv->number = dev_read_u32_default(dev, "sata-number", -1);
	priv->flag = dev_read_u32_default(dev, "sata-fpdma", -1);

	priv->base = dev_read_addr(dev);
	if (priv->base == FDT_ADDR_T_NONE)
		return -EINVAL;
#endif

#if 0
	struct mtk_ahci_plat *plat = dev_get_priv(dev);
	struct device_node *np = dev->of_node;

	/* enable SATA function if needed */
	if (of_find_property(np, "mediatek,phy-mode", NULL)) {
		plat->mode = syscon_regmap_lookup_by_phandle(
					np, "mediatek,phy-mode");
		if (IS_ERR(plat->mode)) {
			dev_err(dev, "missing phy-mode phandle\n");
			return PTR_ERR(plat->mode);
		}

		regmap_update_bits(plat->mode, SYS_CFG, SYS_CFG_SATA_MSK,
				   SYS_CFG_SATA_EN);
	}

	of_property_read_u32(np, "ports-implemented", &hpriv->force_port_map);
#endif

	return 0;
}


static int mtk_ahci_bind(struct udevice *dev)
{
	struct udevice *scsi_dev;
	int ret;

	ret = ahci_bind_scsi(dev, &scsi_dev);
	if (ret) {
		debug("%s: Failed to bind (err=%d\n)", __func__, ret);
		return ret;
	}

	return 0;
}

static int mtk_ahci_probe(struct udevice *dev)
{
	struct mtk_ahci_plat *plat = dev_get_priv(dev);
	int ret;

printf("MTK AHCI\n");
	/* Asser/deassert all 3 assigned resets. */

		printf("%s:%d generic_phy_get_by_name(sata-phy)\n", __func__, __LINE__);
		ret = generic_phy_get_by_name(dev, "sata-phy", &plat->phy);
		if (ret)
			return ret;

		printf("%s:%d generic_phy_init\n", __func__, __LINE__);
		ret = generic_phy_init(&plat->phy);
		if (ret)
			return ret;

		printf("%s:%d generic_phy_power_on\n", __func__, __LINE__);
		ret = generic_phy_power_on(&plat->phy);
		if (ret)
			return ret;

	printf("%s:%d ahci_probe_scsi\n", __func__, __LINE__);
	ahci_probe_scsi(dev, (ulong)devfdt_get_addr_ptr(dev));

	return 0;
}

static const struct udevice_id mtk_ahci_ids[] = {
	{ .compatible = "mediatek,mtk-ahci" },
	{ }
};

U_BOOT_DRIVER(ahci_mtk_drv) = {
	.name		= "ahci_mtk",
	.id		= UCLASS_AHCI,
	.of_match	= mtk_ahci_ids,
	.ofdata_to_platdata = mtk_ahci_ofdata_to_platdata,
	.bind		= mtk_ahci_bind,
	.probe		= mtk_ahci_probe,
	.priv_auto_alloc_size = sizeof(struct mtk_ahci_plat),
};

