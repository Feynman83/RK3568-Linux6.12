#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/device.h>

#define DRIVER_NAME "gpio-export"

static struct of_device_id gpio_export_ids[] = {
    { .compatible = "gpio-export" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gpio_export_ids);

static int __init of_gpio_export_probe(struct platform_device *pdev)
{
    struct device_node *np = pdev->dev.of_node;
    struct device_node *child;
    struct class *xbrother_class;
    struct device *gpio_dev;
    const char *sys_name = NULL;
    int nb = 0;
    int err;
    
    /* 创建sysfs类 - 修正参数 */
    xbrother_class = class_create("xbrother");
    if (IS_ERR(xbrother_class)) {
        err = PTR_ERR(xbrother_class);
        dev_err(&pdev->dev, "Failed to create class: %d\n", err);
        return err;
    }
    
    /* 创建设备 */
    gpio_dev = device_create(xbrother_class, NULL, 0, NULL, "gpio");
    if (IS_ERR(gpio_dev)) {
        err = PTR_ERR(gpio_dev);
        dev_err(&pdev->dev, "Failed to create device: %d\n", err);
        goto err_destroy_class;
    }
    
    /* 创建sysfs链接 */
    of_property_read_string(np, "sys_name", &sys_name);
    err = sysfs_create_link(NULL, &gpio_dev->kobj, sys_name ? sys_name : "xbrother");
    if (err) {
        dev_err(&pdev->dev, "Failed to create sysfs link: %d\n", err);
        goto err_destroy_device;
    }
    
    /* 处理每个子节点 */
    for_each_child_of_node(np, child) {
        const char *name = NULL;
        int max_gpio, i;
        
        of_property_read_string(child, "gpio-export,name", &name);
        
        // 使用gpiod_count替代of_gpio_count
        max_gpio = name ? 1 : gpiod_count(&pdev->dev, NULL);
        
        for (i = 0; i < max_gpio; i++) {
            unsigned int flags = 0;
            u32 val;
            int gpio;
            bool dmc;
            
            // 使用of_get_named_gpio_flags替代of_get_gpio_flags
            gpio = of_get_named_gpio(child, "gpios", i);
            if (!gpio_is_valid(gpio)) {
                dev_warn(&pdev->dev, "Invalid GPIO in node %s\n", 
                         of_node_full_name(child));
                continue;
            }
                 
            if (!of_property_read_u32(child, "gpio-export,output", &val))
                flags |= val ? GPIOF_OUT_INIT_HIGH : GPIOF_OUT_INIT_LOW;
            else
                flags |= GPIOF_IN;
            
            /* 请求GPIO */
            err = devm_gpio_request_one(&pdev->dev, gpio, flags, 
                                       name ? name : of_node_full_name(child));
            if (err) {
                dev_warn(&pdev->dev, "Failed to request GPIO %d: %d\n", 
                         gpio, err);
                continue;
            }
            
            /* 导出GPIO */
            dmc = of_property_read_bool(child, "gpio-export,direction_may_change");
            err = gpiod_export(gpio_to_desc(gpio), dmc); // 使用gpiod_export
            if (err) {
                dev_warn(&pdev->dev, "Failed to export GPIO %d: %d\n", 
                         gpio, err);
                continue;
            }
            
            /* 创建GPIO链接 */
            if (name) {
                err = gpiod_export_link(gpio_dev, name, gpio_to_desc(gpio)); // 使用gpiod_export_link
                if (err) {
                    dev_warn(&pdev->dev, 
                             "Failed to create export link for GPIO %d: %d\n", 
                             gpio, err);
                }
            }
            
            nb++;
        }
    }
    
    dev_info(&pdev->dev, "%d GPIO(s) exported\n", nb);
    return 0;
    
err_destroy_device:
    device_destroy(xbrother_class, 0);
err_destroy_class:
    class_destroy(xbrother_class);
    return err;
}

static struct platform_driver gpio_export_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(gpio_export_ids),
    },
};

static int __init of_gpio_export_init(void)
{
    return platform_driver_probe(&gpio_export_driver, of_gpio_export_probe);
}
late_initcall(of_gpio_export_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Li Feng");
MODULE_DESCRIPTION("GPIO Export Driver");
MODULE_VERSION("1.0");