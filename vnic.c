// SPDX-License-Identifier: GPL-2.0-only
/*
 * vnic.c - Virtual NIC pair driver
 *
 * Two virtual network interfaces (vnic0, vnic1) connected by a software
 * wire. Packets sent on vnic0 arrive on vnic1 and vice versa, with no
 * physical hardware involved.
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/u64_stats_sync.h>
#include <linux/ethtool.h>
#include <linux/kthread.h>
#include <linux/wait.h>

#define DRIVER_NAME      "vnic"
#define DRIVER_VERSION   "0.2"
#define VNIC_NUM_DEVS    2

/*
 * TX ring — must be a power of 2 so head & MASK replaces head % SIZE.
 * WAKE_THRESH creates hysteresis: stop at 256, wake at 64, preventing
 * rapid stop/start oscillation under sustained load.
 */
#define VNIC_TX_RING_SIZE   256
#define VNIC_TX_RING_MASK   (VNIC_TX_RING_SIZE - 1)
#define VNIC_TX_WAKE_THRESH (VNIC_TX_RING_SIZE / 4)

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pavan");
MODULE_DESCRIPTION("Virtual NIC pair driver");
MODULE_VERSION(DRIVER_VERSION);

static struct net_device *vnic_devs[VNIC_NUM_DEVS];

struct vnic_pcpu_stats {
    u64 rx_packets;
    u64 rx_bytes;
    u64 tx_packets;
    u64 tx_bytes;
    u64 rx_drops;
    u64 tx_drops;
    struct u64_stats_sync syncp;
};

/*
 * vnic_tx_ring - circular TX descriptor ring
 *
 * head: next slot to fill   — written by vnic_start_xmit (softirq)
 * tail: next slot to drain  — written by vnic_tx_thread  (process context)
 *
 * Full:  head - tail == VNIC_TX_RING_SIZE
 * Empty: head == tail
 *
 * Unsigned subtraction wraps correctly at UINT_MAX with no special handling.
 * lock is held by xmit (spin_lock, BH already off) and kthread (spin_lock_bh).
 */
struct vnic_tx_ring {
    struct sk_buff    *desc[VNIC_TX_RING_SIZE];
    unsigned int       head;
    unsigned int       tail;
    spinlock_t         lock;
};

/*
 * vnic_priv - private driver data per net_device
 *
 * @peer:       RCU-protected pointer to the paired device.
 * @pcpu_stats: per-CPU counters, aggregated in ndo_get_stats64.
 * @tx_ring:    TX descriptor ring shared between xmit and the kthread.
 * @tx_thread:  kernel thread simulating the hardware DMA engine.
 * @tx_wq:      wait queue — xmit wakes the thread after enqueuing.
 */
struct vnic_priv {
    struct net_device __rcu          *peer;
    struct vnic_pcpu_stats __percpu  *pcpu_stats;
    struct vnic_tx_ring               tx_ring;
    struct task_struct               *tx_thread;
    wait_queue_head_t                 tx_wq;
};

/*
 * vnic_tx_thread - simulated DMA engine, one per device
 *
 * Drains the TX ring and forwards each skb to the peer via
 * dev_forward_skb().  Wakes the TX queue when the ring drops below
 * VNIC_TX_WAKE_THRESH, giving the stack room to enqueue more work.
 *
 * Locking: spin_lock_bh because the TX softirq (vnic_start_xmit) holds
 * spin_lock on the same ring.  _bh disables softirqs so the softirq
 * cannot preempt the kthread and deadlock on the same spinlock.
 */
static int vnic_tx_thread(void *data)
{
    struct net_device *dev  = data;
    struct vnic_priv  *priv = netdev_priv(dev);

    while (!kthread_should_stop()) {
        wait_event_interruptible(priv->tx_wq,
            READ_ONCE(priv->tx_ring.head) != READ_ONCE(priv->tx_ring.tail) ||
            kthread_should_stop());

        while (READ_ONCE(priv->tx_ring.head) != READ_ONCE(priv->tx_ring.tail)) {
            struct vnic_pcpu_stats *pcpu;
            struct net_device *peer;
            struct sk_buff *skb;
            unsigned int len;
            bool do_wake;

            spin_lock_bh(&priv->tx_ring.lock);
            if (priv->tx_ring.head == priv->tx_ring.tail) {
                spin_unlock_bh(&priv->tx_ring.lock);
                break;
            }
            skb = priv->tx_ring.desc[priv->tx_ring.tail & VNIC_TX_RING_MASK];
            priv->tx_ring.tail++;
            do_wake = netif_queue_stopped(dev) &&
                      (priv->tx_ring.head - priv->tx_ring.tail <= VNIC_TX_WAKE_THRESH);
            spin_unlock_bh(&priv->tx_ring.lock);

            /* wake_queue called outside the lock — it may trigger xmit
             * on another CPU which would try to re-acquire the lock */
            if (do_wake)
                netif_wake_queue(dev);

            len = skb->len;
            rcu_read_lock();
            peer = rcu_dereference(priv->peer);
            if (likely(peer && netif_running(peer))) {
                pr_info(DRIVER_NAME ": %s → %s, len=%u\n",
                        dev->name, peer->name, len);
                if (dev_forward_skb(peer, skb) == NET_RX_SUCCESS) {
                    pcpu = get_cpu_ptr(priv->pcpu_stats);
                    u64_stats_update_begin(&pcpu->syncp);
                    pcpu->tx_packets++;
                    pcpu->tx_bytes += len;
                    u64_stats_update_end(&pcpu->syncp);
                    put_cpu_ptr(priv->pcpu_stats);

                    pcpu = get_cpu_ptr(
                        ((struct vnic_priv *)netdev_priv(peer))->pcpu_stats);
                    u64_stats_update_begin(&pcpu->syncp);
                    pcpu->rx_packets++;
                    pcpu->rx_bytes += len;
                    u64_stats_update_end(&pcpu->syncp);
                    put_cpu_ptr(
                        ((struct vnic_priv *)netdev_priv(peer))->pcpu_stats);
                } else {
                    pr_info(DRIVER_NAME ": dev_forward_skb failed\n");
                    pcpu = get_cpu_ptr(priv->pcpu_stats);
                    u64_stats_update_begin(&pcpu->syncp);
                    pcpu->tx_drops++;
                    u64_stats_update_end(&pcpu->syncp);
                    put_cpu_ptr(priv->pcpu_stats);
                }
            } else {
                dev_kfree_skb(skb);
                pcpu = get_cpu_ptr(priv->pcpu_stats);
                u64_stats_update_begin(&pcpu->syncp);
                pcpu->tx_drops++;
                u64_stats_update_end(&pcpu->syncp);
                put_cpu_ptr(priv->pcpu_stats);
            }
            rcu_read_unlock();
        }
    }
    return 0;
}

static int vnic_open(struct net_device *dev)
{
    struct vnic_priv *priv = netdev_priv(dev);

    priv->tx_thread = kthread_run(vnic_tx_thread, dev, "%s-tx", dev->name);
    if (IS_ERR(priv->tx_thread)) {
        pr_err(DRIVER_NAME ": failed to start TX thread for %s\n", dev->name);
        return PTR_ERR(priv->tx_thread);
    }
    netif_start_queue(dev);
    netif_carrier_on(dev);
    pr_info(DRIVER_NAME ": %s up\n", dev->name);
    return 0;
}

static int vnic_stop(struct net_device *dev)
{
    struct vnic_priv *priv = netdev_priv(dev);
    struct sk_buff *skb;

    netif_stop_queue(dev);
    netif_carrier_off(dev);

    if (priv->tx_thread) {
        kthread_stop(priv->tx_thread);
        priv->tx_thread = NULL;
    }

    /* drain any skbs the kthread did not process before stopping */
    spin_lock_bh(&priv->tx_ring.lock);
    while (priv->tx_ring.tail != priv->tx_ring.head) {
        skb = priv->tx_ring.desc[priv->tx_ring.tail & VNIC_TX_RING_MASK];
        priv->tx_ring.tail++;
        dev_kfree_skb(skb);
    }
    spin_unlock_bh(&priv->tx_ring.lock);

    pr_info(DRIVER_NAME ": %s down\n", dev->name);
    return 0;
}

/*
 * vnic_start_xmit - enqueue skb onto the TX descriptor ring
 *
 * Does not forward the packet — writes the skb pointer into the ring
 * and wakes the TX thread (simulating a doorbell write to hardware).
 * If the ring is full, stops the queue to apply backpressure.
 *
 * Locking: spin_lock (not _bh) because we are already in softirq context
 * (BH disabled), so there is no risk of deadlock from a softirq preempting
 * us and re-acquiring the same lock.
 */
static netdev_tx_t vnic_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct vnic_priv *priv = netdev_priv(dev);
    struct vnic_pcpu_stats *pcpu;
    unsigned int len = skb->len;

    spin_lock(&priv->tx_ring.lock);

    if (unlikely(priv->tx_ring.head - priv->tx_ring.tail >= VNIC_TX_RING_SIZE)) {
        netif_stop_queue(dev);
        spin_unlock(&priv->tx_ring.lock);
        dev_kfree_skb(skb);
        pcpu = this_cpu_ptr(priv->pcpu_stats);
        u64_stats_update_begin(&pcpu->syncp);
        pcpu->tx_drops++;
        u64_stats_update_end(&pcpu->syncp);
        return NETDEV_TX_OK;
    }

    priv->tx_ring.desc[priv->tx_ring.head & VNIC_TX_RING_MASK] = skb;
    priv->tx_ring.head++;

    pr_info(DRIVER_NAME ": %s enqueued len=%u ring=%u/%u\n",
            dev->name, len,
            priv->tx_ring.head - priv->tx_ring.tail, VNIC_TX_RING_SIZE);

    if (priv->tx_ring.head - priv->tx_ring.tail >= VNIC_TX_RING_SIZE)
        netif_stop_queue(dev);

    spin_unlock(&priv->tx_ring.lock);

    wake_up(&priv->tx_wq);
    return NETDEV_TX_OK;
}

static void vnic_get_stats64(struct net_device *dev,
                              struct rtnl_link_stats64 *stats)
{
    struct vnic_priv *priv = netdev_priv(dev);
    int cpu;

    for_each_possible_cpu(cpu) {
        const struct vnic_pcpu_stats *pcpu;
        u64 rx_packets, rx_bytes, tx_packets, tx_bytes, rx_drops, tx_drops;
        unsigned int start;

        pcpu = per_cpu_ptr(priv->pcpu_stats, cpu);
        do {
            start      = u64_stats_fetch_begin(&pcpu->syncp);
            rx_packets = pcpu->rx_packets;
            rx_bytes   = pcpu->rx_bytes;
            tx_packets = pcpu->tx_packets;
            tx_bytes   = pcpu->tx_bytes;
            rx_drops   = pcpu->rx_drops;
            tx_drops   = pcpu->tx_drops;
        } while (u64_stats_fetch_retry(&pcpu->syncp, start));

        stats->rx_packets += rx_packets;
        stats->rx_bytes   += rx_bytes;
        stats->tx_packets += tx_packets;
        stats->tx_bytes   += tx_bytes;
        stats->rx_dropped += rx_drops;
        stats->tx_dropped += tx_drops;
    }
}

static const struct net_device_ops vnic_netdev_ops = {
    .ndo_open        = vnic_open,
    .ndo_stop        = vnic_stop,
    .ndo_start_xmit  = vnic_start_xmit,
    .ndo_get_stats64 = vnic_get_stats64,
};

static void vnic_ethtool_get_drvinfo(struct net_device *dev,
                                     struct ethtool_drvinfo *info)
{
    strscpy(info->driver,  DRIVER_NAME,    sizeof(info->driver));
    strscpy(info->version, DRIVER_VERSION, sizeof(info->version));
}

static const struct ethtool_ops vnic_ethtool_ops = {
    .get_drvinfo = vnic_ethtool_get_drvinfo,
    .get_link    = ethtool_op_get_link,
};

static void vnic_setup(struct net_device *dev)
{
    ether_setup(dev);
    dev->netdev_ops  = &vnic_netdev_ops;
    dev->ethtool_ops = &vnic_ethtool_ops;
    eth_hw_addr_random(dev);
}

static int __init vnic_init(void)
{
    int i, err;

    for (i = 0; i < VNIC_NUM_DEVS; i++) {
        struct vnic_priv *priv;
        char name[IFNAMSIZ];

        snprintf(name, IFNAMSIZ, "vnic%d", i);

        vnic_devs[i] = alloc_netdev(sizeof(struct vnic_priv), name,
                                    NET_NAME_PREDICTABLE, vnic_setup);
        if (!vnic_devs[i]) {
            pr_err(DRIVER_NAME ": failed to allocate %s\n", name);
            err = -ENOMEM;
            goto err_free;
        }

        priv = netdev_priv(vnic_devs[i]);

        priv->pcpu_stats = netdev_alloc_pcpu_stats(struct vnic_pcpu_stats);
        if (!priv->pcpu_stats) {
            pr_err(DRIVER_NAME ": failed to allocate pcpu_stats for %s\n", name);
            free_netdev(vnic_devs[i]);
            vnic_devs[i] = NULL;
            err = -ENOMEM;
            goto err_free;
        }

        spin_lock_init(&priv->tx_ring.lock);
        init_waitqueue_head(&priv->tx_wq);
        priv->tx_thread = NULL;

        err = register_netdev(vnic_devs[i]);
        if (err) {
            pr_err(DRIVER_NAME ": failed to register %s: %d\n", name, err);
            free_percpu(priv->pcpu_stats);
            free_netdev(vnic_devs[i]);
            vnic_devs[i] = NULL;
            goto err_free;
        }

        pr_info(DRIVER_NAME ": registered %s\n", name);
    }

    /* Cross-link the two devices as peers */
    RCU_INIT_POINTER(((struct vnic_priv *)netdev_priv(vnic_devs[0]))->peer, vnic_devs[1]);
    RCU_INIT_POINTER(((struct vnic_priv *)netdev_priv(vnic_devs[1]))->peer, vnic_devs[0]);

    pr_info(DRIVER_NAME ": loaded v%s\n", DRIVER_VERSION);
    return 0;

err_free:
    while (--i >= 0) {
        free_percpu(((struct vnic_priv *)netdev_priv(vnic_devs[i]))->pcpu_stats);
        unregister_netdev(vnic_devs[i]);
        free_netdev(vnic_devs[i]);
        vnic_devs[i] = NULL;
    }
    return err;
}

static void __exit vnic_exit(void)
{
    int i;

    /* Clear peer pointers before unregistering — in-flight kthread iterations
     * after this point will see peer=NULL and drop skbs safely. */
    for (i = 0; i < VNIC_NUM_DEVS; i++) {
        if (vnic_devs[i])
            RCU_INIT_POINTER(((struct vnic_priv *)netdev_priv(vnic_devs[i]))->peer, NULL);
    }
    synchronize_rcu();

    for (i = 0; i < VNIC_NUM_DEVS; i++) {
        if (vnic_devs[i]) {
            free_percpu(((struct vnic_priv *)netdev_priv(vnic_devs[i]))->pcpu_stats);
            unregister_netdev(vnic_devs[i]); /* calls vnic_stop if UP */
            free_netdev(vnic_devs[i]);
            vnic_devs[i] = NULL;
        }
    }

    pr_info(DRIVER_NAME ": unloaded\n");
}

module_init(vnic_init);
module_exit(vnic_exit);
