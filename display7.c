//
// Usage:
// * Write to display:
//  echo <value> > /sys/class/display7/<display-name>/digit
//
// * Read displayed value:
// cat /sys/class/display7/<display-name>/digit
//
// <display-name> comes from device-tree 
//
//
// Parses device tree to setup GPIOS to drive the display (see NOTE below).
// Uses a custom user-space framework to drive the LED display to show
// a character between 0 and F.
//
// Framework:
// * Creates a new /sys/class (display7).
// * Creates a subdevice for a dislpay within this class.
//
//
// NOTE: 
// The driver expects one device tree subnode per each display with
// mandatory 'label' and 'gpios' properties defined.
//
// Example of a valid configuration:
//
//  seven_segment_displays {
//      compatible = "filhodamain,display7";
//
//      display7_1 {
//          label = "display7:user:1";
//          disp1-gpios = <&gpio 15 0>,	/* segment A */
//                        <&gpio 14 0>,	/* segment B */
//                        <&gpio 8 0>,	/* segment C */
//                        <&gpio 25 0>,	/* segment D */
//                        <&gpio 24 0>,	/* segment E */
//                        <&gpio 18 0>,	/* segment F */
//                        <&gpio 23 0>,	/* segment G */
//                        <&gpio 7 0>;	/* DP */
//      };
//  };
//


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>


#define DRIVER_NAME             "display7"
#define SYSCLASS_NAME           "display7"
#define DISPLAY_DEVICE_NAME     "user:1"

struct display7_data_st {
    dev_t devnum;
    char digit;
    struct gpio_descs * descs;
};
static struct display7_data_st * display7_data;

// From probe()
static struct device *parent_device = NULL;

// User-space interface:
// ----------------------------------------------
// A class to appear in /sys/class/
static struct class * display7_class = NULL;

// An "object / instance" of class display7_class
static struct device * sysfs_display7_device = NULL;
// ----------------------------------------------

// Segments:
//      a
//    +---+
//  f | g | b
//    +---+
//  e |   | c
//    +---+ * dp 
//      d 
//

// Segment codes for digits between 0 and F.
// Bit order (each bit represents a segment):
//   [dp] [g] [f] ... [b] [a]
static unsigned long segment_table[] = {
    0x3F,   // 0: f,e,d,c,b,a
    0x06,   // 1: c,b
    0x5B,   // 2: g,e,d,b,a
    0x4F,   // 3: g,d,c,b,a
    0x66,   // 4: g,f,c,b
    0x6D,   // 5: g,f,d,c,a
    0x7D,   // 6: g,f,e,d,c,a
    0x07,   // 7: c,b,a
    0x7F,   // 8: g,f,e,d,c,b,a
    0x6F,   // 9: g,f,d,c,b,a
    0x77,   // A: g,f,e,c,b,a
    0x7C,   // b; g,f,e,d,c
    0x39,   // C: f,e,d,a
    0x5E,   // d: g,e,d,c,b
    0x79,   // E: g,f,e,d,a
    0x71    // F: g,f,e,a
};

static void display7_setled(unsigned int digit)
{
    // Sanity check
    if (digit > 15)
    {
        return;
    }

    unsigned long digit_segments = segment_table[digit];
    int result = gpiod_set_array_value( display7_data->descs->ndescs,
                                    display7_data->descs->desc,
                                    display7_data->descs->info,
                                    &digit_segments);

    if (IS_ERR(result))
    {
        dev_err(parent_device, "Error setting a value in GPIOS: %d", result);
    }
}

// User space interface for "read" callbacks to special file
static ssize_t digit_show(struct device *dev,
            struct device_attribute *attr, char *buf)
{
    *buf = display7_data->digit;
    return 0;
}

// User space interface for "write" callbacks to special file
static ssize_t digit_store(struct device *dev,
        struct device_attribute *attr, const char *buf, size_t size)
{
    char digit = *buf;

    // Basic stupid conversion
    switch (digit)
    {
        case '0': display7_setled(0); break;
        case '1': display7_setled(1); break;
        case '2': display7_setled(2); break;
        case '3': display7_setled(3); break;
        case '4': display7_setled(4); break;
        case '5': display7_setled(5); break;
        case '6': display7_setled(6); break;
        case '7': display7_setled(7); break;
        case '8': display7_setled(8); break;
        case '9': display7_setled(9); break;
        case 'a': display7_setled(10); break;
        case 'b': display7_setled(11); break;
        case 'c': display7_setled(12); break;
        case 'd': display7_setled(13); break;
        case 'e': display7_setled(14); break;
        case 'f': display7_setled(15); break;
        default: digit = '8'; display7_setled(8);
    }

    display7_data->digit = digit;
    return size;
}

// Delcares 'dev_attr_digit' of type 'struct device_attribute'.
// Fills the store/show callbacks with 'digit_store()', 'digit_show()'.
static DEVICE_ATTR_RW(digit);

static int display7_probe(struct platform_device *pdev)
{
    parent_device = &pdev->dev;
    struct device_node *np = pdev->dev.of_node; // Parent
    struct device_node *child = NULL;           // Child device-tree node
    struct device * child_device;               // Child device
    int result;

    display7_data = devm_kzalloc(parent_device, sizeof(*display7_data), GFP_KERNEL);
    if (!display7_data)
    {
        return -ENOMEM;
    }

    // Get the child display device in two steps.
    // (1) Get child device tree (dt) node (we use dt nodes to traverse the device-tree)
    // (2) Since dt node is a member of device struct we can get it using container_of()
    child = of_get_next_child(np, NULL);    // DT node
    child_device = container_of(&child, struct device, of_node);    // Child device

    // Get from device tree an array of gpios named "disp1-gpios".
    // gpiod_get_array() automatically converts to the gpio descriptor form and
    // sets direction to Output and level to Low (0)
    display7_data->descs = gpiod_get_array(child_device, "disp1", GPIOD_OUT_LOW);
    if (IS_ERR(display7_data->descs))
    {
        dev_err(parent_device, "Error getting array og GPIOS: %d", display7_data->descs);
        return (int) display7_data->descs;
    }

    // Define a custom user-space interface 
    // -----------------------------------------------------------------
    // Allocate a Major Number
    // After success call, display7_data.devnum represents a unique
    // MAJOR | MINOR number
    result = alloc_chrdev_region(&display7_data->devnum, 0, 1, DRIVER_NAME);
    if (result)
    {
        dev_err(parent_device,"Failed to allocate device number");
        goto ret_err_alloc_chrdev_region;
    }

    // Create a class of devices to appear in /sys/class/
    display7_class = class_create(THIS_MODULE, SYSCLASS_NAME);
    if (IS_ERR(display7_class))
    {
        result = PTR_ERR(display7_class);
        goto ret_err_class_create;
    }

    // Create a device inside /sys/class/display7/
    sysfs_display7_device = device_create( display7_class, NULL,     /* no parent device */ 
                            display7_data->devnum, NULL,    /* no additional data */
                            DISPLAY_DEVICE_NAME );

    if (IS_ERR(sysfs_display7_device))
    {
        result = PTR_ERR(sysfs_display7_device);
        dev_err(parent_device, "Failed to create a device file!");
        goto ret_err_create_device;
    }

    // Add subfile to directory entry.
    // Echo'ing and cat'ting this file will call *_store() and *_show()
    // functions respectively.
    result = device_create_file(sysfs_display7_device, &dev_attr_digit);
    if (IS_ERR(result))
    {
        dev_err(parent_device, "Failed to create a device sub-file!");
        goto ret_err_create_device_subfile;
    }
    // ------------------------------------------------------------------

    dev_info(parent_device, "Driver initialized.");
    goto ret_ok;

ret_err_create_device_subfile:
    device_destroy(display7_class, display7_data->devnum);
ret_err_create_device:
    class_destroy(display7_class);
ret_err_class_create:
    unregister_chrdev_region(display7_data->devnum, 1);
ret_err_alloc_chrdev_region:
ret_ok:
    return result;
}

static int display7_remove(struct platform_device *pdev)
{
    device_remove_file(sysfs_display7_device, &dev_attr_digit);
    device_destroy(display7_class, display7_data->devnum);
    class_destroy(display7_class);
    unregister_chrdev_region(display7_data->devnum, 1);
    gpiod_put_array(display7_data->descs);
    dev_info(&pdev->dev, "Driver unloaded!");
    return 0;
}

static const struct of_device_id of_display7_match[] = {
    {.compatible = "filhodamain,display7"},
    {},
};

static struct platform_driver display7_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = of_display7_match,
    },
    .probe = display7_probe,
    .remove = display7_remove,
};

module_platform_driver(display7_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Andre Temprilho (filhoDaMain)");
MODULE_VERSION("1.0");
