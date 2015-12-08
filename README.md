#1 What is MQ-ECN
MQ-ECN is a new ECN marking scheme to enable ECN for multi-service multi-queue production data center networks. MQ-ECN can achieve high throughput, low latency and weighted fair sharing simultaneously. 

For more details about MQ-ECN, please refer to our NSDI 2016 paper <a href="http://sing.cse.ust.hk/~wei/papers/mqecn-draft.pdf">draft</a>.

#2 How to use
##2.1 Compiling
MQ-ECN software prototype is implemented as a Linux queuing discpline (qdisc) kernel module. So you need the kernel headers to compile it. You can find available headers on your system in `/lib/modules`. To install the kernel headers, you just need to use the following command：
<pre><code>$ apt-get install linux-headers-$(uname -r)
</code></pre>

Then you can compile MQ-ECN kernel module:
<pre><code>$ cd sch_dwrr
$ make
</code></pre>

This will produce a kernel module called `sch_dwrr.ko`. I have tested it with Linux kernel 3.18.11. MQ-ECN kernel module is built on the top of <a href="http://lxr.free-electrons.com/source/net/sched/sch_tbf.c">Token Bucket Filter (tbf)</a> and <a href="http://lxr.free-electrons.com/source/net/sched/sch_drr.c">Deficit Round Robin (drr) scheduler</a> in Linux kernel. 

##2.2 Installing
MQ-ECN replaces token bucket rate limiter module so you can use your existing `tc` tool to install MQ-ECN. So you need to remove `tbf` before you can use MQ-ECN. To install MQ-ECN on a device:

<pre><code>$ rmmod sch_tbf
$ insmod sch_dwrr.ko
$ tc qdisc add dev eth1 root tbf rate 995mbit limit 1000k burst 1000k mtu 66000 peakrate 1000mbit
</code></pre>

In above example, we install MQ-ECN on eth1. The shaping rate is 995Mbps (line rate is 1000Mbps). To accurately reflect switch buffer occupancy, we usually trade a little bandwidth. 

##2.3 Configuring
Except for shaping rate, all the parameters of MQ-ECN are configured through `sysctl` interfaces. Here, I only show several important parameters. For the reset, see `params.h` and `params.c` for more details.
<ul>
<li>ECN marking scheme:
<pre><code>$ sysctl dwrr.ecn_scheme
</code></pre>
To enable MQ-ECN:
<pre><code>$ sysctl -w dwrr.ecn_scheme=4
</code></pre>
To enable per-queue ECN marking:
<pre><code>$ sysctl -w dwrr.ecn_scheme=1
</code></pre>
</li>
<li>Per-port ECN marking threshold (bytes):
<pre><code>$ sysctl dwrr.port_thresh_bytes
</code></pre>
To set per-port ECN marking threshold to 32KB:
<pre><code>$ sysctl -w dwrr.port_thresh_bytes=32000
</code></pre>
</li>
</ul>
