/* kxo: A Tic-Tac-Toe Game Engine implemented as Linux kernel module */

#include <linux/cdev.h>
#include <linux/circ_buf.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "game.h"
#include "mcts.h"
#include "negamax.h"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("In-kernel Tic-Tac-Toe game engine");

/* Macro DECLARE_TASKLET_OLD exists for compatibility.
 * See https://lwn.net/Articles/830964/
 */
#ifndef DECLARE_TASKLET_OLD
#define DECLARE_TASKLET_OLD(arg1, arg2) DECLARE_TASKLET(arg1, arg2, 0L)
#endif

#define DEV_NAME "kxo"

#define NR_KMLDRV 1

static int delay = 500; /* time (in ms) to generate an event */

/* Declare kernel module attribute for sysfs */

struct kxo_attr {
    char display;
    char resume;
    char end;
    rwlock_t lock;
};

/* Declare game infomation for coroutine */
struct kxo_task {
    /* basic info of a game */
    bool playing;
    char turn;
    int finish;
    int id;
    char table[N_GRIDS];

    /* for tasklet & workqueue */
    struct tasklet_struct tasklet;
    struct work_struct ai_one;
    struct work_struct ai_two;
    struct work_struct ai;

    /* for cdev */
    struct cdev cdev;
    struct device *dev;

    /* the lock for modifing game info */
    struct mutex lock, producer_lock, consumer_lock;

    /* for drawing */
    char draw_buffer[DRAWBUFFER_SIZE];
    DECLARE_KFIFO_PTR(rx_fifo, unsigned char);
    wait_queue_head_t rx_wait;
    struct mutex read_lock;
};

/* Declare the queue of games */
static struct kxo_task game_list[MAX_GAME];

static struct kxo_attr attr_obj;

static ssize_t kxo_state_show(struct device *dev,
                              struct device_attribute *attr,
                              char *buf)
{
    read_lock(&attr_obj.lock);
    int ret = snprintf(buf, 7, "%c %c %c\n", attr_obj.display, attr_obj.resume,
                       attr_obj.end);
    read_unlock(&attr_obj.lock);
    return ret;
}

static ssize_t kxo_state_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf,
                               size_t count)
{
    write_lock(&attr_obj.lock);
    sscanf(buf, "%c %c %c", &(attr_obj.display), &(attr_obj.resume),
           &(attr_obj.end));
    write_unlock(&attr_obj.lock);
    return count;
}

static DEVICE_ATTR_RW(kxo_state);

/* Data produced by the simulated device */

/* Timer to simulate a periodic IRQ */
static struct timer_list timer;

/* Character device stuff */
static int major;
static struct class *kxo_class;

static unsigned short produce_step(int win, char turn, int id, int move)
{
    unsigned short tn = (turn == 'X') ? 1 : 0;
    if (win)
        tn ^= 1;
    unsigned short ret = ((win & 1) << 15) | ((tn & 1) << 14) |
                         ((id & 0x3F) << 8) | (move & 0xFF);
    return ret;
}

static void ai_work_func(struct work_struct *w)
{
    struct kxo_task *cur = container_of(w, struct kxo_task, ai);
    ktime_t tv_start, tv_end;
    s64 nsecs;

    int cpu;

    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kxo: [CPU#%d game#%d turn#%c] start doing %s\n", cpu, cur->id,
            cur->turn, __func__);
    tv_start = ktime_get();
    mutex_lock(&cur->producer_lock);
    int move;
    unsigned short step = 0;

    if (cur->turn == 'O') {
        WRITE_ONCE(move, mcts(cur->table, 'O'));
    } else {
        WRITE_ONCE(move, negamax_predict(cur->table, 'X').move);
    }
    pr_info("kxo: [game %d] AI %c played at %d\n", cur->id, cur->turn, move);
    smp_mb();

    if (move != -1) {
        WRITE_ONCE(cur->table[move], cur->turn);
        WRITE_ONCE(step, produce_step(0, cur->turn, cur->id, move));
        unsigned char bytes[2];
        bytes[0] = step & 0xFF;
        bytes[1] = (step >> 8) & 0xFF;
        kfifo_in(&cur->rx_fifo, bytes, 2);
        pr_info("write %4x to kfifo", step);
        wake_up_interruptible(&cur->rx_wait);
    }

    if (cur->turn == 'O') {
        WRITE_ONCE(cur->turn, 'X');
    } else {
        WRITE_ONCE(cur->turn, 'O');
    }
    WRITE_ONCE(cur->finish, 1);
    smp_wmb();
    mutex_unlock(&cur->producer_lock);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    pr_info("kxo: [CPU#%d game#%d turn#%c] %s completed in %llu usec\n", cpu,
            cur->id, cur->turn, __func__, (unsigned long long) nsecs >> 10);
    put_cpu();
}

/* Workqueue for asynchronous bottom-half processing */
static struct workqueue_struct *kxo_workqueue;

/* Tasklet handler.
 *
 * NOTE: different tasklets can run concurrently on different processors, but
 * two of the same type of tasklet cannot run simultaneously. Moreover, a
 * tasklet always runs on the same CPU that schedules it.
 */
static void game_tasklet_func(unsigned long __data)
{
    struct kxo_task *cur = (struct kxo_task *) __data;
    ktime_t tv_start, tv_end;
    s64 nsecs;

    WARN_ON_ONCE(!in_interrupt());
    WARN_ON_ONCE(!in_softirq());

    tv_start = ktime_get();

    READ_ONCE(cur->finish);
    READ_ONCE(cur->turn);
    smp_rmb();

    if (cur->finish) {
        WRITE_ONCE(cur->finish, 0);
        smp_wmb();
        queue_work(kxo_workqueue, &cur->ai);
    }

    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("kxo: [CPU#%d game %d] %s in_softirq: %llu usec\n",
            smp_processor_id(), cur->id, __func__,
            (unsigned long long) nsecs >> 10);
}

static void ai_game(struct kxo_task *cur)
{
    WARN_ON_ONCE(!irqs_disabled());

    pr_info("kxo: [CPU#%d] doing AI game\n", smp_processor_id());
    pr_info("kxo: [CPU#%d] scheduling tasklet\n", smp_processor_id());
    tasklet_schedule(&cur->tasklet);
}

static void timer_handler(struct timer_list *__timer)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    pr_info("kxo: [CPU#%d] enter %s\n", smp_processor_id(), __func__);
    /* We are using a kernel timer to simulate a hard-irq, so we must expect
     * to be in softirq context here.
     */
    WARN_ON_ONCE(!in_softirq());

    /* Disable interrupts for this CPU to simulate real interrupt context */
    local_irq_disable();
    for (int i = 0; i < MAX_GAME; i++) {
        tv_start = ktime_get();
        struct kxo_task *cur = &game_list[i];

        if (!cur->playing)
            continue;

        char win = check_win(cur->table);

        if (win == ' ') {
            ai_game(cur);
        } else {
            read_lock(&attr_obj.lock);
            if (attr_obj.display == '1') {
                mutex_lock(&cur->producer_lock);
                unsigned short final;
                WRITE_ONCE(final, produce_step(1, cur->turn, cur->id, 0));
                unsigned char bytes[2];
                bytes[0] = final & 0xFF;
                bytes[1] = (final >> 8) & 0xFF;
                kfifo_in(&cur->rx_fifo, bytes, 2);
                mutex_unlock(&cur->producer_lock);
                pr_info("write %x to kfifo", final);
                wake_up_interruptible(&cur->rx_wait);
            }

            if (attr_obj.end == '0') {
                /* Reset the table so the game restart */
                memset(cur->table, ' ', N_GRIDS);
            }

            read_unlock(&attr_obj.lock);
            pr_info("kxo: game %d -> %c win!!!\n", cur->id, win);
        }
    }
    tv_end = ktime_get();
    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    pr_info("kxo: [CPU#%d] %s in_irq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);
    mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    local_irq_enable();
}

static ssize_t kxo_read(struct file *file,
                        char __user *buf,
                        size_t count,
                        loff_t *ppos)
{
    int minor = MINOR(file_inode(file)->i_rdev);
    struct kxo_task *cur = &game_list[minor];

    unsigned int read;
    int ret;

    pr_debug("kxo: %s(%p, %zd, %lld)\n", __func__, buf, count, *ppos);

    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    if (mutex_lock_interruptible(&cur->read_lock))
        return -ERESTARTSYS;

    do {
        ret = kfifo_to_user(&cur->rx_fifo, buf, count, &read);
        if (unlikely(ret < 0))
            break;
        if (read)
            break;
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            break;
        }
        ret = wait_event_interruptible(cur->rx_wait, kfifo_len(&cur->rx_fifo));
    } while (ret == 0);

    pr_debug("kxo: %s: out %u/%u bytes\n", __func__, read,
             kfifo_len(&cur->rx_fifo));

    mutex_unlock(&cur->read_lock);

    return ret ? ret : read;
}

static atomic_t open_cnt;

static int kxo_open(struct inode *inode, struct file *filp)
{
    int minor = MINOR(inode->i_rdev);
    game_list[minor].playing = true;
    pr_debug("kxo: %s\n", __func__);
    if (atomic_inc_return(&open_cnt) == 1)
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    // pr_info("open current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static int kxo_release(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_dec_and_test(&open_cnt)) {
        del_timer_sync(&timer);
        flush_workqueue(kxo_workqueue);
    }
    // pr_info("release, current cnt: %d\n", atomic_read(&open_cnt));
    attr_obj.end = 48;

    return 0;
}

static const struct file_operations kxo_fops = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    .owner = THIS_MODULE,
#endif
    .read = kxo_read,
    .llseek = noop_llseek,
    .open = kxo_open,
    .release = kxo_release,
};

static int init_kxo_task(void)
{
    /* Setup structure for games*/
    for (int i = 0; i < MAX_GAME; i++) {
        struct kxo_task *new = &game_list[i];

        new->id = i;
        new->turn = 'O';
        new->finish = 1;
        memset(new->table, ' ', N_GRIDS);

        tasklet_init(&new->tasklet, game_tasklet_func, (unsigned long) new);
        INIT_WORK(&new->ai, ai_work_func);

        mutex_init(&new->lock);
        mutex_init(&new->producer_lock);
        mutex_init(&new->consumer_lock);
        mutex_init(&new->read_lock);

        init_waitqueue_head(&new->rx_wait);

        if (kfifo_alloc(&new->rx_fifo, PAGE_SIZE, GFP_KERNEL) < 0)
            return -ENOMEM;
    }
    return 0;
}

static int __init kxo_init(void)
{
    dev_t dev_id;
    int ret;

    ret = init_kxo_task();
    if (ret)
        return ret;

    /* Register major/minor numbers */
    ret = alloc_chrdev_region(&dev_id, 0, MAX_GAME, DEV_NAME);
    if (ret)
        goto error_alloc;
    major = MAJOR(dev_id);

    /* Create a class structure */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    kxo_class = class_create(THIS_MODULE, DEV_NAME);
#else
    kxo_class = class_create(DEV_NAME);
#endif
    if (IS_ERR(kxo_class)) {
        printk(KERN_ERR "error creating kxo class\n");
        ret = PTR_ERR(kxo_class);
        goto error_cdev;
    }

    /* Add the character device to the system */
    for (int i = 0; i < MAX_GAME; i++) {
        cdev_init(&game_list[i].cdev, &kxo_fops);
        ret = cdev_add(&game_list[i].cdev, MKDEV(major, i), 1);
        if (ret) {
            kobject_put(&game_list[i].cdev.kobj);
            goto error_region;
        }
        game_list[i].dev =
            device_create(kxo_class, NULL, MKDEV(major, i), NULL, "kxo%d", i);
    }

    /* Create the workqueue */
    kxo_workqueue = alloc_workqueue("kxod", WQ_UNBOUND, WQ_MAX_ACTIVE);
    if (!kxo_workqueue) {
        ret = -ENOMEM;
        goto error_vmalloc;
    }

    negamax_init();
    mcts_init();

    attr_obj.display = '1';
    attr_obj.resume = '1';
    attr_obj.end = '0';
    rwlock_init(&attr_obj.lock);

    /* Setup the timer */
    timer_setup(&timer, timer_handler, 0);
    atomic_set(&open_cnt, 0);

    pr_info("kxo: registered new kxo devices: %d, %d~%d\n", major, 0,
            MAX_GAME - 1);
out:
    return ret;
error_vmalloc:
    device_destroy(kxo_class, dev_id);
// error_device:
//     class_destroy(kxo_class);
error_region:
    unregister_chrdev_region(dev_id, NR_KMLDRV);
error_cdev:
    for (int i = 0; i < MAX_GAME; i++)
        cdev_del(&game_list[i].cdev);
error_alloc:
    for (int i = 0; i < MAX_GAME; i++)
        kfifo_free(&game_list[i].rx_fifo);
    goto out;
}

static void __exit kxo_exit(void)
{
    dev_t dev_id = MKDEV(major, 0);
    del_timer_sync(&timer);

    flush_workqueue(kxo_workqueue);
    destroy_workqueue(kxo_workqueue);
    for (int i = 0; i < MAX_GAME; i++) {
        tasklet_kill(&game_list[i].tasklet);
        device_destroy(kxo_class, MKDEV(major, i));
        cdev_del(&game_list[i].cdev);
    }
    class_destroy(kxo_class);
    unregister_chrdev_region(dev_id, NR_KMLDRV);
    for (int i = 0; i < MAX_GAME; i++)
        kfifo_free(&game_list[i].rx_fifo);
    pr_info("kxo: unloaded\n");
}

module_init(kxo_init);
module_exit(kxo_exit);
