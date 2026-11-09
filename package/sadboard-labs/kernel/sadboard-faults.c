// SPDX-License-Identifier: GPL-2.0-only
/* Manual AM625 laboratory faults. Never registered as an autoloaded device. */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/smp.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>

#define CHUNK SZ_1M
#define MAX_CHUNKS 128
static struct platform_device *lab;
static struct dentry *directory;
static DEFINE_MUTEX(control_lock);
static void *buffers[MAX_CHUNKS];
static dma_addr_t handles[MAX_CHUNKS];
static unsigned int allocated;
static char armed[96];

static void release_buffers(void)
{
    unsigned int i;
    for (i = 0; i < allocated; ++i) {
        if (buffers[i])
            dma_free_coherent(&lab->dev, CHUNK, buffers[i], handles[i]);
        buffers[i] = NULL;
    }
    allocated = 0;
}

static int fragment(unsigned long long base, unsigned long long pages)
{
    unsigned int i, j;
    unsigned long until = jiffies + 20 * HZ;
    u64 end = base + pages * PAGE_SIZE;
    if (allocated || !pages || pages > (SZ_128M / PAGE_SIZE) || end <= base)
        return -EINVAL;
    for (i = 0; i < MAX_CHUNKS && time_before(jiffies, until); ++i) {
        dma_addr_t dma;
        void *p = dma_alloc_coherent(&lab->dev, CHUNK, &dma, GFP_KERNEL | __GFP_NOWARN);
        if (!p)
            break;
        if (dma < base || dma + CHUNK > end) {
            dma_free_coherent(&lab->dev, CHUNK, p, dma);
            break; /* Reject the page allocator fallback outside the real CMA pool. */
        }
        buffers[allocated] = p;
        handles[allocated++] = dma;
        cond_resched();
    }
    if (allocated < 32) {
        release_buffers();
        return -ENOSPC;
    }
    for (i = 0; i < allocated; ++i)
        for (j = i + 1; j < allocated; ++j)
            if (handles[i] > handles[j]) {
                dma_addr_t d = handles[i];
                void *p = buffers[i];
                handles[i] = handles[j]; buffers[i] = buffers[j];
                handles[j] = d; buffers[j] = p;
            }
    for (i = 0; i < allocated; i += 2) {
        dma_free_coherent(&lab->dev, CHUNK, buffers[i], handles[i]);
        buffers[i] = NULL;
    }
    pr_info("sadboard: CMA fragmented with %u owned 1MiB allocations, alternate holes released\n", allocated);
    return 0;
}

static void halt_cpu(void *unused)
{
    local_irq_disable();
    for (;;)
        cpu_relax();
}

static ssize_t control_write(struct file *file, const char __user *user,
                             size_t length, loff_t *position)
{
    char command[192], token[96], trailing;
    unsigned long long base, pages;
    int result = -EINVAL;
    if (!length || length >= sizeof(command))
        return -EINVAL;
    if (copy_from_user(command, user, length))
        return -EFAULT;
    command[length] = 0;
    mutex_lock(&control_lock);
    if (!strcmp(command, "release\n")) {
        release_buffers(); result = 0;
    } else if (sscanf(command, "fragment %llu %llu %c", &base, &pages, &trailing) == 2) {
        result = fragment(base, pages);
    } else if (sscanf(command, "arm %95s %c", token, &trailing) == 1) {
        strscpy(armed, token, sizeof(armed)); result = 0;
    } else if (sscanf(command, "safe %95s %c", token, &trailing) == 1) {
        /* Reference: a bounded operation with interrupts/preemption enabled. */
        msleep(2);
        pr_info("sadboard: bounded kernel reference completed, run=%s\n", token);
        result = 0;
    } else if (sscanf(command, "oops %95s %c", token, &trailing) == 1 &&
               armed[0] && !strcmp(token, armed)) {
        armed[0] = 0;
        pr_emerg("SadBoard deliberate kernel Oops: run=%s\n", token);
        BUG();
    } else if (sscanf(command, "lockup %95s %c", token, &trailing) == 1 &&
               armed[0] && !strcmp(token, armed)) {
        armed[0] = 0;
        pr_emerg("SadBoard deliberate softlockup: run=%s\n", token);
        preempt_disable();
        { unsigned long stop = jiffies + 6 * HZ;
          while (time_before(jiffies, stop)) cpu_relax(); }
        preempt_enable();
        pr_emerg("SadBoard deliberate all-CPU IRQ-off lockup: run=%s; hardware watchdog must recover\n", token);
        msleep(250);
        on_each_cpu(halt_cpu, NULL, 0);
        unreachable();
    }
    mutex_unlock(&control_lock);
    return result ? result : length;
}
static const struct file_operations control_ops = {
    .owner = THIS_MODULE, .write = control_write,
};
static int __init lab_init(void)
{
    int error;
    if (!of_machine_is_compatible("ti,am625"))
        return -ENODEV;
    lab = platform_device_register_simple("sadboard-dma-lab", -1, NULL, 0);
    if (IS_ERR(lab)) return PTR_ERR(lab);
    lab->dev.coherent_dma_mask = DMA_BIT_MASK(32);
    lab->dev.dma_mask = &lab->dev.coherent_dma_mask;
    error = dma_set_mask_and_coherent(&lab->dev, DMA_BIT_MASK(32));
    if (error) goto fail;
    directory = debugfs_create_dir("sadboard-faults", NULL);
    if (IS_ERR(directory)) { error = PTR_ERR(directory); goto fail; }
    if (IS_ERR(debugfs_create_file("control", 0200, directory, NULL, &control_ops))) {
        error = -ENOMEM; debugfs_remove_recursive(directory); goto fail;
    }
    pr_info("sadboard: manual fault module loaded, idle until an explicit root command\n");
    return 0;
fail:
    platform_device_unregister(lab); return error;
}
static void __exit lab_exit(void)
{
    debugfs_remove_recursive(directory);
    release_buffers();
    platform_device_unregister(lab);
}
module_init(lab_init);
module_exit(lab_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AM625 manual SadBoard CMA/Oops/lockup laboratory");
