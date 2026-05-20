// SPDX-License-Identifier: GPL-2.0-only
/*
 * vnic.c - Virtual NIC pair driver
 *
 * Module skeleton — allocate and register two net_devices (vnic0, vnic1).
 * Packets are dropped until forwarding is implemented.
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/u64_stats_sync.h>
#include <linux/ethtool.h>

#define DRIVER_NAME    "vnic"
#define DRIVER_VERSION "0.1"
#define VNIC_NUM_DEVS  2

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pavan");
MODULE_DESCRIPTION("Virtual NIC pair driver");
MODULE_VERSION(DRIVER_VERSION);

static struct net_device *vnic_devs[VNIC_NUM_DEVS];

/*
 * vnic_pcpu_stats - per-CPU statistics
 *
 * Per-CPU avoids false sharing — each core updates its own copy.
 * u64_stats_sync provides consistent reads without disabling interrupts.
 */
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
 * vnic_priv - private driver data per net_device
 *
 * @peer:       RCU-protected pointer to the other device in the pair.
 *              RCU is used because xmit reads this on the hot TX path
 *              while exit writes NULL during teardown.
 * @pcpu_stats: per-CPU stats, aggregated by ndo_get_stats64.
 */
struct vnic_priv {
    struct net_device __rcu        *peer;
    struct vnic_pcpu_stats __percpu *pcpu_stats;
};

/*
 * vnic_open - bring the interface up
 *
 * Called when: ip link set vnic0 up
 * Starts the TX queue and sets carrier on so the kernel considers
 * the link ready to send and receive packets.
 */
static int vnic_open(struct net_device *dev)
{
    netif_start_queue(dev);
    netif_carrier_on(dev);
    pr_info(DRIVER_NAME ": %s up\n", dev->name);
    return 0;
}

/*
 * vnic_stop - bring the interface down
 *
 * Called when: ip link set vnic0 down
 * Stops the TX queue and clears carrier so the kernel stops
 * sending packets through this interface.
 */
static int vnic_stop(struct net_device *dev)
{
    netif_stop_queue(dev);
    netif_carrier_off(dev);
    pr_info(DRIVER_NAME ": %s down\n", dev->name);
    return 0;
}

/*
 * vnic_start_xmit - forward a packet to the peer device
 *
 * Looks up the peer under RCU and hands the skb to the peer's RX path
 * via dev_forward_skb(). Updates TX stats on the sender and RX stats
 * on the receiver.
 */
static netdev_tx_t vnic_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct vnic_priv *priv = netdev_priv(dev);
    struct vnic_pcpu_stats *pcpu;
    struct net_device *peer;
    unsigned int len = skb->len;

    rcu_read_lock();
    peer = rcu_dereference(priv->peer);

    if (unlikely(!peer || !netif_running(peer))) {
        pr_info(DRIVER_NAME ": %s — peer not available\n", dev->name);
        dev_kfree_skb(skb);
        pcpu = this_cpu_ptr(priv->pcpu_stats);
        u64_stats_update_begin(&pcpu->syncp);
        pcpu->tx_drops++;
        u64_stats_update_end(&pcpu->syncp);
        rcu_read_unlock();
        return NETDEV_TX_OK;
    }

    pr_info(DRIVER_NAME ": %s → %s, len=%d\n", dev->name, peer->name, len);

    if (dev_forward_skb(peer, skb) == NET_RX_SUCCESS) {
        pcpu = this_cpu_ptr(priv->pcpu_stats);
        u64_stats_update_begin(&pcpu->syncp);
        pcpu->tx_packets++;
        pcpu->tx_bytes += len;
        u64_stats_update_end(&pcpu->syncp);

        pcpu = this_cpu_ptr(((struct vnic_priv *)netdev_priv(peer))->pcpu_stats);
        u64_stats_update_begin(&pcpu->syncp);
        pcpu->rx_packets++;
        pcpu->rx_bytes += len;
        u64_stats_update_end(&pcpu->syncp);
    } else {
        pr_info(DRIVER_NAME ": dev_forward_skb failed\n");
        pcpu = this_cpu_ptr(priv->pcpu_stats);
        u64_stats_update_begin(&pcpu->syncp);
        pcpu->tx_drops++;
        u64_stats_update_end(&pcpu->syncp);
    }

    rcu_read_unlock();
    return NETDEV_TX_OK;
}

/*
 * vnic_get_stats64 - return interface statistics
 *
 * Aggregates per-CPU stats across all CPUs. Called when userspace
 * runs "ip -s link show vnic0".
 */
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

/*
 * vnic_setup - initialize net_device fields
 *
 * Called by alloc_netdev(). Sets Ethernet defaults and assigns our ops.
 */
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
        char name[IFNAMSIZ];

        snprintf(name, IFNAMSIZ, "vnic%d", i);

        vnic_devs[i] = alloc_netdev(sizeof(struct vnic_priv), name, NET_NAME_PREDICTABLE, vnic_setup);
        if (!vnic_devs[i]) {
            pr_err(DRIVER_NAME ": failed to allocate %s\n", name);
            err = -ENOMEM;
            goto err_free;
        }

        ((struct vnic_priv *)netdev_priv(vnic_devs[i]))->pcpu_stats =
            netdev_alloc_pcpu_stats(struct vnic_pcpu_stats);
        if (!((struct vnic_priv *)netdev_priv(vnic_devs[i]))->pcpu_stats) {
            pr_err(DRIVER_NAME ": failed to allocate pcpu_stats for %s\n", name);
            free_netdev(vnic_devs[i]);
            vnic_devs[i] = NULL;
            err = -ENOMEM;
            goto err_free;
        }

        err = register_netdev(vnic_devs[i]);
        if (err) {
            pr_err(DRIVER_NAME ": failed to register %s: %d\n", name, err);
            free_percpu(((struct vnic_priv *)netdev_priv(vnic_devs[i]))->pcpu_stats);
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

    /* Clear peer pointers before unregistering to prevent any in-flight
     * xmit from forwarding to a device that is being freed. */
    for (i = 0; i < VNIC_NUM_DEVS; i++) {
        if (vnic_devs[i])
            RCU_INIT_POINTER(((struct vnic_priv *)netdev_priv(vnic_devs[i]))->peer, NULL);
    }
    synchronize_rcu();

    for (i = 0; i < VNIC_NUM_DEVS; i++) {
        if (vnic_devs[i]) {
            free_percpu(((struct vnic_priv *)netdev_priv(vnic_devs[i]))->pcpu_stats);
            unregister_netdev(vnic_devs[i]);
            free_netdev(vnic_devs[i]);
            vnic_devs[i] = NULL;
        }
    }

    pr_info(DRIVER_NAME ": unloaded\n");
}

module_init(vnic_init);
module_exit(vnic_exit);
