# Virtual NIC Pair Driver

A Linux kernel module that creates a pair of virtual network interfaces
(vnic0, vnic1) connected by a software wire. Packets sent on vnic0
arrive on vnic1 and vice versa, with no physical hardware involved.

Built to demonstrate NIC driver internals: TX descriptor rings, DMA
simulation, backpressure, per-CPU stats, and kernel thread management.

---

## Architecture

```
host1 namespace            host2 namespace
┌─────────────┐            ┌─────────────┐
│    vnic0    │            │    vnic1    │
│192.168.100.1│            │192.168.100.2│
└──────┬──────┘            └──────┬──────┘
       │                          │
       │   vnic_start_xmit        │
       │   enqueue → TX ring      │
       │        ↓                 │
       │   vnic0-tx kthread       │
       │   dev_forward_skb() ─────┘
       │   (skb pointer passed
       └──  directly in memory)
```

`vnic_start_xmit` writes the skb pointer into a 256-slot circular
descriptor ring and returns immediately, like a driver writing to
hardware registers and ringing a doorbell. A per-device kernel thread
(`vnic0-tx`, `vnic1-tx`) simulates the DMA engine: it drains the ring
asynchronously and forwards packets via `dev_forward_skb()`.

---

## What it demonstrates

| Concept | Where |
|---------|-------|
| TX descriptor ring: circular buffer with head/tail pointers | `struct vnic_tx_ring`, `vnic_start_xmit` |
| TX backpressure: stop queue when ring full, wake on drain | `netif_stop_queue`, `netif_wake_queue` in xmit + kthread |
| Simulated DMA engine: async packet processing | `vnic_tx_thread` kthread |
| RCU-protected peer pointer: zero overhead on TX hot path | `rcu_dereference`, `synchronize_rcu` |
| Per-CPU stats: no cache contention between cores | `vnic_pcpu_stats`, `u64_stats_sync` |
| ethtool integration | `get_drvinfo`, `get_link` |

---

## Environment

- Host: Windows 11
- VM: Multipass (Ubuntu 22.04, kernel 6.8.0-110-generic)
- Build tools: gcc-13, linux-headers-6.8.0-110-generic

---

## Build

```bash
cd /home/ubuntu/vnic-driver
make
```

Output:
```
CC [M]  /home/ubuntu/vnic-driver/vnic.o
LD [M]  /home/ubuntu/vnic-driver/vnic.ko
```

---

## Load

```bash
sudo insmod vnic.ko
sudo dmesg | tail -5
```

Output:
```
vnic: registered vnic0
vnic: registered vnic1
vnic: loaded v0.2
```

Verify interfaces exist:
```bash
ip link show | grep vnic
```

Output:
```
4: vnic0: <BROADCAST,MULTICAST> mtu 1500 qdisc noop state DOWN ...
5: vnic1: <BROADCAST,MULTICAST> mtu 1500 qdisc noop state DOWN ...
```

---

## Test

Set up network namespaces to simulate two separate machines:

```bash
sudo ip netns add host1
sudo ip netns add host2
sudo ip link set vnic0 netns host1
sudo ip link set vnic1 netns host2
sudo ip netns exec host1 ip link set vnic0 up
sudo ip netns exec host2 ip link set vnic1 up
sudo ip netns exec host1 ip addr add 192.168.100.1/24 dev vnic0
sudo ip netns exec host2 ip addr add 192.168.100.2/24 dev vnic1
```

### Kernel threads

When the interfaces come up, each device spawns its own TX thread:

```bash
ps aux | grep vnic
```

Output:
```
root      1620  0.0  0.0      0     0 ?        S    11:54   0:00 [vnic0-tx]
root      1626  0.0  0.0      0     0 ?        S    11:54   0:00 [vnic1-tx]
```

Each thread sleeps on a wait queue when its TX ring is empty and wakes
the moment `vnic_start_xmit` enqueues a packet.

### Ping

```bash
sudo ip netns exec host1 ping -c 4 192.168.100.2
```

Output:
```
PING 192.168.100.2 (192.168.100.2) 56(84) bytes of data.
64 bytes from 192.168.100.2: icmp_seq=1 ttl=64 time=0.087 ms
64 bytes from 192.168.100.2: icmp_seq=2 ttl=64 time=0.066 ms
64 bytes from 192.168.100.2: icmp_seq=3 ttl=64 time=0.064 ms
64 bytes from 192.168.100.2: icmp_seq=4 ttl=64 time=0.067 ms

--- 192.168.100.2 ping statistics ---
4 packets transmitted, 4 received, 0% packet loss, time 3092ms
rtt min/avg/max/mdev = 0.064/0.071/0.087/0.009 ms
```

~0.07ms latency, slightly higher than direct forwarding due to the queue +
kthread scheduling hop, but still no physical medium involved.

### TX ring activity in dmesg

```bash
sudo dmesg | grep vnic | grep -E "enqueued|→" | head -30
```

Output:
```
[   80.481967] vnic: vnic0 enqueued len=90 ring=1/256
[   80.740933] vnic: vnic0 enqueued len=86 ring=1/256
[   80.781238] vnic: vnic0 enqueued len=90 ring=1/256
[   81.772772] vnic: vnic0 enqueued len=90 ring=1/256
[   81.772788] vnic: vnic0 enqueued len=70 ring=2/256
[   81.844763] vnic: vnic0 enqueued len=90 ring=1/256
[   85.868371] vnic: vnic0 enqueued len=70 ring=1/256
[   89.337090] vnic: vnic1 enqueued len=90 ring=1/256
[   89.337097] vnic: vnic1 → vnic0, len=90
[   89.370077] vnic: vnic1 enqueued len=86 ring=1/256
[   89.370091] vnic: vnic1 → vnic0, len=86
[   89.684409] vnic: vnic1 enqueued len=90 ring=1/256
[   89.684436] vnic: vnic1 → vnic0, len=90
[   90.412065] vnic: vnic1 enqueued len=90 ring=1/256
[   90.412086] vnic: vnic1 enqueued len=70 ring=2/256
[   90.412098] vnic: vnic1 → vnic0, len=90
[   90.412114] vnic: vnic1 → vnic0, len=70
[   90.588041] vnic: vnic1 enqueued len=90 ring=1/256
[   90.588061] vnic: vnic1 → vnic0, len=90
[   94.187765] vnic: vnic1 enqueued len=70 ring=1/256
[   94.187825] vnic: vnic1 → vnic0, len=70
[   94.571734] vnic: vnic0 enqueued len=70 ring=1/256
[   94.571798] vnic: vnic0 → vnic1, len=70
[  102.251356] vnic: vnic1 enqueued len=70 ring=1/256
[  102.251424] vnic: vnic1 → vnic0, len=70
[  110.954975] vnic: vnic0 enqueued len=70 ring=1/256
[  110.955027] vnic: vnic0 → vnic1, len=70
[  118.122720] vnic: vnic1 enqueued len=70 ring=1/256
[  118.122763] vnic: vnic1 → vnic0, len=70
[  124.694320] vnic: vnic0 enqueued len=42 ring=1/256
```

Three things visible in this trace:

**No `→` for vnic0 at 80-85s**: vnic1 was not yet running when vnic0 sent
its initial IPv6 NDP packets. The kthread dequeued each skb, found
`!netif_running(peer)`, freed it, and counted a TX drop. These are the 7
TX drops visible in `ip -s link show vnic0`. The ring worked correctly;
it just had nowhere to forward.

**Burst absorption at `[90.412...]`**: two packets enqueued 21µs apart
(`ring=2/256`) before the kthread ran. The kthread's inner loop drained
both in the same pass, logging two `→` lines back-to-back. This is the ring
absorbing a burst without dropping.

**Async gap**: enqueue and `→` are always a pair but never the same
instruction. vnic1's gap is ~20µs (kthread woke fast); vnic0's gap at
`[94.571...]` is 64µs. The ring sits between them, decoupling the TX path
from the forwarding path.

### Interface stats

```bash
sudo ip netns exec host1 ip -s link show vnic0
```

Output:
```
4: vnic0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 ...
    RX:  bytes packets errors dropped  missed   mcast
          1202      15      0       0       0       0
    TX:  bytes packets errors dropped carrier collsns
           616       8      0       7       0       0
```

### ethtool

```bash
sudo ip netns exec host1 ethtool vnic0
```

Output:
```
Settings for vnic0:
        Link detected: yes
```

```bash
sudo ip netns exec host1 ethtool -i vnic0
```

Output:
```
driver: vnic
version: 0.2
firmware-version:
bus-info:
supports-statistics: no
```

---

## Unload

```bash
sudo rmmod vnic
sudo dmesg | tail -3
```

Output:
```
vnic: vnic0 down
vnic: vnic1 down
vnic: unloaded
```

On unload, `vnic_stop` signals each kthread to exit via `kthread_stop`,
drains any remaining skbs from the ring, then `vnic_exit` clears peer
pointers under RCU before freeing devices.

---

## Key implementation details

- **TX descriptor ring**: 256-slot circular buffer in `vnic_priv`. `vnic_start_xmit`
  writes the skb pointer at `ring[head & 255]` and returns. The kthread reads
  from `ring[tail & 255]` and forwards. Power-of-2 size makes indexing a
  single bitwise AND.

- **Backpressure**: when `head - tail >= 256` (ring full), `netif_stop_queue`
  blocks the kernel from calling xmit. When the kthread drains below 64 entries,
  `netif_wake_queue` resumes it. The gap (256 to 64) prevents rapid stop/start
  oscillation under sustained load.

- **Spinlock pairing**: xmit runs in softirq context (BH already disabled),
  so it uses `spin_lock`. The kthread runs in process context and uses
  `spin_lock_bh` to disable softirqs while holding the lock, preventing
  the xmit softirq from preempting it and deadlocking on the same lock.

- **`netif_wake_queue` outside the lock**: waking the queue can immediately
  trigger `vnic_start_xmit` on another CPU, which tries to acquire the same
  spinlock. The kthread releases the lock before calling `wake_queue` to
  avoid this deadlock.

- **`get_cpu_ptr` / `put_cpu_ptr` in kthread**: the kthread can be preempted
  and migrated between CPUs. `get_cpu_ptr` disables preemption to pin the
  thread to its current CPU for the duration of the stat update.

- **RCU peer pointer**: zero overhead on the TX hot path, safe teardown
  via `synchronize_rcu` on module exit.

- **Per-CPU stats**: `u64_stats_sync` per CPU core. No cache line bouncing
  between cores; aggregated in `ndo_get_stats64`.
