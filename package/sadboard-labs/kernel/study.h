/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared transport for synchronous, bounded, manually loaded laboratories. */
#ifndef SADBOARD_KERNEL_STUDY_H
#define SADBOARD_KERNEL_STUDY_H
#include <linux/debugfs.h>
#include <linux/hardirq.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

struct sb_observation {
    u64 ns;
    const char *event;
    unsigned int value, cpu, pid, irq, disabled;
};
static DEFINE_MUTEX(sb_control_lock);
static DEFINE_SPINLOCK(sb_event_lock);
static struct dentry *sb_directory;
static struct sb_observation sb_events[128];
static unsigned int sb_count;
static u64 sb_values[24];
static const char *const *sb_fields, *const *sb_kinds;
static unsigned int sb_field_count, sb_kind_count;
static int (*sb_run)(unsigned int, bool);
static void (*sb_cleanup)(void);
enum { STUDY_KIND, STUDY_FIXED, STUDY_ACTIVE, STUDY_HELD, STUDY_ERROR, STUDY_FIRST };

static void sb_event(const char *name, unsigned int value)
{
    unsigned long flags;
    bool disabled = irqs_disabled(), irq = in_interrupt();
    spin_lock_irqsave(&sb_event_lock, flags);
    if (sb_count < ARRAY_SIZE(sb_events))
        sb_events[sb_count++] = (struct sb_observation){ktime_get_ns(), name, value,
            raw_smp_processor_id(), current->pid, irq, disabled};
    spin_unlock_irqrestore(&sb_event_lock, flags);
}

static ssize_t sb_control_write(struct file *file, const char __user *input,
                                size_t count, loff_t *position)
{
    char command[96], kind[24], variant[8], extra;
    unsigned int index;
    int error = -EINVAL;
    if (!count || count >= sizeof(command)) return -EINVAL;
    if (copy_from_user(command, input, count)) return -EFAULT;
    command[count] = 0;
    mutex_lock(&sb_control_lock);
    if (!strcmp(command, "clear\n")) {
        sb_cleanup(); error = 0;
    } else if (sscanf(command, "start %23s %7s %c", kind, variant, &extra) == 2) {
        for (index = 0; index < sb_kind_count && strcmp(kind, sb_kinds[index]); index++) {}
        if (index != sb_kind_count && (!strcmp(variant, "bug") || !strcmp(variant, "fixed"))) {
            sb_cleanup();
            memset(sb_values, 0, sizeof(sb_values)); sb_count = 0;
            sb_values[STUDY_KIND] = index; sb_values[STUDY_FIXED] = !strcmp(variant, "fixed");
            sb_values[STUDY_ACTIVE] = 1; sb_event("start", index);
            error = sb_run(index, sb_values[STUDY_FIXED]);
            sb_cleanup(); sb_values[STUDY_ACTIVE] = 0;
            if (error) sb_values[STUDY_ERROR] = -error;
            sb_event("clean", sb_values[STUDY_ERROR]);
        }
    }
    mutex_unlock(&sb_control_lock);
    return error ? error : count;
}
static int sb_status_show(struct seq_file *stream, void *unused)
{
    unsigned int i;
    mutex_lock(&sb_control_lock);
    seq_putc(stream, '{');
    for (i = 0; i < sb_field_count; i++)
        seq_printf(stream, "%s\"%s\":%llu", i ? "," : "", sb_fields[i], sb_values[i]);
    seq_puts(stream, "}\n"); mutex_unlock(&sb_control_lock);
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(sb_status);
static int sb_events_show(struct seq_file *stream, void *unused)
{
    unsigned int i;
    mutex_lock(&sb_control_lock);
    seq_puts(stream, "index,ns,event,value,cpu,pid,in_interrupt,irqs_disabled\n");
    for (i = 0; i < sb_count; i++) {
        struct sb_observation *e = &sb_events[i];
        seq_printf(stream, "%u,%llu,%s,%u,%u,%u,%u,%u\n", i, e->ns, e->event, e->value,
                   e->cpu, e->pid, e->irq, e->disabled);
    }
    mutex_unlock(&sb_control_lock); return 0;
}
DEFINE_SHOW_ATTRIBUTE(sb_events);
static const struct file_operations sb_operations = {
    .owner = THIS_MODULE, .write = sb_control_write, .llseek = noop_llseek,
};
static int sb_register(const char *name, const char *const *kinds, unsigned int nk,
                        const char *const *fields, unsigned int nf,
                        int (*run)(unsigned int, bool), void (*cleanup)(void))
{
    if (nf > ARRAY_SIZE(sb_values)) return -EINVAL;
    sb_kinds = kinds; sb_kind_count = nk; sb_fields = fields; sb_field_count = nf;
    sb_run = run; sb_cleanup = cleanup;
    sb_directory = debugfs_create_dir(name, NULL);
    if (IS_ERR(sb_directory)) return PTR_ERR(sb_directory);
    debugfs_create_file("control", 0200, sb_directory, NULL, &sb_operations);
    debugfs_create_file("status", 0400, sb_directory, NULL, &sb_status_fops);
    debugfs_create_file("events", 0400, sb_directory, NULL, &sb_events_fops);
    return 0;
}
static void sb_unregister(void)
{
    debugfs_remove_recursive(sb_directory); sb_cleanup();
}
#endif
