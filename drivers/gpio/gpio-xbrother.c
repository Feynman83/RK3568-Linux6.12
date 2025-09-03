/*
 * xbrother_gpio_driver.c
 *
 * A Linux kernel module that parses GPIO configurations from Device Tree,
 * requests GPIOs, configures direction and initial output level (if applicable),
 * and exposes them via sysfs under /sys/class/xbrother/<gpio_name>/value.
 *
 * DT example:
 * xbrother-gpios {
 *     compatible = "xbrother,gpios";
 *     gpios = <&gpio0 17 GPIO_ACTIVE_HIGH>,
 *             <&gpio1 18 GPIO_ACTIVE_HIGH>;
 *     gpio-names = "led1", "button1";
 *     directions = "low", "input"; // "input", "output", "low", "high"
 * };
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/slab.h>

#define DRIVER_NAME "xbrother_gpios"
#define CLASS_NAME "xbrother"

struct xbrother_gpio {
    struct gpio_desc *gpiod;
    char *name;
    bool is_output;
    struct device *dev;
};

struct xbrother_priv {
    struct platform_device *pdev;
    struct class *class;
    struct xbrother_gpio *gpios;
    int num_gpios;
};

// Sysfs attribute for value
static ssize_t value_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct xbrother_gpio *gpio = dev_get_drvdata(dev);
    int val;

    val = gpiod_get_value(gpio->gpiod);
    if (val < 0)
        return val;

    return sprintf(buf, "%d\n", val);
}

static ssize_t value_store(struct device *dev, struct device_attribute *attr,
                           const char *buf, size_t count)
{
    struct xbrother_gpio *gpio = dev_get_drvdata(dev);
    long val;
    int ret;

    if (!gpio->is_output) {
        dev_err(dev, "Cannot write to input GPIO\n");
        return -EPERM;
    }

    ret = kstrtol(buf, 10, &val);
    if (ret)
        return ret;

    gpiod_set_value(gpio->gpiod, !!val);
    return count;
}

static DEVICE_ATTR_RW(value);

static struct attribute *xbrother_attrs[] = {
    &dev_attr_value.attr,
    NULL,
};

static const struct attribute_group xbrother_group = {
    .attrs = xbrother_attrs,
};

static const struct attribute_group *xbrother_groups[] = {
    &xbrother_group,
    NULL,
};

static int xbrother_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct device_node *np = dev->of_node;
    struct xbrother_priv *priv;
    const char *dir_str;
    enum gpiod_flags dflags;
    int i, ret;

    if (!np)
        return -ENODEV;

    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->pdev = pdev;
    priv->num_gpios = of_property_count_u32_elems(np, "gpios") / 3; // Each GPIO has phandle, pin, flags
    if (priv->num_gpios <= 0) {
        dev_err(dev, "No GPIOs defined in DT\n");
        return -EINVAL;
    }

    priv->gpios = devm_kzalloc(dev, sizeof(struct xbrother_gpio) * priv->num_gpios, GFP_KERNEL);
    if (!priv->gpios)
        return -ENOMEM;

    // Create class
    priv->class = class_create(CLASS_NAME);
    if (IS_ERR(priv->class))
        return PTR_ERR(priv->class);

    for (i = 0; i < priv->num_gpios; i++) {
        struct xbrother_gpio *gpio = &priv->gpios[i];

        // Get name
        ret = of_property_read_string_index(np, "gpio-names", i, (const char **)&gpio->name);
        if (ret) {
            dev_err(dev, "Failed to read gpio-name at index %d\n", i);
            goto err_cleanup;
        }

        // Get direction
        ret = of_property_read_string_index(np, "directions", i, &dir_str);
        if (ret) {
            dev_err(dev, "Failed to read direction at index %d\n", i);
            goto err_cleanup;
        }

        // Determine direction and initial value
        if (!strcmp(dir_str, "input")) {
            gpio->is_output = false;
            dflags = GPIOD_IN;
        } else if (!strcmp(dir_str, "output") || !strcmp(dir_str, "low")) {
            gpio->is_output = true;
            dflags = GPIOD_OUT_LOW; // Default low for "output" and "low"
        } else if (!strcmp(dir_str, "high")) {
            gpio->is_output = true;
            dflags = GPIOD_OUT_HIGH;
        } else {
            dev_err(dev, "Invalid direction at index %d: %s\n", i, dir_str);
            ret = -EINVAL;
            goto err_cleanup;
        }

        // Get GPIO descriptor
        gpio->gpiod = devm_gpiod_get_index(dev, "gpios", i, dflags);
        if (IS_ERR(gpio->gpiod)) {
            ret = PTR_ERR(gpio->gpiod);
            dev_err(dev, "Failed to get GPIO %s at index %d: %d\n", gpio->name, i, ret);
            goto err_cleanup;
        }

        // Create sysfs device
        gpio->dev = device_create_with_groups(priv->class, dev, MKDEV(0, 0), gpio,
                                              xbrother_groups, "%s", gpio->name);
        if (IS_ERR(gpio->dev)) {
            ret = PTR_ERR(gpio->dev);
            dev_err(dev, "Failed to create device for %s: %d\n", gpio->name, ret);
            goto err_cleanup;
        }
    }

    platform_set_drvdata(pdev, priv);
    dev_info(dev, "Xbrother GPIO driver probed with %d GPIOs\n", priv->num_gpios);
    return 0;

err_cleanup:
    for (i = 0; i < priv->num_gpios; i++) {
        if (priv->gpios[i].dev)
            device_unregister(priv->gpios[i].dev);
    }
    class_destroy(priv->class);
    return ret;
}

static void xbrother_remove(struct platform_device *pdev)
{
    struct xbrother_priv *priv = platform_get_drvdata(pdev);
    int i;

    for (i = 0; i < priv->num_gpios; i++) {
        if (priv->gpios[i].dev)
            device_unregister(priv->gpios[i].dev);
    }
    class_destroy(priv->class);

    dev_info(&pdev->dev, "Xbrother GPIO driver removed\n");
}

static const struct of_device_id xbrother_of_match[] = {
    { .compatible = "xbrother,gpios" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, xbrother_of_match);

static struct platform_driver xbrother_driver = {
    .probe = xbrother_probe,
    .remove_new = xbrother_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = xbrother_of_match,
    },
};

module_platform_driver(xbrother_driver);

MODULE_AUTHOR("Li Feng");
MODULE_DESCRIPTION("Xbrother GPIO sysfs exporter");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");