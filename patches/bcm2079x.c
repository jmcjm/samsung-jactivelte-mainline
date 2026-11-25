// SPDX-License-Identifier: GPL-2.0-only
/*
 * NCI driver for the Broadcom BCM2079x NFC controller family, I2C transport
 *
 * The controller speaks NCI, but every packet on the I2C bus carries a one
 * byte packet type in front of it, the same framing Broadcom uses for HCI
 * over UART on its Bluetooth parts: 0x10 for an NCI packet, 0x04 for an HCI
 * event. HCI events only appear while patch RAM firmware is downloaded,
 * which this driver does not do (yet), so they are read and dropped.
 *
 * Two lines control the controller: REG_PU ("enable") powers it, and
 * NFC_WAKE ("wake") keeps it out of its low power snooze mode while
 * asserted. NFC_WAKE is active low on every board known so far; the device
 * tree carries the polarity, so the driver only deals in asserted and
 * deasserted.
 *
 * Timings follow the stock Android HAL (libnfc-nci, halimpl/bcm2079x) and
 * the libnfc-brcm.conf shipped with the Galaxy S4 (jflte) family.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>

#include <net/nfc/nci.h>
#include <net/nfc/nci_core.h>
#include <net/nfc/nfc.h>

#define BCM2079X_DRIVER_NAME		"bcm2079x"

#define BCM2079X_PKT_TYPE_NCI		0x10
#define BCM2079X_PKT_TYPE_HCI_EVENT	0x04

/*
 * Every read starts with the packet type and three more bytes: the full
 * NCI header, or an HCI event header (event code, length) plus the first
 * payload byte.
 */
#define BCM2079X_RX_HDR_SIZE		(1 + NCI_CTRL_HDR_SIZE)
#define BCM2079X_HCI_EVENT_PLEN_OFFSET	2

/* REG_PU held low before powering up (HAL: power cycle in UPIO reset). */
#define BCM2079X_POWER_CYCLE_MS		10
/* Boot time after REG_PU goes high (HAL: POWER_ON_DELAY default). */
#define BCM2079X_POWER_ON_DELAY_MS	300
/* Settle time with NFC_WAKE released before cutting power (jflte conf). */
#define BCM2079X_PRE_POWER_OFF_DELAY_MS	1500
/*
 * Gap the controller needs after a write before it takes the next one:
 * NFC_WRITE_DELAY=20 in the jflte conf, 20 ms per 1000 bytes written, and
 * never less than the 5 ms the HAL enforces between back-to-back writes
 * (setWriteDelay()). Every packet shorter than 250 bytes gets the minimum.
 */
#define BCM2079X_WRITE_DELAY_NS_PER_BYTE	(20 * NSEC_PER_USEC)
#define BCM2079X_WRITE_DELAY_MIN_NS		(5 * NSEC_PER_MSEC)

/*
 * Failed reads in a row before the interrupt is turned off. The level
 * triggered interrupt stays up, so the retries follow each other at once:
 * this rides out a single NAK on the bit-banged bus, while a controller
 * that stopped answering would otherwise retrigger the thread forever.
 * Whether the controller restarts at a packet boundary after a read failed
 * halfway is not documented anywhere.
 */
#define BCM2079X_MAX_RX_ERRORS		5

#define BCM2079X_NFC_PROTOCOLS	(NFC_PROTO_JEWEL_MASK | \
				 NFC_PROTO_FELICA_MASK | \
				 NFC_PROTO_ISO14443_MASK | \
				 NFC_PROTO_ISO14443_B_MASK | \
				 NFC_PROTO_ISO15693_MASK | \
				 NFC_PROTO_NFC_DEP_MASK)

struct bcm2079x {
	struct i2c_client *client;
	struct nci_dev *ndev;
	struct gpio_desc *gpiod_en;
	struct gpio_desc *gpiod_wake;

	/*
	 * Serialises bus access: a read is two I2C transfers (header, then
	 * payload) and a write must not land between them.
	 */
	struct mutex lock;
	/* Earliest time the controller accepts the next write. */
	ktime_t next_write;
	/* Failed reads in a row, reset by every good one. */
	unsigned int rx_errors;
	/* The IRQ thread disabled the interrupt after too many failed reads. */
	bool rx_fault;
	/* Scratch space for HCI event payloads that are dropped. */
	u8 drop_buf[U8_MAX];
};

static void bcm2079x_power_on(struct bcm2079x *bcm)
{
	/* The controller expects NFC_WAKE asserted when it leaves reset. */
	gpiod_set_value_cansleep(bcm->gpiod_wake, 1);
	gpiod_set_value_cansleep(bcm->gpiod_en, 0);
	msleep(BCM2079X_POWER_CYCLE_MS);
	gpiod_set_value_cansleep(bcm->gpiod_en, 1);
	msleep(BCM2079X_POWER_ON_DELAY_MS);

	bcm->next_write = ktime_get();
	bcm->rx_errors = 0;
}

static void bcm2079x_power_off(struct bcm2079x *bcm)
{
	gpiod_set_value_cansleep(bcm->gpiod_wake, 0);
	msleep(BCM2079X_PRE_POWER_OFF_DELAY_MS);
	gpiod_set_value_cansleep(bcm->gpiod_en, 0);

	/*
	 * Park NFC_WAKE asserted, the level the controller expects at the
	 * next power up and the one the vendor driver sets at probe. The
	 * HAL leaves it released instead, which drives the line high into
	 * an unpowered controller.
	 */
	gpiod_set_value_cansleep(bcm->gpiod_wake, 1);
}

static int bcm2079x_read_nci(struct bcm2079x *bcm, const u8 *hdr,
			     struct sk_buff **skb)
{
	const struct nci_ctrl_hdr *nci_hdr = (const void *)(hdr + 1);
	/* Control and data packets keep the payload length in the same byte. */
	size_t plen = nci_hdr->plen;
	int ret;

	*skb = alloc_skb(NCI_CTRL_HDR_SIZE + plen, GFP_KERNEL);
	if (!*skb)
		return -ENOMEM;

	skb_put_data(*skb, nci_hdr, NCI_CTRL_HDR_SIZE);
	if (!plen)
		return 0;

	ret = i2c_master_recv(bcm->client, skb_put(*skb, plen), plen);
	if (ret == plen)
		return 0;

	kfree_skb(*skb);
	*skb = NULL;
	return ret < 0 ? ret : -EBADMSG;
}

static int bcm2079x_drop_hci_event(struct bcm2079x *bcm, const u8 *hdr)
{
	size_t plen = hdr[BCM2079X_HCI_EVENT_PLEN_OFFSET];
	int ret;

	dev_dbg(&bcm->client->dev, "dropping HCI event 0x%02x, %zu bytes\n",
		hdr[1], plen);

	/* The header read already took the first payload byte. */
	if (plen <= 1)
		return 0;

	ret = i2c_master_recv(bcm->client, bcm->drop_buf, plen - 1);
	if (ret == plen - 1)
		return 0;

	return ret < 0 ? ret : -EBADMSG;
}

/* Reads one packet; *skb stays NULL for packets that are not NCI. */
static int bcm2079x_read(struct bcm2079x *bcm, struct sk_buff **skb)
{
	u8 hdr[BCM2079X_RX_HDR_SIZE];
	int ret;

	*skb = NULL;

	ret = i2c_master_recv(bcm->client, hdr, sizeof(hdr));
	if (ret < 0)
		return ret;
	if (ret != sizeof(hdr))
		return -EBADMSG;

	switch (hdr[0]) {
	case BCM2079X_PKT_TYPE_NCI:
		return bcm2079x_read_nci(bcm, hdr, skb);
	case BCM2079X_PKT_TYPE_HCI_EVENT:
		return bcm2079x_drop_hci_event(bcm, hdr);
	default:
		dev_err(&bcm->client->dev, "unknown packet type 0x%02x\n",
			hdr[0]);
		return -EBADMSG;
	}
}

static irqreturn_t bcm2079x_irq_thread_fn(int irq, void *data)
{
	struct bcm2079x *bcm = data;
	struct sk_buff *skb;
	int ret;

	mutex_lock(&bcm->lock);
	ret = bcm2079x_read(bcm, &skb);
	if (ret < 0) {
		dev_err_ratelimited(&bcm->client->dev, "read failed: %d\n", ret);

		/*
		 * Stop listening until the device is closed and opened again,
		 * which power cycles the controller.
		 */
		if (++bcm->rx_errors >= BCM2079X_MAX_RX_ERRORS) {
			bcm->rx_fault = true;
			disable_irq_nosync(irq);
			dev_err(&bcm->client->dev,
				"%u failed reads in a row, interrupt disabled until reopen\n",
				bcm->rx_errors);
		}
		mutex_unlock(&bcm->lock);
		return IRQ_HANDLED;
	}
	bcm->rx_errors = 0;
	mutex_unlock(&bcm->lock);

	if (skb)
		nci_recv_frame(bcm->ndev, skb);

	return IRQ_HANDLED;
}

static int bcm2079x_nci_open(struct nci_dev *ndev)
{
	struct bcm2079x *bcm = nci_get_drvdata(ndev);

	bcm2079x_power_on(bcm);
	enable_irq(bcm->client->irq);

	return 0;
}

static int bcm2079x_nci_close(struct nci_dev *ndev)
{
	struct bcm2079x *bcm = nci_get_drvdata(ndev);

	/* Waits for a running IRQ thread, none can start after this. */
	disable_irq(bcm->client->irq);

	/* Drop the extra disable depth a failed read left behind. */
	if (bcm->rx_fault) {
		bcm->rx_fault = false;
		enable_irq(bcm->client->irq);
	}

	bcm2079x_power_off(bcm);

	return 0;
}

static void bcm2079x_wait_for_write_slot(struct bcm2079x *bcm)
{
	s64 wait_us = ktime_us_delta(bcm->next_write, ktime_get());

	if (wait_us > 0)
		fsleep(wait_us);
}

static int bcm2079x_nci_send(struct nci_dev *ndev, struct sk_buff *skb)
{
	struct bcm2079x *bcm = nci_get_drvdata(ndev);
	unsigned int len;
	int ret;

	ret = skb_cow_head(skb, 1);
	if (ret)
		goto free_skb;

	*(u8 *)skb_push(skb, 1) = BCM2079X_PKT_TYPE_NCI;
	len = skb->len;

	mutex_lock(&bcm->lock);
	bcm2079x_wait_for_write_slot(bcm);
	ret = i2c_master_send(bcm->client, skb->data, len);
	bcm->next_write = ktime_add_ns(ktime_get(),
				       max_t(u64, BCM2079X_WRITE_DELAY_MIN_NS,
					     (u64)len * BCM2079X_WRITE_DELAY_NS_PER_BYTE));
	mutex_unlock(&bcm->lock);

	if (ret < 0)
		goto free_skb;
	if (ret != len) {
		ret = -EREMOTEIO;
		goto free_skb;
	}

	consume_skb(skb);
	return 0;

free_skb:
	kfree_skb(skb);
	return ret;
}

static const struct nci_ops bcm2079x_nci_ops = {
	.open = bcm2079x_nci_open,
	.close = bcm2079x_nci_close,
	.send = bcm2079x_nci_send,
};

static int bcm2079x_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct bcm2079x *bcm;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -ENODEV, "I2C_FUNC_I2C not supported\n");

	bcm = devm_kzalloc(dev, sizeof(*bcm), GFP_KERNEL);
	if (!bcm)
		return -ENOMEM;

	bcm->client = client;
	i2c_set_clientdata(client, bcm);

	ret = devm_mutex_init(dev, &bcm->lock);
	if (ret)
		return ret;

	bcm->gpiod_en = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(bcm->gpiod_en))
		return dev_err_probe(dev, PTR_ERR(bcm->gpiod_en),
				     "failed to get enable GPIO\n");

	bcm->gpiod_wake = devm_gpiod_get(dev, "wake", GPIOD_OUT_HIGH);
	if (IS_ERR(bcm->gpiod_wake))
		return dev_err_probe(dev, PTR_ERR(bcm->gpiod_wake),
				     "failed to get wake GPIO\n");

	/* One byte of headroom for the packet type sent in front of NCI. */
	bcm->ndev = nci_allocate_device(&bcm2079x_nci_ops,
					BCM2079X_NFC_PROTOCOLS, 1, 0);
	if (!bcm->ndev)
		return -ENOMEM;

	nci_set_parent_dev(bcm->ndev, dev);
	nci_set_drvdata(bcm->ndev, bcm);

	/*
	 * The trigger type comes from the device tree. The interrupt stays
	 * off until the NCI device is opened and the controller powered.
	 */
	ret = devm_request_threaded_irq(dev, client->irq, NULL,
					bcm2079x_irq_thread_fn,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					BCM2079X_DRIVER_NAME, bcm);
	if (ret) {
		dev_err_probe(dev, ret, "failed to request IRQ\n");
		goto free_ndev;
	}

	ret = nci_register_device(bcm->ndev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register NCI device\n");
		goto free_ndev;
	}

	return 0;

free_ndev:
	nci_free_device(bcm->ndev);
	return ret;
}

static void bcm2079x_remove(struct i2c_client *client)
{
	struct bcm2079x *bcm = i2c_get_clientdata(client);

	nci_unregister_device(bcm->ndev);
	nci_free_device(bcm->ndev);
}

static const struct i2c_device_id bcm2079x_id_table[] = {
	{ "bcm20794" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bcm2079x_id_table);

static const struct of_device_id bcm2079x_of_match[] = {
	{ .compatible = "brcm,bcm20794" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcm2079x_of_match);

static struct i2c_driver bcm2079x_driver = {
	.driver = {
		.name = BCM2079X_DRIVER_NAME,
		.of_match_table = bcm2079x_of_match,
	},
	.probe = bcm2079x_probe,
	.remove = bcm2079x_remove,
	.id_table = bcm2079x_id_table,
};
module_i2c_driver(bcm2079x_driver);

MODULE_DESCRIPTION("NCI driver for Broadcom BCM2079x NFC controllers (I2C)");
MODULE_LICENSE("GPL");
