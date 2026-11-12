// SPDX-License-Identifier: GPL-2.0-only
/* Only a private software platform device: no physical clocks or rails. */
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/workqueue.h>
#include "study.h"
enum { P_USAGE = STUDY_FIRST, P_STATE, P_AUTO, P_RESUME, P_SUSPEND, P_API_ERROR,
       P_WORK, P_LATE, P_PAYLOAD, P_FINAL_USAGE, P_FINAL_AUTO };
static const char *const fields[] = {"kind","fixed","active","held","error","usage","suspended","auto_allowed",
    "resume_calls","suspend_calls","api_error","work_calls","late","payload","final_usage","final_auto"};
static const char *const kinds[] = {"geterror","autosuspend","workref","unwind","policy"};
static struct platform_device *owned;
static bool enabled, driver_ready, fail_resume;
static struct work_struct business_work;
static int sb_pm_resume(struct device *dev)
{
    sb_values[P_RESUME]++; sb_event("resume", fail_resume ? EIO : 0);
    return fail_resume ? -EIO : 0;
}
static int sb_pm_suspend(struct device *dev)
{
    sb_values[P_SUSPEND]++; sb_event("suspend", 0); return 0;
}
static const struct dev_pm_ops own_pm = { .runtime_resume = sb_pm_resume, .runtime_suspend = sb_pm_suspend };
static int sb_pm_probe(struct platform_device *pdev) { return 0; }
static struct platform_driver own_driver = { .probe = sb_pm_probe,
    .driver = { .name = "sadboard-runtime-pm", .pm = &own_pm } };
static void sb_pm_business(struct work_struct *unused)
{
    sb_values[P_WORK]++; sb_values[P_LATE] = pm_runtime_suspended(&owned->dev);
    if (!sb_values[P_LATE]) sb_values[P_PAYLOAD] = 42;
    sb_event("work", sb_values[P_LATE]);
}
static int await_suspend(struct device *dev)
{
    unsigned int i;
    for (i = 0; i < 200 && !pm_runtime_suspended(dev); i++) msleep(5);
    return pm_runtime_suspended(dev) ? 0 : -ETIMEDOUT;
}
static void cleanup(void)
{
    cancel_work_sync(&business_work);
    if (owned) {
        struct device *dev = &owned->dev;
        if (enabled) {
            pm_runtime_barrier(dev);
            pm_runtime_disable(dev); enabled = false;
            /* Own device, all remaining references are this experiment's. */
            if (!dev->power.runtime_auto) pm_runtime_allow(dev);
            while (atomic_read(&dev->power.usage_count) > 0) pm_runtime_put_noidle(dev);
            sb_values[P_FINAL_USAGE] = atomic_read(&dev->power.usage_count);
            sb_values[P_FINAL_AUTO] = dev->power.runtime_auto;
        }
        platform_device_unregister(owned); owned = NULL; sb_event("released", 0);
    }
    if (driver_ready) { platform_driver_unregister(&own_driver); driver_ready = false; }
    sb_values[STUDY_HELD] = 0;
}
static int run(unsigned int kind, bool fixed)
{
    struct device *dev;
    int error;
    fail_resume = kind == 0;
    error = platform_driver_register(&own_driver);
    if (error) return error;
    driver_ready = true;
    owned = platform_device_register_simple("sadboard-runtime-pm", -1, NULL, 0);
    if (IS_ERR(owned)) { error = PTR_ERR(owned); owned = NULL; return error; }
    if (!owned->dev.driver) return -ENODEV;
    dev = &owned->dev; sb_values[STUDY_HELD] = 1;
    error = pm_runtime_set_suspended(dev); if (error) return error;
    pm_runtime_set_autosuspend_delay(dev, 20); pm_runtime_use_autosuspend(dev);
    pm_runtime_enable(dev); enabled = true;
    sb_event("configured", kind);
    if (kind == 0) {
        error = fixed ? pm_runtime_resume_and_get(dev) : pm_runtime_get_sync(dev);
        sb_values[P_API_ERROR] = error < 0 ? -error : 0;
        sb_event("get", sb_values[P_API_ERROR]);
        if (error != -EIO) return -EPROTO;
    } else if (kind == 4) {
        pm_runtime_forbid(dev); sb_event("forbid", 1);
        if (fixed) { pm_runtime_allow(dev); sb_event("allow", 1); if (await_suspend(dev)) return -ETIMEDOUT; }
    } else {
        error = pm_runtime_resume_and_get(dev); sb_event("get", error < 0 ? -error : 0);
        if (error < 0) return error;
        if (kind == 1 && fixed) {
            pm_runtime_mark_last_busy(dev); pm_runtime_put_autosuspend(dev); sb_event("put", 1);
            if (await_suspend(dev)) return -ETIMEDOUT;
        } else if (kind == 2) {
            if (!fixed) { pm_runtime_put_sync_suspend(dev); sb_event("put", 1); }
            if (!schedule_work(&business_work)) return -EBUSY;
            flush_work(&business_work);
            if (fixed) { pm_runtime_put_sync_suspend(dev); sb_event("put", 1); }
        } else if (kind == 3) {
            sb_values[P_API_ERROR] = ENOSPC; sb_event("init-error", ENOSPC);
            if (fixed) { pm_runtime_put_sync_suspend(dev); sb_event("put", 1); }
        }
    }
    sb_values[P_USAGE] = atomic_read(&dev->power.usage_count);
    sb_values[P_STATE] = pm_runtime_suspended(dev);
    sb_values[P_AUTO] = dev->power.runtime_auto;
    sb_event("snapshot", sb_values[P_USAGE]);
    return 0;
}
static int __init pm_study_init(void)
{
    INIT_WORK(&business_work, sb_pm_business);
    return sb_register("sadboard-power", kinds, ARRAY_SIZE(kinds), fields, ARRAY_SIZE(fields), run, cleanup);
}
static void __exit pm_study_exit(void) { sb_unregister(); }
module_init(pm_study_init); module_exit(pm_study_exit); MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Private software device runtime-PM ownership studies");
