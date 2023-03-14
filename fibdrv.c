#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Fibonacci engine driver");
MODULE_VERSION("0.1");

#define DEV_FIBONACCI_NAME "fibonacci"

/* MAX_LENGTH is set to 92 because
 * ssize_t can't fit the number > 92
 */
#define MAX_LENGTH 500

static dev_t fib_dev = 0;
static struct cdev *fib_cdev;
static struct class *fib_class;
static DEFINE_MUTEX(fib_mutex);
static ktime_t kt;

typedef struct BigN {
    char num[128];
} bn;

// static long long fib_sequence(long long k)
// {
//     /* FIXME: C99 variable-length array (VLA) is not allowed in Linux kernel.
//     */ long long f[k + 2];

//     f[0] = 0;
//     f[1] = 1;

//     for (int i = 2; i <= k; i++) {
//         f[i] = f[i - 1] + f[i - 2];
//     }

//     return f[k];
// }

// static long long fib_sequence(long long k)
// {
//     if (k <= 2)
//         return !!k;

//     uint8_t count = 63 - __builtin_clzll(k);
//     uint64_t n0 = 1, n1 = 1;

//     for (uint64_t i = count; i-- > 0;) {
//         uint64_t fib_2n0 = n0 * ((n1 << 1) - n0);
//         uint64_t fib_2n1 = n0 * n0 + n1 * n1;

//         if (k & (1UL << i)) {
//             n0 = fib_2n1;
//             n1 = fib_2n0 + fib_2n1;
//         } else {
//             n0 = fib_2n0;
//             n1 = fib_2n1;
//         }
//     }
//     return n0;
// }

void reverse_string(char *s, size_t size)
{
    for (int i = 0; i < size / 2; ++i) {
        s[i] = s[i] ^ s[size - i - 1];
        s[size - i - 1] = s[i] ^ s[size - i - 1];
        s[i] = s[i] ^ s[size - i - 1];
    }
}

static void string_add(char *a, char *b, char *out)
{
    int size_a = strlen(a), size_b = strlen(b);
    int index = 0, carry = 0;
    for (index = 0; index < size_a; ++index) {
        int tmp = (index < size_b) ? (a[index] - '0') + (b[index] - '0') + carry
                                   : (a[index] - '0') + carry;
        out[index] = '0' + tmp % 10;
        carry = tmp / 10;
    }
    if (carry) {
        out[index] = '1';
    }
    out[++index] = '\0';
}

static long long fib_sequence_bn(long long k, char *buf)
{
    bn *fib = kmalloc(sizeof(bn) * (k + 1), GFP_KERNEL);
    strncpy(fib[0].num, "0", 2);
    strncpy(fib[1].num, "1", 2);
    for (int i = 2; i <= k; ++i) {
        string_add(fib[i - 1].num, fib[i - 2].num, fib[i].num);
    }
    uint64_t size = strlen(fib[k].num);
    reverse_string(fib[k].num, size);
    __copy_to_user(buf, fib[k].num, size + 1);
    return size;
}

static int fib_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&fib_mutex)) {
        printk(KERN_ALERT "fibdrv is in use");
        return -EBUSY;
    }
    return 0;
}

static int fib_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&fib_mutex);
    return 0;
}

static long long fib_time_proxy(long long k, char *buf)
{
    kt = ktime_get();
    long long result = fib_sequence_bn(k, buf);
    kt = ktime_sub(ktime_get(), kt);

    return result;
}

/* calculate the fibonacci number at given offset */
static ssize_t fib_read(struct file *file,
                        char *buf,
                        size_t size,
                        loff_t *offset)
{
    // return (ssize_t) fib_sequence_bn(*offset, buf);
    return (ssize_t) fib_time_proxy(*offset, buf);
}

/* write operation is skipped */
static ssize_t fib_write(struct file *file,
                         const char *buf,
                         size_t size,
                         loff_t *offset)
{
    return ktime_to_ns(kt);
}

static loff_t fib_device_lseek(struct file *file, loff_t offset, int orig)
{
    loff_t new_pos = 0;
    switch (orig) {
    case 0: /* SEEK_SET: */
        new_pos = offset;
        break;
    case 1: /* SEEK_CUR: */
        new_pos = file->f_pos + offset;
        break;
    case 2: /* SEEK_END: */
        new_pos = MAX_LENGTH - offset;
        break;
    }

    if (new_pos > MAX_LENGTH)
        new_pos = MAX_LENGTH;  // max case
    if (new_pos < 0)
        new_pos = 0;        // min case
    file->f_pos = new_pos;  // This is what we'll use now
    return new_pos;
}

const struct file_operations fib_fops = {
    .owner = THIS_MODULE,
    .read = fib_read,
    .write = fib_write,
    .open = fib_open,
    .release = fib_release,
    .llseek = fib_device_lseek,
};

static int __init init_fib_dev(void)
{
    int rc = 0;

    mutex_init(&fib_mutex);

    // Let's register the device
    // This will dynamically allocate the major number
    rc = alloc_chrdev_region(&fib_dev, 0, 1, DEV_FIBONACCI_NAME);

    if (rc < 0) {
        printk(KERN_ALERT
               "Failed to register the fibonacci char device. rc = %i",
               rc);
        return rc;
    }

    fib_cdev = cdev_alloc();
    if (fib_cdev == NULL) {
        printk(KERN_ALERT "Failed to alloc cdev");
        rc = -1;
        goto failed_cdev;
    }
    fib_cdev->ops = &fib_fops;
    rc = cdev_add(fib_cdev, fib_dev, 1);

    if (rc < 0) {
        printk(KERN_ALERT "Failed to add cdev");
        rc = -2;
        goto failed_cdev;
    }

    fib_class = class_create(THIS_MODULE, DEV_FIBONACCI_NAME);

    if (!fib_class) {
        printk(KERN_ALERT "Failed to create device class");
        rc = -3;
        goto failed_class_create;
    }

    if (!device_create(fib_class, NULL, fib_dev, NULL, DEV_FIBONACCI_NAME)) {
        printk(KERN_ALERT "Failed to create device");
        rc = -4;
        goto failed_device_create;
    }
    return rc;
failed_device_create:
    class_destroy(fib_class);
failed_class_create:
    cdev_del(fib_cdev);
failed_cdev:
    unregister_chrdev_region(fib_dev, 1);
    return rc;
}

static void __exit exit_fib_dev(void)
{
    mutex_destroy(&fib_mutex);
    device_destroy(fib_class, fib_dev);
    class_destroy(fib_class);
    cdev_del(fib_cdev);
    unregister_chrdev_region(fib_dev, 1);
}

module_init(init_fib_dev);
module_exit(exit_fib_dev);
