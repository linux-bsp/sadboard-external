// SPDX-License-Identifier: GPL-2.0-only
/* Bounded manual exercises; no physical IRQ, MMIO or live driver is modified. */
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/hardirq.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

enum exercise { COALESCE, ORDERED, PREDICATE, REFERENCE, STOP };
static const char *const names[] = {"coalesce", "ordered", "predicate", "reference", "stop"};
struct metrics {
    unsigned int kind, fixed, requested, queued, coalesced, executed, processed;
    unsigned int slow_done, control_before_slow, ready, premature, delivered;
    unsigned int release_count, owner_refs, late, wait_timeout, stop_seen, active, held, error;
};
struct observation {
    u64 ns;
    const char *name;
    unsigned int value, cpu, pid, interrupt, disabled;
};
struct owned_object {
    struct kref references;
    bool released;
    unsigned int value;
};
static DEFINE_MUTEX(control_mutex);
static DEFINE_SPINLOCK(event_lock);
static struct metrics result;
static struct observation events[96];
static unsigned int event_count;
static struct dentry *directory;
static struct workqueue_struct *queue, *control_queue;
static struct work_struct blocker, work, control_work;
static struct task_struct *thread;
static struct owned_object *object;
static struct completion entered, gate, done;
static wait_queue_head_t waiters;
static atomic_t pending;
static bool condition;

static void observe(const char *name, unsigned int value)
{
    unsigned long flags;
    bool disabled = irqs_disabled(), interrupt = in_interrupt();
    spin_lock_irqsave(&event_lock, flags);
    if (event_count < ARRAY_SIZE(events)) {
        struct observation *event = &events[event_count++];
        *event = (struct observation){ktime_get_ns(), name, value, raw_smp_processor_id(),
                                     current->pid, interrupt, disabled};
    }
    spin_unlock_irqrestore(&event_lock, flags);
}

static void object_release(struct kref *reference)
{
    struct owned_object *owned = container_of(reference, struct owned_object, references);
    WRITE_ONCE(owned->released, true);
    result.release_count++;
    observe("release", 0);
    /* Quarantine until all work is joined: this demonstrates ownership, never a real UAF. */
}

static void slow_callback(struct work_struct *unused)
{
    observe("slow-enter", 0);
    complete(&entered);
    if (!wait_for_completion_timeout(&gate, 2 * HZ))
        WRITE_ONCE(result.error, ETIMEDOUT);
    WRITE_ONCE(result.slow_done, 1);
    observe("slow-exit", 1);
}

static void event_callback(struct work_struct *unused)
{
    result.executed++;
    result.processed += result.fixed ? atomic_xchg(&pending, 0) : 1;
    observe("event-work", result.processed);
}

static void control_callback(struct work_struct *unused)
{
    result.control_before_slow = !READ_ONCE(result.slow_done);
    observe("control-work", result.control_before_slow);
    complete(&done);
}

static void reference_callback(struct work_struct *unused)
{
    observe("ref-enter", 0);
    complete(&entered);
    if (!wait_for_completion_timeout(&gate, 2 * HZ))
        WRITE_ONCE(result.error, ETIMEDOUT);
    result.executed++;
    result.late = READ_ONCE(object->released);
    if (!result.late) {
        result.delivered = object->value;
        result.processed = 1;
    }
    observe("ref-use", result.late);
    if (result.fixed)
        kref_put(&object->references, object_release);
}

static int waiter_thread(void *unused)
{
    long waited;
    if (!result.fixed) {
        DEFINE_WAIT(wait);
        prepare_to_wait(&waiters, &wait, TASK_UNINTERRUPTIBLE);
        complete(&entered);
        schedule_timeout(HZ);
        finish_wait(&waiters, &wait);
    } else {
        complete(&entered);
        waited = wait_event_timeout(waiters, READ_ONCE(condition) || kthread_should_stop(), HZ);
        if (!waited)
            WRITE_ONCE(result.error, ETIMEDOUT);
    }
    result.executed = 1;
    result.premature = !READ_ONCE(condition);
    result.delivered = result.premature ? 0 : 42;
    result.processed = !result.premature;
    observe("consume", result.delivered);
    complete(&done);
    return 0;
}

static int stop_thread(void *unused)
{
    long waited;
    complete(&entered);
    if (result.fixed)
        waited = wait_event_timeout(waiters, READ_ONCE(condition) || kthread_should_stop(), HZ);
    else
        waited = wait_event_timeout(waiters, READ_ONCE(condition), HZ);
    result.wait_timeout = !waited;
    result.stop_seen = kthread_should_stop();
    result.executed = result.processed = 1;
    observe("thread-exit", result.wait_timeout);
    return 0;
}

static int start_owned_thread(int (*function)(void *))
{
    thread = kthread_create(function, NULL, "sadboard-async");
    if (IS_ERR(thread)) {
        int error = PTR_ERR(thread);
        thread = NULL;
        return error;
    }
    /* Keep the task valid even if the bounded thread function returns before stop. */
    get_task_struct(thread);
    wake_up_process(thread);
    return wait_for_completion_timeout(&entered, HZ) ? 0 : -ETIMEDOUT;
}

static void join_thread(void)
{
    if (thread) {
        kthread_stop(thread);
        put_task_struct(thread);
        thread = NULL;
    }
}

static void clear_resources(void)
{
    complete_all(&gate);
    if (thread) {
        WRITE_ONCE(condition, true);
        wake_up_all(&waiters);
        join_thread();
    }
    if (control_queue) {
        destroy_workqueue(control_queue);
        control_queue = NULL;
    }
    if (queue) {
        destroy_workqueue(queue);
        queue = NULL;
    }
    if (object) {
        while (kref_read(&object->references))
            kref_put(&object->references, object_release);
        kfree(object);
        object = NULL;
    }
    result.active = result.held = 0;
}

static int run_workqueue(void)
{
    unsigned int i;
    queue = alloc_ordered_workqueue("sadboard-async", 0);
    if (!queue)
        return -ENOMEM;
    if (result.kind == REFERENCE) {
        object = kzalloc(sizeof(*object), GFP_KERNEL);
        if (!object)
            return -ENOMEM;
        result.held = 1;
        kref_init(&object->references);
        object->value = 42;
        if (result.fixed)
            kref_get(&object->references);
        INIT_WORK(&work, reference_callback);
        result.requested = result.queued = 1;
        queue_work(queue, &work);
        if (!wait_for_completion_timeout(&entered, HZ))
            return -ETIMEDOUT;
        observe("owner-put", 0);
        kref_put(&object->references, object_release);
        result.owner_refs = kref_read(&object->references);
        complete(&gate);
        flush_workqueue(queue);
        return 0;
    }
    INIT_WORK(&blocker, slow_callback);
    queue_work(queue, &blocker);
    if (!wait_for_completion_timeout(&entered, HZ))
        return -ETIMEDOUT;
    if (result.kind == COALESCE) {
        INIT_WORK(&work, event_callback);
        result.requested = 16;
        for (i = 0; i < result.requested; i++) {
            bool accepted;
            if (result.fixed)
                atomic_inc(&pending);
            accepted = queue_work(queue, &work);
            result.queued += accepted;
            result.coalesced += !accepted;
            observe("submit", accepted);
        }
    } else {
        struct workqueue_struct *destination = queue;
        if (result.fixed) {
            control_queue = alloc_ordered_workqueue("sadboard-control", 0);
            if (!control_queue)
                return -ENOMEM;
            destination = control_queue;
        }
        INIT_WORK(&control_work, control_callback);
        result.requested = result.queued = 2;
        queue_work(destination, &control_work);
        observe("control-submit", 1);
        if (result.fixed) {
            if (!wait_for_completion_timeout(&done, HZ))
                return -ETIMEDOUT;
        } else if (wait_for_completion_timeout(&done, msecs_to_jiffies(20))) {
            return -EIO;
        }
    }
    complete(&gate);
    flush_workqueue(queue);
    if (control_queue)
        flush_workqueue(control_queue);
    if (result.kind == ORDERED)
        result.executed = result.processed = 2;
    return 0;
}

static int start(const char *name, const char *variant)
{
    unsigned int kind;
    int error;
    for (kind = 0; kind < ARRAY_SIZE(names); kind++)
        if (!strcmp(name, names[kind]))
            break;
    if (kind == ARRAY_SIZE(names) || (strcmp(variant, "bug") && strcmp(variant, "fixed")))
        return -EINVAL;
    clear_resources();
    memset(&result, 0, sizeof(result));
    event_count = 0;
    result.kind = kind;
    result.fixed = !strcmp(variant, "fixed");
    result.active = 1;
    condition = false;
    atomic_set(&pending, 0);
    reinit_completion(&entered);
    reinit_completion(&gate);
    reinit_completion(&done);
    observe("start", kind);
    if (kind == PREDICATE) {
        result.requested = 1;
        error = start_owned_thread(waiter_thread);
        if (error)
            goto out;
        observe("false-wake", 0);
        wake_up_all(&waiters);
        if (!result.fixed) {
            if (!wait_for_completion_timeout(&done, HZ)) {
                error = -ETIMEDOUT;
                goto out;
            }
        } else {
            if (wait_for_completion_timeout(&done, msecs_to_jiffies(20))) {
                error = -EIO;
                goto out;
            }
            observe("publish", 42);
            WRITE_ONCE(condition, true);
            wake_up_all(&waiters);
            if (!wait_for_completion_timeout(&done, HZ)) {
                error = -ETIMEDOUT;
                goto out;
            }
        }
        join_thread();
    } else if (kind == STOP) {
        result.requested = 1;
        error = start_owned_thread(stop_thread);
        if (error)
            goto out;
        observe("stop-request", 1);
        join_thread();
    } else {
        error = run_workqueue();
    }
out:
    result.ready = READ_ONCE(condition);
    clear_resources();
    if (error)
        result.error = -error;
    observe("clean", result.error);
    return result.error ? -(int)result.error : 0;
}

static ssize_t control_write(struct file *file, const char __user *input,
                             size_t count, loff_t *position)
{
    char command[96], name[20], variant[8], extra;
    int error = -EINVAL;
    if (!count || count >= sizeof(command))
        return -EINVAL;
    if (copy_from_user(command, input, count))
        return -EFAULT;
    command[count] = 0;
    mutex_lock(&control_mutex);
    if (!strcmp(command, "clear\n")) {
        clear_resources();
        error = 0;
    } else if (sscanf(command, "start %19s %7s %c", name, variant, &extra) == 2) {
        error = start(name, variant);
    }
    mutex_unlock(&control_mutex);
    return error ? error : count;
}

static int status_show(struct seq_file *stream, void *unused)
{
    struct metrics *r = &result;
    mutex_lock(&control_mutex);
    seq_printf(stream, "{\"kind\":%u,\"fixed\":%u,\"requested\":%u,\"queued\":%u,"
               "\"coalesced\":%u,\"executed\":%u,\"processed\":%u,\"slow_done\":%u,"
               "\"control_before_slow\":%u,\"ready\":%u,\"premature\":%u,\"delivered\":%u,"
               "\"release_count\":%u,\"owner_refs\":%u,\"late\":%u,\"wait_timeout\":%u,"
               "\"stop_seen\":%u,\"active\":%u,\"held\":%u,\"error\":%u}\n",
               r->kind, r->fixed, r->requested, r->queued, r->coalesced, r->executed,
               r->processed, r->slow_done, r->control_before_slow, r->ready, r->premature,
               r->delivered, r->release_count, r->owner_refs, r->late, r->wait_timeout,
               r->stop_seen, r->active, r->held, r->error);
    mutex_unlock(&control_mutex);
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int events_show(struct seq_file *stream, void *unused)
{
    unsigned int i;
    mutex_lock(&control_mutex);
    seq_puts(stream, "index,ns,event,value,cpu,pid,in_interrupt,irqs_disabled\n");
    for (i = 0; i < event_count; i++) {
        struct observation *e = &events[i];
        seq_printf(stream, "%u,%llu,%s,%u,%u,%u,%u,%u\n", i, e->ns, e->name,
                   e->value, e->cpu, e->pid, e->interrupt, e->disabled);
    }
    mutex_unlock(&control_mutex);
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(events);
static const struct file_operations operations = {
    .owner = THIS_MODULE, .write = control_write, .llseek = noop_llseek,
};

static int __init async_init(void)
{
    init_completion(&entered);
    init_completion(&gate);
    init_completion(&done);
    init_waitqueue_head(&waiters);
    directory = debugfs_create_dir("sadboard-async", NULL);
    if (IS_ERR(directory))
        return PTR_ERR(directory);
    debugfs_create_file("control", 0200, directory, NULL, &operations);
    debugfs_create_file("status", 0400, directory, NULL, &status_fops);
    debugfs_create_file("events", 0400, directory, NULL, &events_fops);
    return 0;
}
static void __exit async_exit(void)
{
    debugfs_remove_recursive(directory);
    clear_resources();
}
module_init(async_init);
module_exit(async_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Manual SadBoard workqueue, waitqueue, kref and kthread exercises");
