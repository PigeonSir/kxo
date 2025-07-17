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

#define MAX_GAME 2
/* Macro DECLARE_TASKLET_OLD exists for compatibility.
 * See https://lwn.net/Articles/830964/
 */
#ifndef DECLARE_TASKLET_OLD
#define DECLARE_TASKLET_OLD(arg1, arg2) DECLARE_TASKLET(arg1, arg2, 0L)
#endif

#define DEV_NAME "kxo"

#define NR_KMLDRV 1

static int delay = 100; /* time (in ms) to generate an event */

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
    struct work_struct draw;

    /* for cdev */
    struct cdev cdev;
    struct device *dev;

    /* the lock for modifing game info */
    struct mutex lock, producer_lock, consumer_lock;

    /* for drawing */
    char draw_buffer[DRAWBUFFER_SIZE];
    DECLARE_KFIFO_PTR(rx_fifo, unsigned char);
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

/* NOTE: the usage of kfifo is safe (no need for extra locking), until there is
 * only one concurrent reader and one concurrent writer. Writes are serialized
 * from the interrupt context, readers are serialized using this mutex.
 */
static DEFINE_MUTEX(read_lock);

/* Wait queue to implement blocking I/O from userspace */
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);

/* Insert the whole chess board into the kfifo buffer */
static void produce_board(struct kxo_task *cur)
{
    unsigned int len =
        kfifo_in(&cur->rx_fifo, cur->draw_buffer, sizeof(cur->draw_buffer));
    if (unlikely(len < sizeof(cur->draw_buffer)))
        pr_warn_ratelimited("%s: %zu bytes dropped\n", __func__,
                            sizeof(cur->draw_buffer) - len);

    pr_debug("kxo: %s: in %u/%u bytes\n", __func__, len,
             kfifo_len(&cur->rx_fifo));
}

/* We use an additional "faster" circular buffer to quickly store data from
 * interrupt context, before adding them to the kfifo.
 */
static struct circ_buf fast_buf;

/* Draw the board into draw_buffer */
static int draw_board(struct kxo_task *cur)
{
    int i = 0, k = 0;
    cur->draw_buffer[i++] = '\n';
    smp_wmb();
    cur->draw_buffer[i++] = '\n';
    smp_wmb();

    while (i < DRAWBUFFER_SIZE) {
        for (int j = 0; j < (BOARD_SIZE << 1) - 1 && k < N_GRIDS; j++) {
            cur->draw_buffer[i++] = j & 1 ? '|' : cur->table[k++];
            smp_wmb();
        }
        cur->draw_buffer[i++] = '\n';
        smp_wmb();
        for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
            cur->draw_buffer[i++] = '-';
            smp_wmb();
        }
        cur->draw_buffer[i++] = '\n';
        smp_wmb();
    }


    return 0;
}

/* Clear all data from the circular buffer fast_buf */
static void fast_buf_clear(void)
{
    fast_buf.head = fast_buf.tail = 0;
}

/* Workqueue handler: executed by a kernel thread */
static void drawboard_work_func(struct work_struct *w)
{
    struct kxo_task *cur = container_of(w, struct kxo_task, draw);
    int cpu;

    /* This code runs from a kernel thread, so softirqs and hard-irqs must
     * be enabled.
     */
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    /* Pretend to simulate access to per-CPU data, disabling preemption
     * during the pr_info().
     */
    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] %s\n", cpu, __func__);
    put_cpu();

    read_lock(&attr_obj.lock);
    if (attr_obj.display == '0') {
        read_unlock(&attr_obj.lock);
        return;
    }
    read_unlock(&attr_obj.lock);

    mutex_lock(&cur->producer_lock);
    draw_board(cur);
    mutex_unlock(&cur->producer_lock);

    /* Store data to the kfifo buffer */
    mutex_lock(&cur->consumer_lock);
    produce_board(cur);
    mutex_unlock(&cur->consumer_lock);

    wake_up_interruptible(&rx_wait);
}

static char turn;
static int finish;

static void ai_one_work_func(struct work_struct *w)
{
    struct kxo_task *cur = container_of(w, struct kxo_task, ai_one);
    ktime_t tv_start, tv_end;
    s64 nsecs;

    int cpu;

    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] start doing %s\n", cpu, __func__);
    tv_start = ktime_get();
    mutex_lock(&cur->lock);
    int move;
    WRITE_ONCE(move, mcts(cur->table, 'O'));
    pr_info("kxo: [game %d] AI %c played at %d\n", cur->id, 'O', move);
    smp_mb();

    if (move != -1)
        WRITE_ONCE(cur->table[move], 'O');

    WRITE_ONCE(cur->turn, 'X');
    WRITE_ONCE(cur->finish, 1);
    smp_wmb();
    mutex_unlock(&cur->lock);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    pr_info("kxo: [CPU#%d] %s completed in %llu usec\n", cpu, __func__,
            (unsigned long long) nsecs >> 10);
    put_cpu();
}

static void ai_two_work_func(struct work_struct *w)
{
    struct kxo_task *cur = container_of(w, struct kxo_task, ai_two);
    ktime_t tv_start, tv_end;
    s64 nsecs;

    int cpu;

    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kxo: [CPU#%d game %d] start doing %s\n", cpu, cur->id, __func__);
    tv_start = ktime_get();
    mutex_lock(&cur->lock);
    int move;
    WRITE_ONCE(move, negamax_predict(cur->table, 'X').move);
    pr_info("kxo: [game %d] AI %c played at %d\n", cur->id, 'X', move);

    smp_mb();

    if (move != -1)
        WRITE_ONCE(cur->table[move], 'X');

    WRITE_ONCE(cur->turn, 'O');
    WRITE_ONCE(cur->finish, 1);
    smp_wmb();
    mutex_unlock(&cur->lock);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    pr_info("kxo: [CPU#%d game %d] %s completed in %llu usec\n", cpu, cur->id,
            __func__, (unsigned long long) nsecs >> 10);
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

    if (cur->finish && cur->turn == 'O') {
        WRITE_ONCE(cur->finish, 0);
        smp_wmb();
        queue_work(kxo_workqueue, &cur->ai_one);
    } else if (cur->finish && cur->turn == 'X') {
        WRITE_ONCE(cur->finish, 0);
        smp_wmb();
        queue_work(kxo_workqueue, &cur->ai_two);
    }
    // queue_work(kxo_workqueue, &cur->draw);
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
    // tasklet_schedule(&game_tasklet);
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
            // mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
        } else {
            read_lock(&attr_obj.lock);
            if (attr_obj.display == '1') {
                int cpu = get_cpu();
                pr_info("kxo: [CPU#%d] Drawing final board of game %d\n", cpu,
                        cur->id);
                put_cpu();

                mutex_lock(&cur->producer_lock);
                draw_board(cur);
                mutex_unlock(&cur->producer_lock);

                /* Store data to the kfifo buffer */
                mutex_lock(&cur->consumer_lock);
                produce_board(cur);
                mutex_unlock(&cur->consumer_lock);

                wake_up_interruptible(&rx_wait);
            }

            if (attr_obj.end == '0') {
                memset(cur->table, ' ',
                       N_GRIDS); /* Reset the table so the game restart */
                // mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
            }

            read_unlock(&attr_obj.lock);

            pr_info("kxo: game %d -> %c win!!!\n", cur->id, win);
        }
        tv_end = ktime_get();

        nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));
    }

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

    if (mutex_lock_interruptible(&read_lock))
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
        ret = wait_event_interruptible(rx_wait, kfifo_len(&cur->rx_fifo));
    } while (ret == 0);
    pr_debug("kxo: %s: out %u/%u bytes\n", __func__, read,
             kfifo_len(&cur->rx_fifo));

    mutex_unlock(&read_lock);

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
    pr_info("open current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static int kxo_release(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_dec_and_test(&open_cnt)) {
        del_timer_sync(&timer);
        flush_workqueue(kxo_workqueue);
        fast_buf_clear();
    }
    pr_info("release, current cnt: %d\n", atomic_read(&open_cnt));
    attr_obj.end = 48;

    return 0;
}

static const struct file_operations kxo_fops = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    .owner = THIS_MODULE,
#endif
    .read = kxo_read,
    .llseek = no_llseek,
    .open = kxo_open,
    .release = kxo_release,
};

int init_kxo_task(void)
{
    /* Setup structure for games*/
    for (int i = 0; i < MAX_GAME; i++) {
        struct kxo_task *new = &game_list[i];

        new->id = i;
        new->turn = 'O';
        new->finish = 1;
        memset(new->table, ' ', N_GRIDS);

        tasklet_init(&new->tasklet, game_tasklet_func, (unsigned long) new);
        INIT_WORK(&new->ai_one, ai_one_work_func);
        INIT_WORK(&new->ai_two, ai_two_work_func);
        INIT_WORK(&new->draw, drawboard_work_func);

        mutex_init(&new->lock);
        mutex_init(&new->producer_lock);
        mutex_init(&new->consumer_lock);

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

    /* Allocate fast circular buffer */
    fast_buf.buf = vmalloc(PAGE_SIZE);
    if (!fast_buf.buf) {
        ret = -ENOMEM;
        goto error_vmalloc;
    }

    /* Create the workqueue */
    kxo_workqueue = alloc_workqueue("kxod", WQ_UNBOUND, WQ_MAX_ACTIVE);
    if (!kxo_workqueue) {
        ret = -ENOMEM;
        goto error_workqueue;
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
error_workqueue:
    vfree(fast_buf.buf);
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
    vfree(fast_buf.buf);
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
