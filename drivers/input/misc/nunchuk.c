/* Available on http://free-electrons.com/labs/solutions/uxlyjr/nunchuk.c */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/input-polldev.h>

struct nunchuk_dev {
	struct i2c_client *i2c_client;
};

static int nunchuk_read_registers(struct i2c_client *client,
				   u8 *recv)
{
	u8 buf[1];
	int ret;

	/* Ask the device to get ready for a read */

	msleep(10);

	buf[0] = 0x00;
	ret = i2c_master_send(client, buf, 1);
	if (ret != 1) {
		dev_err(&client->dev, "i2c send failed (%d)\n", ret);
		return ret < 0 ? ret : -EIO;
	}

	msleep(10);

	/* Now read registers */

	ret = i2c_master_recv(client, recv, 6);
	if (ret != 6) {
		dev_err(&client->dev, "i2c recv failed (%d)\n", ret);
		return ret < 0 ? ret : -EIO;
	}

	return 0;
}

static void nunchuk_poll(struct input_polled_dev *polled_input)
{
	u8 recv[6] = { };
	int zpressed, cpressed;

	/* Retrieve the physical i2c device */

	struct nunchuk_dev *nunchuk = polled_input->private;
	struct i2c_client *client = nunchuk->i2c_client;

	/* Get the state of the device registers */

	pr_info("%s:%i\n", __func__, __LINE__);
	if (nunchuk_read_registers(client, recv) < 0)
		return;
	pr_info("%s:%i\n", __func__, __LINE__);

	zpressed = (recv[5] & BIT(0)) ? 0 : 1;
	cpressed = (recv[5] & BIT(1)) ? 0 : 1;
	pr_info("%s:%i zpressed = %d cpressed = %d\n", __func__, __LINE__, zpressed, cpressed);

	/* Send events to the INPUT subsystem */
	input_event(polled_input->input,
		    EV_KEY, BTN_Z, zpressed);

	input_event(polled_input->input,
		    EV_KEY, BTN_C, cpressed);

	input_sync(polled_input->input);
}

static int nunchuk_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	int ret;
	u8 buf[2];

	struct input_polled_dev *polled_input;
	struct input_dev *input;
	struct nunchuk_dev *nunchuk;

	nunchuk = devm_kzalloc(&client->dev,
			       sizeof(struct nunchuk_dev),
			       GFP_KERNEL);
	if (!nunchuk) {
		dev_err(&client->dev, "Failed to allocate memory\n");
		return -ENOMEM;
	}

	/* Initialize device */

	buf[0] = 0xf0;
	buf[1] = 0x55;

	ret = i2c_master_send(client, buf, 2);
	if (ret != 2) {
		dev_err(&client->dev, "i2c send failed (%d)\n", ret);
		return ret < 0 ? ret : -EIO;
	}

	msleep(1);

	buf[0] = 0xfb;
	buf[1] = 0x00;

	ret = i2c_master_send(client, buf, 2);
	if (ret != 2) {
		dev_err(&client->dev, "i2c send failed (%d)\n", ret);
		return ret < 0 ? ret : -EIO;
	}

	/* Register input device */

	polled_input = devm_input_allocate_polled_device(&client->dev);
	if (!polled_input) {
		dev_err(&client->dev, "Failed to allocate memory\n");
		return -ENOMEM;
	}

	/* Implement pointers from logical to physical
	 * Here, no need for physical to logical pointers
	 * as unregistering and freeing the polled_input device
	 * will be automatic
	 */

	nunchuk->i2c_client = client;
	polled_input->private = nunchuk;

	/* Configure input device */

	input = polled_input->input;
	input->name = "Wii Nunchuk";
	input->id.bustype = BUS_I2C;

	set_bit(EV_KEY, input->evbit);
	set_bit(BTN_C, input->keybit);
	set_bit(BTN_Z, input->keybit);

	polled_input->poll = nunchuk_poll;
	polled_input->poll_interval = 50;

	/* Register the input device when everything is ready */

	ret = input_register_polled_device(polled_input);
	if (ret < 0) {
		dev_err(&client->dev, "cannot register input device (%d)\n",
			ret);
		return ret;
	}

	return 0;
}

static int nunchuk_remove(struct i2c_client *client)
{
	/* Nothing to do here, as the polled_input device is automatically
	 * unregistered and freed thanks to the use of
	 * devm_input_allocate_polled_device
	 */

	return 0;
}

static const struct i2c_device_id nunchuk_id[] = {
	{ "nunchuk", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, nunchuk_id);

#ifdef CONFIG_OF
static const struct of_device_id nunchuk_dt_match[] = {
	{ .compatible = "nintendo,nunchuk" },
	{ },
};
#endif

static struct i2c_driver nunchuk_driver = {
	.driver = {
		.name = "nunchuk",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(nunchuk_dt_match),
	},
	.probe = nunchuk_probe,
	.remove = nunchuk_remove,
	.id_table = nunchuk_id,
};

module_i2c_driver(nunchuk_driver);

MODULE_LICENSE("GPL");
