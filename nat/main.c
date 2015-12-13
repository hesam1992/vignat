#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>
#include <assert.h>

#ifdef KLEE_VERIFICATION
#  include "rte_stubs.h"
#  include <klee/klee.h>
#else //KLEE_VERIFICATION
#  include <sys/queue.h>
#  include <rte_common.h>
#  include <rte_vect.h>
#  include <rte_byteorder.h>
#  include <rte_log.h>
#  include <rte_memory.h>
#  include <rte_memcpy.h>
#  include <rte_memzone.h>
#  include <rte_eal.h>
#  include <rte_per_lcore.h>
#  include <rte_launch.h>
#  include <rte_atomic.h>
#  include <rte_cycles.h>
#  include <rte_prefetch.h>
#  include <rte_lcore.h>
#  include <rte_per_lcore.h>
#  include <rte_branch_prediction.h>
#  include <rte_interrupts.h>
#  include <rte_pci.h>
#  include <rte_random.h>
#  include <rte_debug.h>
#  include <rte_ether.h>
#  include <rte_ethdev.h>
#  include <rte_ring.h>
#  include <rte_mempool.h>
#  include <rte_mbuf.h>
#  include <rte_ip.h>
#  include <rte_tcp.h>
#  include <rte_udp.h>
#  include <rte_string_fns.h>

#  include <cmdline_parse.h>
#  include <cmdline_parse_etheraddr.h>
#endif //KLEE_VERIFICATION

#include "flowtable.h"

#define RTE_LOGTYPE_NAT RTE_LOGTYPE_USER1

#ifdef KLEE_VERIFICATION
#  define LOG(...) 
#  define LOG_ADD(...)
#else //KLEE_VERIFICATION
#  define LOG(...) RTE_LOG(INFO, NAT, __VA_ARGS__)
#  define LOG_ADD(...) printf(__VA_ARGS__)
#endif //KLEE_VERIFICATION

#define MAX_PKT_BURST     32
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

/*
 * Try to avoid TX buffering if we have at least MAX_TX_BURST packets to send.
 */
#define	MAX_TX_BURST	(MAX_PKT_BURST / 2)

#define NB_SOCKETS 8

/* ethernet addresses of ports */
static uint64_t dest_eth_addr[RTE_MAX_ETHPORTS];
static struct ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];

static __m128i val_eth[RTE_MAX_ETHPORTS];

/* replace first 12B of the ethernet header. */
#define    MASK_ETH    0x3f

/* The WAN port */
static uint32_t wan_port_id = 0;

/* mask of enabled ports */
static uint32_t enabled_port_mask = 0;
static int promiscuous_on = 0; /**< Ports set in promiscuous mode off by default. */
static int numa_on = 1; /**< NUMA is enabled by default. */

struct mbuf_table {
    uint16_t len;
    struct rte_mbuf *m_table[MAX_PKT_BURST];
};

struct lcore_rx_queue {
    uint8_t port_id;
    uint8_t queue_id;
} __rte_cache_aligned;

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT RTE_MAX_ETHPORTS
#define MAX_RX_QUEUE_PER_PORT 128

#define MEMPOOL_CACHE_SIZE 256

/*
 * This expression is used to calculate the number of mbufs needed depending on user input, taking
 *  into account memory for rx and tx hardware rings, cache per lcore and mtable per port per lcore.
 *  RTE_MAX is used to ensure that NB_MBUF never goes below a minimum value of 8192
 */

#define NB_MBUF RTE_MAX	(                                               \
                         (nb_ports*nb_rx_queues*RTE_TEST_RX_DESC_DEFAULT + \
                          nb_ports*nb_lcores*MAX_PKT_BURST +            \
                          nb_ports*n_tx_queue*RTE_TEST_TX_DESC_DEFAULT + \
                          nb_lcores*MEMPOOL_CACHE_SIZE),                \
                         (unsigned)8192)

/* Configure how many packets ahead to prefetch, when reading packets */
#define PREFETCH_OFFSET	3

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;


#define MAX_LCORE_PARAMS 1024
struct lcore_params {
    uint8_t port_id;
    uint8_t queue_id;
    uint8_t lcore_id;
} __rte_cache_aligned;

static struct lcore_params lcore_params_array_default[] = {
    {0, 0, 0},
    {1, 0, 0},
};

static struct lcore_params * lcore_params = lcore_params_array_default;
static uint16_t nb_lcore_params = sizeof(lcore_params_array_default) /
                sizeof(lcore_params_array_default[0]);

static struct rte_eth_conf port_conf = {
    .rxmode = {
        .mq_mode = ETH_MQ_RX_RSS,
        .max_rx_pkt_len = ETHER_MAX_LEN,
        .split_hdr_size = 0,
        .header_split   = 0, /**< Header Split disabled */
        .hw_ip_checksum = 1, /**< IP checksum offload enabled */
        .hw_vlan_filter = 0, /**< VLAN filtering disabled */
        .jumbo_frame    = 0, /**< Jumbo Frame Support disabled */
        .hw_strip_crc   = 0, /**< CRC stripped by hardware */
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = NULL,
            .rss_hf = ETH_RSS_IP,
        },
    },
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE,
    },
};

static struct rte_mempool * pktmbuf_pool[NB_SOCKETS];

struct lcore_conf {
    uint16_t n_rx_queue;
    struct lcore_rx_queue rx_queue_list[MAX_RX_QUEUE_PER_LCORE];
    uint16_t tx_queue_id[RTE_MAX_ETHPORTS];
    struct mbuf_table tx_mbufs[RTE_MAX_ETHPORTS];
} __rte_cache_aligned;

static struct lcore_conf lcore_conf[RTE_MAX_LCORE];

/* Send burst of packets on an output interface */
static inline int
send_burst(struct lcore_conf *qconf, uint16_t n, uint8_t port)
{
    struct rte_mbuf **m_table;
    int ret;
    uint16_t queueid;

    queueid = qconf->tx_queue_id[port];
    m_table = (struct rte_mbuf **)qconf->tx_mbufs[port].m_table;

    ret = rte_eth_tx_burst(port, queueid, m_table, n);
    if (unlikely(ret < n)) {
        do {
            rte_pktmbuf_free(m_table[ret]);
        } while (++ret < n);
    }

    return 0;
}

/* Enqueue a single packet, and send burst if queue is filled */
static inline int
send_single_packet(struct rte_mbuf *m, uint8_t port)
{
    uint32_t lcore_id;
    uint16_t len;
    struct lcore_conf *qconf;

    lcore_id = rte_lcore_id();

    qconf = &lcore_conf[lcore_id];
    len = qconf->tx_mbufs[port].len;
    qconf->tx_mbufs[port].m_table[len] = m;
    len++;

    /* enough pkts to be sent */
    if (unlikely(len == MAX_PKT_BURST)) {
        send_burst(qconf, MAX_PKT_BURST, port);
        len = 0;
    }

    qconf->tx_mbufs[port].len = len;
    return 0;
}

static uint16_t get_src_port(struct rte_mbuf *m) {
    struct ipv4_hdr *ipv4_hdr = 
        rte_pktmbuf_mtod_offset(m, struct ipv4_hdr *,
                                sizeof(struct ether_hdr));
    if (ipv4_hdr->next_proto_id == IPPROTO_TCP ||
        ipv4_hdr->next_proto_id == IPPROTO_UDP) {
        return *(uint16_t*)(ipv4_hdr + 1);
    }
    return 0;
}

static uint16_t get_dst_port(struct rte_mbuf *m) {
    struct ipv4_hdr *ipv4_hdr = 
        rte_pktmbuf_mtod_offset(m, struct ipv4_hdr *,
                                sizeof(struct ether_hdr));
    if (ipv4_hdr->next_proto_id == IPPROTO_TCP ||
        ipv4_hdr->next_proto_id == IPPROTO_UDP) {
        return *((uint16_t*)(ipv4_hdr + 1) + 1/*skip srcport*/);
    }
    return 0;
}

static void set_src_port(struct rte_mbuf *m, uint16_t port) {
    struct ipv4_hdr *ipv4_hdr = 
        rte_pktmbuf_mtod_offset(m, struct ipv4_hdr *,
                                sizeof(struct ether_hdr));
    if (ipv4_hdr->next_proto_id == IPPROTO_TCP ||
        ipv4_hdr->next_proto_id == IPPROTO_UDP) {
        *(uint16_t*)(ipv4_hdr + 1) = port;
    }
}

static void set_dst_port(struct rte_mbuf *m, uint16_t port) {
    struct ipv4_hdr *ipv4_hdr = 
        rte_pktmbuf_mtod_offset(m, struct ipv4_hdr *,
                                sizeof(struct ether_hdr));
    if (ipv4_hdr->next_proto_id == IPPROTO_TCP ||
        ipv4_hdr->next_proto_id == IPPROTO_UDP) {
        *((uint16_t*)(ipv4_hdr + 1) + 1) = port;
    }
}

struct ext_iface {
    uint32_t ip;
    uint16_t port;
    uint8_t device_id;
};

uint16_t next_unused_external_port = 2747;

static struct ext_iface allocate_ext_iface(void) {
    return (struct ext_iface){
        .ip = IPv4(11,3,168,192),
        .port = next_unused_external_port++,
        .device_id = wan_port_id
    };
}

static inline __attribute__((always_inline)) void
simple_forward(struct rte_mbuf *m, uint8_t portid, struct lcore_conf *qconf)
{
    struct ether_hdr *eth_hdr;
    struct ipv4_hdr *ipv4_hdr;
    uint8_t dst_device;

    (void)qconf;

    eth_hdr = rte_pktmbuf_mtod(m, struct ether_hdr *);

    if (RTE_ETH_IS_IPV4_HDR(m->packet_type) ||
        (m->packet_type == 0 && eth_hdr->ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv4))) {
        //TODO: determine the packet type when packet_type is 0(undefined).
        /* Handle IPv4 headers.*/
        ipv4_hdr = rte_pktmbuf_mtod_offset(m, struct ipv4_hdr *,
                                           sizeof(struct ether_hdr));

        LOG( "forwarding and ipv4 packet on %d\n", portid);
        LOG( "eth_hdr size: %lu; ipv4 hdr size: %lu; data_offset %d:\n", sizeof(struct ether_hdr), sizeof(struct ipv4_hdr), m->data_off);
        if (portid == wan_port_id) {
            //External port.
            LOG( "port %d turns out to be external(%d)\n", portid, wan_port_id);
            struct ext_key key = {
                .ext_src_port = get_dst_port(m), //intentionally swapped.
                .dst_port = get_src_port(m),
                .ext_src_ip = ipv4_hdr->dst_addr, //Note, they are switched for
                .dst_ip = ipv4_hdr->src_addr, // the backwards traffic
                .ext_device_id = portid,
                .protocol = ipv4_hdr->next_proto_id
            };
            LOG( "for key: ");
            log_ext_key(&key);
            struct flow* f = get_flow_ext(&key);
            if (f == NULL) {
                // Use did not ask for this packet.
                LOG( "flow not found. dropping\n");
                rte_pktmbuf_free(m);
                return;
            } else {
                LOG( "found flow:");
                log_flow(f);
                ipv4_hdr->dst_addr = f->int_src_ip;
                set_dst_port(m, f->int_src_port);
                //TODO: recalculate ip checksum.
                dst_device = f->int_device_id;
            }
        } else {
            //Internal port.
            LOG( "port %d turns out NOT to be external(%d)\n", portid, wan_port_id);
            struct int_key key = {
                .int_src_port = get_src_port(m),
                .dst_port = get_dst_port(m),
                .int_src_ip = ipv4_hdr->src_addr,
                .dst_ip = ipv4_hdr->dst_addr,
                .int_device_id = portid,
                .protocol = ipv4_hdr->next_proto_id
            };
            LOG( "for key: ");
            log_int_key(&key);
            struct flow* f = get_flow_int(&key);
            if (f == NULL) {
                struct ext_iface alloc = allocate_ext_iface();
                struct flow new_flow = {
                    .int_src_port = get_src_port(m),
                    .ext_src_port = alloc.port,
                    .dst_port = get_dst_port(m),
                    .int_src_ip = ipv4_hdr->src_addr,
                    .ext_src_ip = alloc.ip,
                    .dst_ip = ipv4_hdr->dst_addr,
                    .int_device_id = portid,
                    .ext_device_id = alloc.device_id,
                    .protocol = ipv4_hdr->next_proto_id
                };
                f = &new_flow;
                LOG( "adding flow: ");
                add_flow(f);
            }
            LOG( "forwarding to: ");
            log_flow(f);
            ipv4_hdr->src_addr = f->ext_src_ip;
            set_src_port(m, f->ext_src_port);
            //TODO: recalculate ip checksum.
            dst_device = f->ext_device_id;
        }

//#ifdef DO_RFC_1812_CHECKS
//        /* Update time to live and header checksum */
//        --(ipv4_hdr->time_to_live);
//        ++(ipv4_hdr->hdr_checksum);
//#endif
        /* dst addr */
        //TODO: how do you get eth addr by ip addr. need to implement LPM?
        *(uint64_t *)&eth_hdr->d_addr = dest_eth_addr[dst_device];

        /* src addr */
        ether_addr_copy(&ports_eth_addr[dst_device], &eth_hdr->s_addr);

        ipv4_hdr->hdr_checksum = 0;
        if (ipv4_hdr->next_proto_id == IPPROTO_TCP) {
            struct tcp_hdr * tcp_hdr = (struct tcp_hdr*)(ipv4_hdr + 1);
            tcp_hdr->cksum = 0;
            tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, tcp_hdr);

        } else {
           struct udp_hdr * udp_hdr = (struct udp_hdr*)(ipv4_hdr + 1);
           udp_hdr->dgram_cksum = 0;
           udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);
        }
        ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);


        send_single_packet(m, dst_device);
    } else {
        LOG( "non ipv4 packet, discard");
        /* Free the mbuf that contains non-IPV4/IPV6 packet */
        rte_pktmbuf_free(m);
    }
}

/* main processing loop */
static int
main_loop(__attribute__((unused)) void *dummy)
{
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
    unsigned lcore_id;
    uint64_t prev_tsc, diff_tsc, cur_tsc;
    int i, j, nb_rx;
    uint8_t portid, queueid;
    struct lcore_conf *qconf;
    const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) /
        US_PER_S * BURST_TX_DRAIN_US;

    prev_tsc = 0;

    lcore_id = rte_lcore_id();
    qconf = &lcore_conf[lcore_id];

    if (qconf->n_rx_queue == 0) {
        LOG( "lcore %u has nothing to do\n", lcore_id);
        return 0;
    }

    LOG( "entering main loop on lcore %u\n", lcore_id);

    for (i = 0; i < qconf->n_rx_queue; i++) {

        portid = qconf->rx_queue_list[i].port_id;
        queueid = qconf->rx_queue_list[i].queue_id;
        LOG( " -- lcoreid=%u portid=%hhu rxqueueid=%hhu\n", lcore_id,
            portid, queueid);
    }

#ifdef KLEE_VERIFICATION
#else //KLEE_VERIFICATION
    while (1) 
#endif //KLEE_VERIFICATION
    {

        cur_tsc = rte_rdtsc();

        /*
         * TX burst queue drain
         */
        diff_tsc = cur_tsc - prev_tsc;
        if (unlikely(diff_tsc > drain_tsc)) {

            /*
             * This could be optimized (use queueid instead of
             * portid), but it is not called so often
             */
            for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
                if (qconf->tx_mbufs[portid].len == 0)
                    continue;
                send_burst(qconf,
                    qconf->tx_mbufs[portid].len,
                    portid);
                qconf->tx_mbufs[portid].len = 0;
            }

            prev_tsc = cur_tsc;
        }

        /*
         * Read packet from RX queues
         */
#ifdef KLEE_VERIFICATION
        klee_make_symbolic(&i, sizeof(int), "queue #: i");
        klee_assume(i < qconf->n_rx_queue);
        klee_assume(0 <= i);
#else //KLEE_VERIFICATION
        for (i = 0; i < qconf->n_rx_queue; ++i)
#endif //KLEE_VERIFICATION
        {
            portid = qconf->rx_queue_list[i].port_id;
            queueid = qconf->rx_queue_list[i].queue_id;
            nb_rx = rte_eth_rx_burst(portid, queueid, pkts_burst,
                MAX_PKT_BURST);
            //too verbose. LOG("received %d packets\n", nb_rx);
            if (nb_rx != 0) {

                /* Prefetch first packets */
                for (j = 0; j < PREFETCH_OFFSET && j < nb_rx; j++) {
                    rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[j], void *));
                }

                /* Prefetch and forward already prefetched packets */
                for (j = 0; j < (nb_rx - PREFETCH_OFFSET); j++) {
                    rte_prefetch0(rte_pktmbuf_mtod
                                  (pkts_burst[j + PREFETCH_OFFSET], void *));
                    simple_forward(pkts_burst[j], portid, qconf);
                }

                /* Forward remaining prefetched packets */
                for (; j < nb_rx; j++) {
                    simple_forward(pkts_burst[j], portid, qconf);
                }
            }
        }
    }
    return 0;
}

static int
check_lcore_params(void)
{
    uint8_t queue, lcore;
    uint16_t i;
    int socketid;

    for (i = 0; i < nb_lcore_params; ++i) {
        queue = lcore_params[i].queue_id;
        if (queue >= MAX_RX_QUEUE_PER_PORT) {
            printf("invalid queue number: %hhu\n", queue);
            return -1;
        }
        lcore = lcore_params[i].lcore_id;
        if (!rte_lcore_is_enabled(lcore)) {
            printf("error: lcore %hhu is not enabled in lcore mask\n", lcore);
            return -1;
        }
        if ((socketid = rte_lcore_to_socket_id(lcore) != 0) &&
            (numa_on == 0)) {
            printf("warning: lcore %hhu is on socket %d with numa off \n",
                lcore, socketid);
        }
    }
    return 0;
}

static int
check_port_config(const unsigned nb_ports)
{
    unsigned portid;
    uint16_t i;

    for (i = 0; i < nb_lcore_params; ++i) {
        portid = lcore_params[i].port_id;
        if ((enabled_port_mask & (1 << portid)) == 0) {
            printf("port %u is not enabled in port mask\n", portid);
            return -1;
        }
        if (portid >= nb_ports) {
            printf("port %u is not present on the board\n", portid);
            return -1;
        }
    }
    return 0;
}

static uint8_t
get_port_n_rx_queues(const uint8_t port)
{
    int queue = -1;
    uint16_t i;

    for (i = 0; i < nb_lcore_params; ++i) {
        if (lcore_params[i].port_id == port && lcore_params[i].queue_id > queue)
            queue = lcore_params[i].queue_id;
    }
    return (uint8_t)(++queue);
}

static int
init_lcore_rx_queues(void)
{
    uint16_t i, nb_rx_queue;
    uint8_t lcore;

    for (i = 0; i < nb_lcore_params; ++i) {
        lcore = lcore_params[i].lcore_id;
        nb_rx_queue = lcore_conf[lcore].n_rx_queue;
        if (nb_rx_queue >= MAX_RX_QUEUE_PER_LCORE) {
            printf("error: too many queues (%u) for lcore: %u\n",
                (unsigned)nb_rx_queue + 1, (unsigned)lcore);
            return -1;
        } else {
            lcore_conf[lcore].rx_queue_list[nb_rx_queue].port_id =
                lcore_params[i].port_id;
            lcore_conf[lcore].rx_queue_list[nb_rx_queue].queue_id =
                lcore_params[i].queue_id;
            lcore_conf[lcore].n_rx_queue++;
        }
    }
    return 0;
}

/* display usage */
static void
print_usage(const char *prgname)
{
    printf ("%s [EAL options] -- -wan port_id"
            "  -wan port_id: set the port port_id to be the external one.\n"
            "NAT does not change the source address of the packets, coming "
            "from the external port",
            prgname);
}

static uint32_t
parse_portmask(const char *portmask)
{
    char *end = NULL;
    unsigned long pm;
    
    /* parse hexadecimal string */
    pm = strtoul(portmask, &end, 16);
    if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
        return 0;
    
    return pm;
}

#define CMD_LINE_OPT_WAN_PORT "wan"

/* Parse the argument given in the command line of the application */
static int
parse_args(int argc, char **argv, unsigned nb_ports)
{
    int opt, ret;
    char **argvopt;
    int option_index;
    char *prgname = argv[0];
    char *port_end;
    static struct option lgopts[] = {
        {CMD_LINE_OPT_WAN_PORT, 1, 0, 0},
        {NULL, 0, 0, 0}
    };

    argvopt = argv;

    while ((opt = getopt_long(argc, argvopt, "p:P",
                           lgopts, &option_index)) != EOF) {
        if (opt == 'p') {
            enabled_port_mask = parse_portmask(optarg);
            if (enabled_port_mask == 0) {
                printf("invalid portmask\n");
                print_usage(prgname);
                return -1;
            }
        }
        else if (0 == strncmp(lgopts[option_index].name, CMD_LINE_OPT_WAN_PORT,
                              sizeof (CMD_LINE_OPT_WAN_PORT))) {
            LOG("parsing wan port: %s\n", optarg);
            wan_port_id = strtoul(optarg, &port_end, 10);
            if ((optarg[0] == '\0') || (port_end == NULL) || (*port_end != '\0'))
                return -1;
            if (wan_port_id >= nb_ports) {
                printf("WAN port does not exist.");
                return -1;
            }
            if ((enabled_port_mask & 1 << wan_port_id) == 0) {
                printf("WAN port is not enabled");
                return -1;
            }
        } else {
            print_usage(prgname);
            return -1;
        }
    }

    if (optind >= 0)
        argv[optind-1] = prgname;

    ret = optind-1;
    optind = 0; /* reset getopt lib */
    return ret;
}

static void
print_ethaddr(const char *name, const struct ether_addr *eth_addr)
{
    char buf[ETHER_ADDR_FMT_SIZE];
    ether_format_addr(buf, ETHER_ADDR_FMT_SIZE, eth_addr);
    printf("%s%s", name, buf);
}

static int
init_mem(unsigned nb_mbuf, uint8_t nb_ports)
{
    int socketid;
    unsigned lcore_id;
    char s[64];

    for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
        if (rte_lcore_is_enabled(lcore_id) == 0)
            continue;

        if (numa_on)
            socketid = rte_lcore_to_socket_id(lcore_id);
        else
            socketid = 0;

        if (socketid >= NB_SOCKETS) {
            rte_exit(EXIT_FAILURE, "Socket %d of lcore %u is out of range %d\n",
                socketid, lcore_id, NB_SOCKETS);
        }
        LOG("sock check completed\n");
        if (pktmbuf_pool[socketid] == NULL) {
            snprintf(s, sizeof(s), "mbuf_pool_%d", socketid);
            pktmbuf_pool[socketid] =
                rte_pktmbuf_pool_create(s, nb_mbuf,
                    MEMPOOL_CACHE_SIZE, 0,
                    RTE_MBUF_DEFAULT_BUF_SIZE, socketid);
            if (pktmbuf_pool[socketid] == NULL)
                rte_exit(EXIT_FAILURE,
                        "Cannot init mbuf pool on socket %d\n", socketid);
            else
                printf("Allocated mbuf pool on socket %d\n", socketid);
        }
    }

    allocate_flowtables(nb_ports);
    LOG("memory initialized successfully\n");
    return 0;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
    uint8_t portid, count, all_ports_up, print_flag = 0;
    struct rte_eth_link link;

    printf("\nChecking link status");
    fflush(stdout);
    for (count = 0; count <= MAX_CHECK_TIME; count++) {
        all_ports_up = 1;
        for (portid = 0; portid < port_num; portid++) {
            if ((port_mask & (1 << portid)) == 0)
                continue;
            memset(&link, 0, sizeof(link));
            rte_eth_link_get_nowait(portid, &link);
            /* print link status if flag set */
            if (print_flag == 1) {
                if (link.link_status)
                    printf("Port %d Link Up - speed %u "
                        "Mbps - %s\n", (uint8_t)portid,
                        (unsigned)link.link_speed,
                (link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
                    ("full-duplex") : ("half-duplex\n"));
                else
                    printf("Port %d Link Down\n",
                        (uint8_t)portid);
                continue;
            }
            /* clear all_ports_up flag if any link down */
            if (link.link_status == 0) {
                all_ports_up = 0;
                break;
            }
        }
        /* after finally printing all link status, get out */
        if (print_flag == 1)
            break;

        if (all_ports_up == 0) {
            printf(".");
            fflush(stdout);
            rte_delay_ms(CHECK_INTERVAL);
        }

        /* set the print_flag if all ports up or timeout */
        if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
            print_flag = 1;
            printf("done\n");
        }
    }
}

int
main(int argc, char **argv)
{
    struct lcore_conf *qconf;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf *txconf;
    int ret;
    unsigned nb_ports;
    uint16_t queueid;
    unsigned lcore_id;
    uint32_t n_tx_queue, nb_lcores;
    uint8_t portid, nb_rx_queues, queue, socketid;

    /* init EAL */
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Invalid EAL parameters\n");
    argc -= ret;
    argv += ret;

    /* pre-init dst MACs for all ports to 02:00:00:00:00:xx */
    for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
        dest_eth_addr[portid] = 0xffffffffffff; //ETHER_LOCAL_ADMIN_ADDR + ((uint64_t)portid << 40);
        *(uint64_t *)(val_eth + portid) = dest_eth_addr[portid];
    }

    // TODO: moved w.r.t l3fwd. see if this still works.
    nb_ports = rte_eth_dev_count();
    if (nb_ports > RTE_MAX_ETHPORTS)
        nb_ports = RTE_MAX_ETHPORTS;

    /* parse application arguments (after the EAL ones) */
    ret = parse_args(argc, argv, nb_ports);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Invalid NAT parameters\n");

    if (check_lcore_params() < 0)
        rte_exit(EXIT_FAILURE, "check_lcore_params failed\n");

    ret = init_lcore_rx_queues();
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "init_lcore_rx_queues failed\n");

    if (check_port_config(nb_ports) < 0)
        rte_exit(EXIT_FAILURE, "check_port_config failed\n");

    nb_lcores = rte_lcore_count();
    n_tx_queue = nb_lcores;
    nb_rx_queues = 0;
    /* initialize all ports */
    for (portid = 0; portid < nb_ports; portid++) {
        /* skip ports that are not enabled */
        if ((enabled_port_mask & (1 << portid)) == 0) {
            printf("\nSkipping disabled port %d\n", portid);
            continue;
        }

        /* init port */
        printf("Initializing port %d ... ", portid );
        fflush(stdout);

        uint8_t nb_rx_queue = get_port_n_rx_queues(portid);
        //VVV ??? this is a questionable aggregation. in l3fwd it is even worse
        nb_rx_queues = RTE_MAX(nb_rx_queue, nb_rx_queues);
        if (n_tx_queue > MAX_TX_QUEUE_PER_PORT)
            n_tx_queue = MAX_TX_QUEUE_PER_PORT;
        printf("Creating queues: nb_rxq=%d nb_txq=%u... ",
            nb_rx_queue, (unsigned)n_tx_queue );
        ret = rte_eth_dev_configure(portid, nb_rx_queue,
                    (uint16_t)n_tx_queue, &port_conf);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%d\n",
                ret, portid);

        rte_eth_macaddr_get(portid, &ports_eth_addr[portid]);
        print_ethaddr(" Address:", &ports_eth_addr[portid]);
        printf(", ");
        print_ethaddr("Destination:",
            (const struct ether_addr *)&dest_eth_addr[portid]);
        printf(", ");

        /*
         * prepare src MACs for each port.
         */
        ether_addr_copy(&ports_eth_addr[portid],
            (struct ether_addr *)(val_eth + portid) + 1);

        /* init one TX queue per couple (lcore,port) */
        queueid = 0;
        for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
            if (rte_lcore_is_enabled(lcore_id) == 0)
                continue;

            if (numa_on)
                socketid = (uint8_t)rte_lcore_to_socket_id(lcore_id);
            else
                socketid = 0;

            printf("txq=%u,%d,%d ", lcore_id, queueid, socketid);
            fflush(stdout);

            rte_eth_dev_info_get(portid, &dev_info);
            txconf = &dev_info.default_txconf;
            if (port_conf.rxmode.jumbo_frame)
                txconf->txq_flags = 0;
            ret = rte_eth_tx_queue_setup(portid, queueid, nb_txd,
                             socketid, txconf);
            if (ret < 0)
                rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup: err=%d, "
                    "port=%d\n", ret, portid);

            qconf = &lcore_conf[lcore_id];
            qconf->tx_queue_id[portid] = queueid;
            queueid++;
        }
        printf("\n");
    }

    /* init memory */
    ret = init_mem(NB_MBUF, nb_ports);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "init_mem failed\n");
    
    for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
        if (rte_lcore_is_enabled(lcore_id) == 0)
            continue;
        qconf = &lcore_conf[lcore_id];
        printf("\nInitializing rx queues on lcore %u ... ", lcore_id );
        fflush(stdout);
        /* init RX queues */
        for(queue = 0; queue < qconf->n_rx_queue; ++queue) {
            portid = qconf->rx_queue_list[queue].port_id;
            queueid = qconf->rx_queue_list[queue].queue_id;

            if (numa_on)
                socketid = (uint8_t)rte_lcore_to_socket_id(lcore_id);
            else
                socketid = 0;

            printf("rxq=%d,%d,%d ", portid, queueid, socketid);
            fflush(stdout);

            ret = rte_eth_rx_queue_setup(portid, queueid, nb_rxd,
                    socketid,
                    NULL,
                    pktmbuf_pool[socketid]);
            if (ret < 0)
                rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup: err=%d,"
                        "port=%d\n", ret, portid);
        }
    }

    printf("\n");

    /* start ports */
    for (portid = 0; portid < nb_ports; portid++) {
        if ((enabled_port_mask & (1 << portid)) == 0) {
            continue;
        }
        /* Start device */
        ret = rte_eth_dev_start(portid);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "rte_eth_dev_start: err=%d, port=%d\n",
                ret, portid);

        /*
         * If enabled, put device in promiscuous mode.
         * This allows IO forwarding mode to forward packets
         * to itself through 2 cross-connected  ports of the
         * target machine.
         */
        if (promiscuous_on)
            rte_eth_promiscuous_enable(portid);
    }

    check_all_ports_link_status((uint8_t)nb_ports, enabled_port_mask);

    /* launch per-lcore init on every lcore */
    rte_eal_mp_remote_launch(main_loop, NULL, CALL_MASTER);
    RTE_LCORE_FOREACH_SLAVE(lcore_id) {
        if (rte_eal_wait_lcore(lcore_id) < 0)
            return -1;
    }

    return 0;
}