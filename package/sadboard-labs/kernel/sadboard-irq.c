// SPDX-License-Identifier: GPL-2.0-only
/* Owned software IRQ source; never requests an external device's interrupt. */
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irq_work.h>
#include <linux/workqueue.h>
#include "study.h"

enum { Q_NUMBER = STUDY_FIRST, Q_TOP, Q_THREAD, Q_WORK, Q_BUSINESS, Q_HARD, Q_OFF,
       Q_PROCESS, Q_LATE, Q_REMOVED, Q_RETURN0, Q_RETURN1, Q_TOP_BUSINESS, Q_CHECKSUM };
static const char *const fields[] = {"kind", "fixed", "active", "held", "error", "irq_number",
    "top_calls", "thread_calls", "work_calls", "business", "hard_context", "irqs_off",
    "process_context", "late", "removed", "primary_return", "secondary_return", "top_business", "checksum"};
static const char *const kinds[] = {"ownership", "wake", "shared", "defer", "drain"};
struct owned_cookie { unsigned int channel; };
static struct owned_cookie cookies[2] = {{0}, {1}};
static int owned_irq;
static bool registered[2], fixed_path, logically_removed;
static unsigned int active_kind, cause;
static unsigned int payload[64];
static struct completion top_done, entered, gate, bottom_done;
static struct work_struct bottom_work;
static struct delayed_work release_work;
static struct irq_work source_work;

static void sb_irq_noop(struct irq_data *data) { }
static struct irq_chip own_chip = {
    .name = "sadboard-software", .irq_mask = sb_irq_noop, .irq_unmask = sb_irq_noop,
    .irq_ack = sb_irq_noop,
};
static void business(unsigned int count)
{
    unsigned int i;
    for (i = 0; i < count; i++) {
        sb_values[Q_BUSINESS]++;
        sb_values[Q_CHECKSUM] += READ_ONCE(payload[i]);
    }
}
static void sb_irq_deferred(struct work_struct *unused)
{
    sb_values[Q_WORK]++;
    sb_values[Q_PROCESS] += !in_interrupt() && !irqs_disabled();
    sb_event("work-enter", 1); business(64); sb_event("work-use", 64);
    complete(&bottom_done);
}
static void sb_release_irq_thread(struct work_struct *unused) { complete_all(&gate); }
static irqreturn_t sb_irq_thread(int irq, void *data)
{
    sb_values[Q_THREAD]++;
    sb_values[Q_PROCESS] += !in_interrupt() && !irqs_disabled();
    sb_event("thread-enter", 1); complete(&entered);
    if (active_kind == 4 && !wait_for_completion_timeout(&gate, 2 * HZ))
        sb_values[STUDY_ERROR] = ETIMEDOUT;
    sb_values[Q_LATE] = READ_ONCE(logically_removed);
    if (!sb_values[Q_LATE]) business(1);
    sb_event("thread-use", sb_values[Q_LATE]);
    complete(&bottom_done);
    return IRQ_HANDLED;
}
static irqreturn_t sb_irq_top(int irq, void *data)
{
    struct owned_cookie *cookie = data;
    irqreturn_t result = IRQ_HANDLED;
    sb_values[Q_TOP]++;
    sb_values[Q_HARD] += !!in_hardirq();
    sb_values[Q_OFF] += irqs_disabled();
    sb_event("top-enter", cookie->channel);
    if (active_kind == 0) {
        if (fixed_path) { business(1); sb_values[Q_TOP_BUSINESS]++; }
        else result = IRQ_NONE;
    } else if (active_kind == 1) {
        result = fixed_path ? IRQ_WAKE_THREAD : IRQ_HANDLED;
    } else if (active_kind == 2) {
        if ((cause & BIT(cookie->channel)) || (!fixed_path && cookie->channel == 1)) {
            cause &= ~BIT(cookie->channel); business(1); sb_values[Q_TOP_BUSINESS]++;
        } else result = IRQ_NONE;
    } else if (active_kind == 3) {
        if (fixed_path) schedule_work(&bottom_work);
        else { business(64); sb_values[Q_TOP_BUSINESS] = 64; }
    } else result = IRQ_WAKE_THREAD;
    sb_values[cookie->channel ? Q_RETURN1 : Q_RETURN0] = result;
    sb_event("top-return", result);
    return result;
}
static void inject_irq(struct irq_work *unused)
{
    int error = generic_handle_irq(owned_irq);
    if (error) sb_values[STUDY_ERROR] = -error;
    complete(&top_done);
}
static void cleanup(void)
{
    unsigned int i;
    complete_all(&gate);
    irq_work_sync(&source_work);
    cancel_delayed_work_sync(&release_work);
    for (i = 0; i < 2; i++) if (registered[i]) {
        free_irq(owned_irq, &cookies[i]); registered[i] = false;
    }
    cancel_work_sync(&bottom_work);
    if (owned_irq > 0) {
        irq_free_descs(owned_irq, 1); sb_event("released", owned_irq); owned_irq = 0;
    }
    sb_values[STUDY_HELD] = 0;
}
static int run(unsigned int kind, bool fixed)
{
    unsigned long flags = kind == 2 ? IRQF_SHARED : 0;
    irq_handler_t thread = NULL;
    int error;
    active_kind = kind; fixed_path = fixed; logically_removed = false; cause = 1;
    reinit_completion(&top_done); reinit_completion(&entered);
    reinit_completion(&gate); reinit_completion(&bottom_done);
    owned_irq = irq_alloc_descs(-1, 1, 1, NUMA_NO_NODE);
    if (owned_irq < 0) { error = owned_irq; owned_irq = 0; return error; }
    sb_values[Q_NUMBER] = owned_irq; sb_values[STUDY_HELD] = 1;
    irq_set_chip_and_handler_name(owned_irq, &own_chip, handle_simple_irq, "sadboard-owned");
    irq_modify_status(owned_irq, IRQ_NOREQUEST | IRQ_NOAUTOEN, IRQ_NOPROBE);
    if (kind == 1 || kind == 4) { flags |= IRQF_ONESHOT; thread = sb_irq_thread; }
    error = request_threaded_irq(owned_irq, sb_irq_top, thread, flags, "sadboard-irq-a", &cookies[0]);
    if (error) return error;
    registered[0] = true;
    if (kind == 2) {
        error = request_irq(owned_irq, sb_irq_top, IRQF_SHARED, "sadboard-irq-b", &cookies[1]);
        if (error) return error;
        registered[1] = true;
    }
    sb_event("inject", owned_irq);
    if (!irq_work_queue(&source_work)) return -EBUSY;
    if (!wait_for_completion_timeout(&top_done, HZ)) return -ETIMEDOUT;
    if (kind == 4) {
        if (!wait_for_completion_timeout(&entered, HZ)) return -ETIMEDOUT;
        if (fixed) {
            schedule_delayed_work(&release_work, msecs_to_jiffies(20));
            synchronize_irq(owned_irq);
            WRITE_ONCE(logically_removed, true); sb_event("remove", 1);
        } else {
            WRITE_ONCE(logically_removed, true); sb_event("remove", 1); complete_all(&gate);
        }
        sb_values[Q_REMOVED] = 1;
    }
    if ((kind == 1 && fixed) || kind == 4 || (kind == 3 && fixed))
        if (!wait_for_completion_timeout(&bottom_done, 2 * HZ)) return -ETIMEDOUT;
    synchronize_irq(owned_irq);
    if (kind == 3 && fixed) flush_work(&bottom_work);
    sb_event("drained", 1);
    return sb_values[STUDY_ERROR] ? -(int)sb_values[STUDY_ERROR] : 0;
}
static int __init irq_study_init(void)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(payload); i++) payload[i] = i + 1;
    init_completion(&top_done); init_completion(&entered); init_completion(&gate); init_completion(&bottom_done);
    INIT_WORK(&bottom_work, sb_irq_deferred); INIT_DELAYED_WORK(&release_work, sb_release_irq_thread);
    source_work = IRQ_WORK_INIT_HARD(inject_irq);
    return sb_register("sadboard-irq", kinds, ARRAY_SIZE(kinds), fields, ARRAY_SIZE(fields), run, cleanup);
}
static void __exit irq_study_exit(void) { sb_unregister(); }
module_init(irq_study_init); module_exit(irq_study_exit); MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Owned software source for real generic IRQ and threaded IRQ studies");
