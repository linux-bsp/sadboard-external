// SPDX-License-Identifier: GPL-2.0-only
/* Manually loaded driver-core exercises. No physical resource is accessed. */
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

enum exercise { MATCH, PROPERTY, DEFER, UNWIND, REMOVE };
static const char *const exercise_names[] = {"match", "property", "defer", "unwind", "remove"};
static DEFINE_MUTEX(control_mutex);
static struct platform_device *device;
static struct dentry *directory;
static struct delayed_work delayed;
static unsigned int kind;
static bool fixed, ready, removed;
static atomic_t probes, last_errno, allocated, released, held, completed, late;
static void *retained;
static u32 sample_rate;
static const u32 cookie = 0x53424452;

static void release_owned_buffer(void *buffer)
{
    kfree(buffer);
    atomic_inc(&released);
    atomic_dec(&held);
}

static void callback(struct work_struct *work)
{
    atomic_inc(&completed);
    if (READ_ONCE(removed))
        atomic_inc(&late);
    pr_info("sadboard-driver: delayed callback removed=%u\n", READ_ONCE(removed));
}

static int probe(struct platform_device *pdev)
{
    const u32 *marker = dev_get_platdata(&pdev->dev);
    void *buffer;
    int error;

    if (!marker || *marker != cookie)
        return -ENODEV;
    atomic_inc(&probes);
    atomic_set(&last_errno, 0);
    if (kind == PROPERTY) {
        error = device_property_read_u32(&pdev->dev, "sadboard,sample-rate", &sample_rate);
        if (error) {
            atomic_set(&last_errno, EINVAL);
            return dev_err_probe(&pdev->dev, -EINVAL, "sample-rate is required\n");
        }
    }
    if (kind == DEFER && !READ_ONCE(ready)) {
        atomic_set(&last_errno, EPROBE_DEFER);
        return dev_err_probe(&pdev->dev, -EPROBE_DEFER, "study supplier is not ready\n");
    }
    if (kind == UNWIND) {
        buffer = kzalloc(96, GFP_KERNEL);
        if (!buffer) {
            atomic_set(&last_errno, ENOMEM);
            return -ENOMEM;
        }
        atomic_inc(&allocated);
        atomic_inc(&held);
        if (fixed) {
            error = devm_add_action_or_reset(&pdev->dev, release_owned_buffer, buffer);
            if (error) {
                atomic_set(&last_errno, -error);
                return error;
            }
        } else {
            /* The laboratory owns this allocation until explicit cleanup. */
            retained = buffer;
        }
        atomic_set(&last_errno, EIO);
        return dev_err_probe(&pdev->dev, -EIO, "bounded failure after resource acquisition\n");
    }
    if (kind == REMOVE)
        schedule_delayed_work(&delayed, msecs_to_jiffies(500));
    dev_info(&pdev->dev, "study probe complete kind=%u fixed=%u\n", kind, fixed);
    return 0;
}

static void remove_device(struct platform_device *pdev)
{
    if (kind == REMOVE && fixed)
        cancel_delayed_work_sync(&delayed);
    WRITE_ONCE(removed, true);
    dev_info(&pdev->dev, "study remove complete fixed=%u\n", fixed);
}

static struct platform_driver driver = {
    .probe = probe,
    .remove = remove_device,
    .driver = {.name = "sadboard-study"},
};

static void clear_device(void)
{
    if (device) {
        platform_device_unregister(device);
        device = NULL;
    }
    /* Both variants have a safe final owner; the bad case never dereferences freed data. */
    cancel_delayed_work_sync(&delayed);
    if (retained) {
        release_owned_buffer(retained);
        retained = NULL;
    }
}

static int start(const char *name, const char *variant)
{
    static const struct property_entry properties[] = {
        PROPERTY_ENTRY_U32("sadboard,sample-rate", 100), {}
    };
    struct platform_device_info info = {
        .name = "sadboard-study", .id = PLATFORM_DEVID_NONE,
        .data = &cookie, .size_data = sizeof(cookie),
    };
    unsigned int index;

    if (device || retained || delayed_work_pending(&delayed))
        return -EBUSY;
    if (strcmp(variant, "bug") && strcmp(variant, "fixed"))
        return -EINVAL;
    for (index = 0; index < ARRAY_SIZE(exercise_names); index++)
        if (!strcmp(name, exercise_names[index]))
            break;
    if (index == ARRAY_SIZE(exercise_names))
        return -EINVAL;
    kind = index;
    fixed = !strcmp(variant, "fixed");
    WRITE_ONCE(ready, kind != DEFER);
    WRITE_ONCE(removed, false);
    sample_rate = 0;
    atomic_set(&probes, 0);
    atomic_set(&last_errno, 0);
    atomic_set(&allocated, 0);
    atomic_set(&released, 0);
    atomic_set(&held, 0);
    atomic_set(&completed, 0);
    atomic_set(&late, 0);
    if (kind == MATCH && !fixed)
        info.name = "sadboard-study-mismatch";
    if (kind == PROPERTY && fixed)
        info.properties = properties;
    device = platform_device_register_full(&info);
    if (IS_ERR(device)) {
        int error = PTR_ERR(device);
        device = NULL;
        clear_device();
        return error;
    }
    return 0;
}

static ssize_t control_write(struct file *file, const char __user *input,
                             size_t count, loff_t *position)
{
    char command[96], name[20], variant[8], extra;
    int result = -EINVAL;

    if (!count || count >= sizeof(command))
        return -EINVAL;
    if (copy_from_user(command, input, count))
        return -EFAULT;
    command[count] = 0;
    mutex_lock(&control_mutex);
    if (!strcmp(command, "clear\n")) {
        clear_device();
        result = 0;
    } else if (sscanf(command, "start %19s %7s %c", name, variant, &extra) == 2) {
        result = start(name, variant);
    } else if (!strcmp(command, "ready\n") && device && kind == DEFER) {
        WRITE_ONCE(ready, true);
        result = device_reprobe(&device->dev);
    } else if (!strcmp(command, "detach\n") && device && kind == REMOVE) {
        platform_device_unregister(device);
        device = NULL;
        result = 0;
    }
    mutex_unlock(&control_mutex);
    return result ? result : count;
}

static int status_show(struct seq_file *stream, void *unused)
{
    mutex_lock(&control_mutex);
    seq_printf(stream,
               "{\"kind\":%u,\"fixed\":%u,\"probes\":%d,\"errno\":%d,"
               "\"bound\":%u,\"ready\":%u,\"allocated\":%d,\"released\":%d,"
               "\"held\":%d,\"removed\":%u,\"completed\":%d,\"late\":%d,"
               "\"active\":%u,\"sample_rate\":%u}\n",
               kind, fixed, atomic_read(&probes), atomic_read(&last_errno),
               device ? device_is_bound(&device->dev) : 0, READ_ONCE(ready),
               atomic_read(&allocated), atomic_read(&released), atomic_read(&held),
               READ_ONCE(removed), atomic_read(&completed), atomic_read(&late),
               device != NULL, sample_rate);
    mutex_unlock(&control_mutex);
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);
static const struct file_operations control_operations = {
    .owner = THIS_MODULE, .write = control_write, .llseek = noop_llseek,
};

static int __init study_init(void)
{
    int error;
    INIT_DELAYED_WORK(&delayed, callback);
    error = platform_driver_register(&driver);
    if (error)
        return error;
    directory = debugfs_create_dir("sadboard-driver", NULL);
    if (IS_ERR(directory)) {
        platform_driver_unregister(&driver);
        return PTR_ERR(directory);
    }
    debugfs_create_file("control", 0200, directory, NULL, &control_operations);
    debugfs_create_file("status", 0400, directory, NULL, &status_fops);
    return 0;
}

static void __exit study_exit(void)
{
    debugfs_remove_recursive(directory);
    clear_device();
    platform_driver_unregister(&driver);
}
module_init(study_init);
module_exit(study_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bounded, manual SadBoard driver-core exercises");
