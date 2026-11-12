// SPDX-License-Identifier: GPL-2.0-only
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/lockdep.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>
#include <linux/srcu.h>
#include <linux/workqueue.h>
#include "study.h"

enum { OPERATIONS = STUDY_FIRST, ACCEPTED, COMPLETED, VALUE, FAILURES, LOCK_HELD,
       LOCKDEP_ENABLED, WARNINGS, READER_HELD, RETIRED_EARLY, CALLBACKS, OLD_READ, NEW_READ };
static const char *const fields[] = {"kind", "fixed", "active", "held", "error", "operations",
    "accepted", "completed", "value", "failures", "lock_held", "lockdep_enabled", "warnings",
    "reader_held", "retired_early", "callbacks", "old_read", "new_read"};
static const char *const kinds[] = {"transaction", "trylock", "assert", "readside", "srcu", "callback"};
static DEFINE_SPINLOCK(value_lock);
static DEFINE_MUTEX(business_lock);
static unsigned int shared_value;
static struct task_struct *tasks[2];
static struct completion entered[2], gate, done, retired;
static struct delayed_work release_work;
static struct srcu_struct srcu;
static bool srcu_live, fixed_path, release_reader, callback_queued;
static unsigned int active_kind;
struct owned_config { unsigned int value; bool retired; struct rcu_head callback; };
static struct owned_config *old_config, *new_config, __rcu *published;

static void release_gate(struct work_struct *unused) { complete_all(&gate); }
static int transaction_worker(void *argument)
{
    unsigned int id = (unsigned long)argument, snapshot;
    unsigned long flags;
    spin_lock_irqsave(&value_lock, flags);
    snapshot = shared_value;
    if (fixed_path) shared_value = snapshot + 1;
    spin_unlock_irqrestore(&value_lock, flags);
    sb_event("load", snapshot); complete(&entered[id]);
    if (!fixed_path) {
        if (!wait_for_completion_timeout(&gate, 2 * HZ)) return -ETIMEDOUT;
        spin_lock_irqsave(&value_lock, flags); shared_value = snapshot + 1;
        spin_unlock_irqrestore(&value_lock, flags);
    }
    sb_event("update", 1); return 0;
}
static int mutex_holder(void *unused)
{
    mutex_lock(&business_lock);
    complete(&entered[0]);
    wait_for_completion_timeout(&gate, 2 * HZ);
    mutex_unlock(&business_lock);
    return 0;
}
static void retire_callback(struct rcu_head *head)
{
    struct owned_config *config = container_of(head, struct owned_config, callback);
    WRITE_ONCE(config->retired, true); sb_values[CALLBACKS]++;
    sb_event("rcu-callback", 1); complete(&retired);
}
static int config_reader(void *unused)
{
    struct owned_config *config;
    int index = 0;
    u64 deadline;
    if (active_kind == 4) {
        index = srcu_read_lock(&srcu); config = srcu_dereference(published, &srcu);
    } else {
        rcu_read_lock(); config = rcu_dereference(published);
    }
    sb_event("reader-enter", config->value); complete(&entered[0]);
    if (active_kind == 4) {
        if (!wait_for_completion_timeout(&gate, 2 * HZ)) sb_values[FAILURES]++;
    } else {
        deadline = ktime_get_ns() + 100000000;
        while (!smp_load_acquire(&release_reader) && ktime_get_ns() < deadline) cpu_relax();
        if (!smp_load_acquire(&release_reader)) sb_values[FAILURES]++;
    }
    sb_values[RETIRED_EARLY] = READ_ONCE(config->retired);
    sb_values[OLD_READ] = config->value;
    sb_event("reader-use", sb_values[RETIRED_EARLY]);
    if (active_kind == 4) srcu_read_unlock(&srcu, index);
    else rcu_read_unlock();
    sb_event("reader-exit", 1); complete(&done); return 0;
}
static int create_task(unsigned int id, int (*function)(void *))
{
    tasks[id] = kthread_create(function, (void *)(unsigned long)id, "sadboard-sync");
    if (IS_ERR(tasks[id])) { int error = PTR_ERR(tasks[id]); tasks[id] = NULL; return error; }
    get_task_struct(tasks[id]);
    if (active_kind == 5) {
        int cpu = cpumask_any_but(cpu_online_mask, get_cpu());
        put_cpu();
        if (cpu >= nr_cpu_ids) return -ENODEV;
        kthread_bind(tasks[id], cpu);
    }
    wake_up_process(tasks[id]); return 0;
}
static void cleanup(void)
{
    unsigned int i;
    complete_all(&gate); smp_store_release(&release_reader, true);
    cancel_delayed_work_sync(&release_work);
    for (i = 0; i < 2; i++) if (tasks[i]) {
        kthread_stop(tasks[i]); put_task_struct(tasks[i]); tasks[i] = NULL;
    }
    if (callback_queued) { rcu_barrier(); callback_queued = false; }
    if (srcu_live) { cleanup_srcu_struct(&srcu); srcu_live = false; }
    if (old_config) {
        RCU_INIT_POINTER(published, NULL); synchronize_rcu();
        kfree(old_config); kfree(new_config); old_config = new_config = NULL;
    }
    sb_values[STUDY_HELD] = 0;
}
static noinline void assert_business_lock(void) { lockdep_assert_held(&business_lock); }
static int run(unsigned int kind, bool fixed)
{
    unsigned int i;
    int error;
    fixed_path = fixed; active_kind = kind; shared_value = 0; release_reader = false;
    reinit_completion(&entered[0]); reinit_completion(&entered[1]);
    reinit_completion(&gate); reinit_completion(&done); reinit_completion(&retired);
    sb_values[LOCKDEP_ENABLED] = debug_locks;
    if (!debug_locks) return -EOPNOTSUPP;
    if (kind == 0) {
        sb_values[OPERATIONS] = sb_values[ACCEPTED] = 2;
        for (i = 0; i < 2; i++) { error = create_task(i, transaction_worker); if (error) return error; }
        for (i = 0; i < 2; i++) if (!wait_for_completion_timeout(&entered[i], HZ)) return -ETIMEDOUT;
        complete_all(&gate);
        for (i = 0; i < 2; i++) { error = kthread_stop(tasks[i]); put_task_struct(tasks[i]); tasks[i] = NULL; if (error) return error; }
        sb_values[COMPLETED] = 2; sb_values[VALUE] = shared_value; sb_event("result", shared_value);
    } else if (kind == 1) {
        int acquired;
        sb_values[OPERATIONS] = 2;
        error = create_task(0, mutex_holder); if (error) return error;
        if (!wait_for_completion_timeout(&entered[0], HZ)) return -ETIMEDOUT;
        acquired = mutex_trylock(&business_lock);
        sb_event("try-busy", acquired);
        sb_values[ACCEPTED] += fixed ? acquired : !acquired;
        sb_values[FAILURES] = fixed ? 0 : !acquired;
        complete_all(&gate);
        kthread_stop(tasks[0]); put_task_struct(tasks[0]); tasks[0] = NULL;
        acquired = mutex_trylock(&business_lock); sb_event("try-free", acquired);
        sb_values[LOCK_HELD] = lockdep_is_held(&business_lock) == LOCK_STATE_HELD;
        sb_values[ACCEPTED] += fixed ? acquired : !acquired;
        sb_values[COMPLETED] = fixed && acquired;
        if (acquired) mutex_unlock(&business_lock);
        sb_event("result", sb_values[COMPLETED]);
    } else if (kind == 2) {
        if (fixed) mutex_lock(&business_lock);
        sb_values[LOCK_HELD] = lockdep_is_held(&business_lock) == LOCK_STATE_HELD;
        pr_info("sadboard-sync: ASSERT_BEGIN fixed=%u\n", fixed);
        assert_business_lock();
        pr_info("sadboard-sync: ASSERT_END fixed=%u\n", fixed);
        if (fixed) mutex_unlock(&business_lock);
        sb_values[WARNINGS] = !fixed;
        sb_event("assert", sb_values[LOCK_HELD]);
    } else if (kind == 3) {
        if (fixed) rcu_read_lock();
        sb_values[READER_HELD] = rcu_read_lock_held();
        sb_event("readside", sb_values[READER_HELD]);
        if (fixed) rcu_read_unlock();
    } else {
        old_config = kzalloc(sizeof(*old_config), GFP_KERNEL);
        new_config = kzalloc(sizeof(*new_config), GFP_KERNEL);
        if (!old_config || !new_config) { kfree(old_config); kfree(new_config); old_config = new_config = NULL; return -ENOMEM; }
        sb_values[STUDY_HELD] = 2; old_config->value = 42; new_config->value = 84;
        rcu_assign_pointer(published, old_config);
        if (kind == 4) { error = init_srcu_struct(&srcu); if (error) return error; srcu_live = true; }
        error = create_task(0, config_reader); if (error) return error;
        if (!wait_for_completion_timeout(&entered[0], HZ)) return -ETIMEDOUT;
        rcu_assign_pointer(published, new_config); sb_event("publish", 84);
        if (!fixed) { WRITE_ONCE(old_config->retired, true); sb_event("retire", 1); }
        if (kind == 4 && fixed) {
            schedule_delayed_work(&release_work, msecs_to_jiffies(20));
            synchronize_srcu(&srcu); WRITE_ONCE(old_config->retired, true); sb_event("retire", 1);
        } else {
            if (kind == 5 && fixed) { callback_queued = true; call_rcu_hurry(&old_config->callback, retire_callback); }
            complete_all(&gate); smp_store_release(&release_reader, true);
        }
        if (!wait_for_completion_timeout(&done, 5 * HZ)) return -ETIMEDOUT;
        if (callback_queued && !wait_for_completion_timeout(&retired, 5 * HZ)) return -ETIMEDOUT;
        rcu_read_lock(); sb_values[NEW_READ] = rcu_dereference(published)->value; rcu_read_unlock();
        sb_values[OPERATIONS] = sb_values[ACCEPTED] = sb_values[COMPLETED] = 1;
    }
    return 0;
}
static int __init sync_init(void)
{
    init_completion(&entered[0]); init_completion(&entered[1]); init_completion(&gate);
    init_completion(&done); init_completion(&retired); INIT_DELAYED_WORK(&release_work, release_gate);
    return sb_register("sadboard-sync", kinds, ARRAY_SIZE(kinds), fields, ARRAY_SIZE(fields), run, cleanup);
}
static void __exit sync_exit(void) { sb_unregister(); }
module_init(sync_init); module_exit(sync_exit); MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Manual lockdep, spin transaction, RCU and SRCU laboratories");
