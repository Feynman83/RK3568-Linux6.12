/*
 * xbrother_gpio_driver.c
 *
 * A Linux kernel module that exports GPIOs defined in Device Tree child nodes
 * to sysfs using gpiod_export, with support for TCA6424 GPIO expander.
 * Supports directions: "input", "output", "low", "high".
 *
 * DT example:
 * xbrother-gpios {
 *     compatible = "xbrother,gpios";
 *     sysfs-link = "xbrother";
 *     status = "okay";
 *     gpio-di08 {
 *         name = "di08";
 *         gpios = <&tca6424 8 GPIO_ACTIVE_HIGH>;
 *         direction = "low";
 *     };
 *     gpio-di09 {
 *         name = "di09";
 *         gpios = <&tca6424 9 GPIO_ACTIVE_HIGH>;
 *         direction = "input";
 *     };
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
    const char *name; /* Changed to const char * */
};

struct xbrother_priv {
    struct platform_device *pdev;
    struct device *sysfs_dev;
    struct class *class;
    struct xbrother_gpio *gpios;
    int gpio_count;
};

static int xbrother_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct device_node *np = dev->of_node;
    struct device_node *cnp;
    struct xbrother_priv *priv;
    const char *sysfs_link = NULL;
    int i, ret, gpio_count = 0;

    if (!np) {
        dev_err(dev, "No device tree node found\n");
        return -ENODEV;
    }

    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->pdev = pdev;

    // Count child nodes to allocate gpio array
    for_each_child_of_node(np, cnp) {
        if (of_device_is_available(cnp))
            gpio_count++;
    }
    if (gpio_count == 0) {
        dev_err(dev, "No valid GPIO child nodes found\n");
        return -EINVAL;
    }

    priv->gpios = devm_kzalloc(dev, sizeof(struct xbrother_gpio) * gpio_count, GFP_KERNEL);
    if (!priv->gpios)
        return -ENOMEM;

    // Create class
    priv->class = class_create(CLASS_NAME);
    if (IS_ERR(priv->class)) {
        ret = PTR_ERR(priv->class);
        dev_err(dev, "Failed to create class: %d\n", ret);
        return ret;
    }

    // Create sysfs device
    priv->sysfs_dev = device_create(priv->class, dev, MKDEV(0, 0), NULL, "gpio");
    if (IS_ERR(priv->sysfs_dev)) {
        ret = PTR_ERR(priv->sysfs_dev);
        dev_err(dev, "Failed to create sysfs device: %d\n", ret);
        goto err_class;
    }

    // Create sysfs link
    ret = of_property_read_string(np, "sysfs-link", &sysfs_link);
    if (ret || !sysfs_link)
        sysfs_link = CLASS_NAME;
    ret = sysfs_create_link(NULL, &priv->sysfs_dev->kobj, sysfs_link);
    if (ret) {
        dev_err(dev, "Failed to create sysfs link '%s': %d\n", sysfs_link, ret);
        goto err_device;
    }

    // Iterate over child nodes
    i = 0;
    for_each_child_of_node(np, cnp) {
        struct xbrother_gpio *gpio;
        const char *dir_str = NULL;
        enum gpiod_flags dflags;
        bool dmc;

        if (!of_device_is_available(cnp)) {
            dev_info(dev, "Child node %s is disabled, skipping\n", cnp->name);
            continue;
        }

        if (i >= gpio_count) {
            dev_err(dev, "Unexpected child node count\n");
            ret = -EINVAL;
            goto err_sysfs;
        }
        gpio = &priv->gpios[i];

        // Get GPIO name
        ret = of_property_read_string(cnp, "name", &gpio->name);
        if (ret) {
            dev_err(dev, "Failed to read name for node %s: %d\n", cnp->name, ret);
            continue;
        }

        // Get direction
        ret = of_property_read_string(cnp, "direction", &dir_str);
        if (ret) {
            dev_err(dev, "Failed to read direction for node %s: %d\n", cnp->name, ret);
            continue;
        }

        // Set direction flags
        if (!strcmp(dir_str, "input")) {
            dflags = GPIOD_IN;
        } else if (!strcmp(dir_str, "output") || !strcmp(dir_str, "low")) {
            dflags = GPIOD_OUT_LOW;
        } else if (!strcmp(dir_str, "high")) {
            dflags = GPIOD_OUT_HIGH;
        } else {
            dev_err(dev, "Invalid direction for node %s: %s\n", cnp->name, dir_str);
            continue;
        }
        // Get GPIO descriptor
        gpio->gpiod = devm_gpiod_get_index(dev, NULL, i, dflags);
        if (IS_ERR(gpio->gpiod)) {
            ret = PTR_ERR(gpio->gpiod);
            if (ret == -EPROBE_DEFER) {
                dev_info(dev, "GPIO %s at index %d deferred, retrying later\n", gpio->name, i);
                goto err_sysfs;
            }
            dev_err(dev, "Failed to get GPIO %s at index %d: %d\n", gpio->name, i, ret);
            continue;
        }

        // Check if direction can change
        dmc = of_property_read_bool(cnp, "direction-may-change");

        // Export to sysfs
        ret = gpiod_export(gpio->gpiod, dmc);
        if (ret) {
            dev_err(dev, "Failed to export GPIO %s: %d\n", gpio->name, ret);
            continue;
        }

        // Create sysfs link
        ret = gpiod_export_link(priv->sysfs_dev, gpio->name, gpio->gpiod);
        if (ret) {
            dev_err(dev, "Failed to create GPIO link for %s: %d\n", gpio->name, ret);
            gpiod_unexport(gpio->gpiod);
            continue;
        }

        i++;
    }

    if (i == 0) {
        dev_err(dev, "No valid GPIOs exported\n");
        ret = -EINVAL;
        goto err_sysfs;
    }

    priv->gpio_count = i;
    platform_set_drvdata(pdev, priv);
    dev_info(dev, "%d GPIO(s) exported\n", i);
    return 0;

err_sysfs:
    sysfs_remove_link(NULL, sysfs_link);
err_device:
    device_destroy(priv->class, MKDEV(0, 0));
err_class:
    class_destroy(priv->class);
    return ret;
}

static void xbrother_remove(struct platform_device *pdev)
{
    struct xbrother_priv *priv = platform_get_drvdata(pdev);
    const char *sysfs_link = NULL;
    struct device_node *np = pdev->dev.of_node;
    int i;

    // Unexport GPIOs
    for (i = 0; i < priv->gpio_count; i++) {
        if (priv->gpios[i].gpiod)
            gpiod_unexport(priv->gpios[i].gpiod);
    }

    // Remove sysfs link
    of_property_read_string(np, "xbrother,sysfs-link", &sysfs_link);
    if (!sysfs_link)
        sysfs_link = CLASS_NAME;
    sysfs_remove_link(NULL, sysfs_link);

    // Clean up device and class
    device_destroy(priv->class, MKDEV(0, 0));
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

MODULE_AUTHOR("Grok");
MODULE_DESCRIPTION("Xbrother GPIO sysfs exporter with TCA6424 support");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_SOFTDEP("pre: pca953x");