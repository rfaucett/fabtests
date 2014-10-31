/*
 * Copyright (c) 2014, Cisco Systems, Inc. All rights reserved.
 *
 * [Insert appropriate license here when releasing outside of Cisco]
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>

struct fid_fabric *fab;
struct fid_mr *param_mr;
struct fid_mr *recv_mr;
struct fid_mr *send_mr;
struct usd_recv_desc *rx_descs;
struct fid_domain *dom;
struct fid_ep *ep;
struct fid_av *av;
struct fid_cq *rcq;
struct fid_cq *scq;
char *sendbuf;
int pkt_size;
int port;
int sock;
struct in_addr my_addr;
int rxbufs;
int wq_entries;
int aligned_pkt_size;
size_t recv_prefix;

void *parambuf;
int paramlen;
struct params {
    uint32_t payload_size;
    uint32_t warmup;
    uint32_t iters;
};
struct params *paramp;

static int64_t
get_elapsed(
    struct timespec *b,
    struct timespec *a)
{
    int64_t elapsed;

    elapsed = (a->tv_sec - b->tv_sec) * 1000 * 1000 * 1000;
    elapsed += a->tv_nsec - b->tv_nsec;
    return elapsed;
}

void alloc_bufs(int payload_size)
{
    char *regbuf;
	char *buf;
    int i;
    int ret;

    pkt_size = payload_size + recv_prefix;
    aligned_pkt_size = (pkt_size + 63) & ~63;

    printf("payload_size=%d, pkt_size=%d\n", payload_size, pkt_size);

    regbuf = malloc(aligned_pkt_size * rxbufs);
    if (regbuf == NULL) {
        perror("allocating regbuf");
        exit(1);
    }

    ret = fi_mr_reg(dom, regbuf, aligned_pkt_size * rxbufs,
		   0, 0, 0, 0, &recv_mr, NULL);
    if (ret != 0) {
		errno = -ret;
        perror("registering regbuf");
        exit(1);
    }
    for (i = 0; i < rxbufs; ++i) {
        buf = regbuf + (i * aligned_pkt_size);
        ret = fi_recv(ep, buf, aligned_pkt_size, NULL, buf);
        if (ret != 0) {
			errno = -ret;
            perror("fi_recv");
            exit(1);
        }
    }
    printf("posted %d RX buffers, size=%d (%d)\n", rxbufs,
           aligned_pkt_size, payload_size);

    sendbuf = malloc(pkt_size);
    if (sendbuf == NULL) {
        perror("alloc sendbuf");
        exit(1);
    }
    ret = fi_mr_reg(dom, sendbuf, pkt_size, 0, 0, 0, 0, &send_mr, NULL);
    if (ret != 0) {
        errno = -ret;
        perror("register sendbuf");
        exit(1);
    }
}

static inline void *
recv_wait(struct fid_cq *cq)
{
	struct fi_cq_entry comp;
    int rc;

    do {
        rc = fi_cq_read(rcq, &comp, sizeof(comp));
    } while (rc == 0);

    return comp.op_context;
}

void
client(uint32_t remote_ip_be, int iters, int payload_size)
{
    struct timespec before, after;
    int64_t elapsed;
	struct sockaddr_in sin;
    int ret;
	char *buf;
    int warmup;
    int i;

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = remote_ip_be;
	sin.sin_port = htons(port);
    ret = fi_connect(ep, &sin, NULL, 0);
    if (ret != 0) {
		errno = -ret;
		perror("fi_connect");
        exit(1);
    }

    printf("sending...\n");

    /* server will echo params to us when its time to go */
	ret = fi_recv(ep, parambuf, paramlen, NULL, parambuf);
    if (ret != 0) {
        perror("fi_recv params");
        exit(1);
    }

    warmup = wq_entries * 2;
    paramp->iters = htonl(iters);
    paramp->warmup = htonl(warmup);
    paramp->payload_size = htonl(payload_size);
	fi_send(ep, paramp, sizeof(struct params), NULL, NULL);

    /* wait for ACK of params */
    buf = recv_wait(rcq);

    alloc_bufs(payload_size);

    /* prime things - do one pingpong exchange first, so we can
     * unconditionally post a new recv right after each send, allows
     * us to always post recv in shadow of posting a send
     */
	fi_send(ep, sendbuf, payload_size, NULL, NULL);

    /* wait for incoming packet */
    buf = recv_wait(rcq);

    /* do the rest of the warmup, cycle thru TXQ twice */
    for (i = 0; i < warmup - 1; ++i) {
		fi_send(ep, sendbuf, payload_size, NULL, NULL);

		fi_recv(ep, buf, aligned_pkt_size, NULL, buf);

        buf = recv_wait(rcq);
    }

    /* warmup leaves us with 1 to post */

    /* And the real loop */
    clock_gettime(CLOCK_MONOTONIC_RAW, &before);
    for (i = 0; i < iters; ++i) {
		fi_send(ep, sendbuf, payload_size, NULL, NULL);

		fi_recv(ep, buf, aligned_pkt_size, NULL, buf);

        buf = recv_wait(rcq);
    }

    clock_gettime(CLOCK_MONOTONIC_RAW, &after);
    elapsed = get_elapsed(&before, &after);
    printf("%d pkts, %.3f us / HRT\n", iters,
           (double) elapsed / iters / 2 / 1000.);
}

struct udp_hdr {
    struct ether_header eth;
    struct iphdr ip;
    struct udphdr udp;
}__attribute__ ((__packed__));

void server()
{
    int ret;
    char *buf;
    struct sockaddr_in sin;
    size_t addrlen;
    fi_addr_t faddr;
    int payload_size;
	struct fi_cq_entry comp;
    int iters;
    int warmup;
    int i;

    /* wait for first incoming packet with params */
	ret = fi_recv(ep, parambuf, paramlen, NULL, parambuf);
    if (ret != 0) {
		errno = -ret;
        perror("fi_recv params");
        exit(1);
    }

    printf("Waiting for setup...\n");
    do {
        ret = fi_cq_readfrom(rcq, &comp, sizeof(comp), &faddr);
    } while (ret == 0);

    if (faddr == FI_ADDR_UNSPEC) {
        printf("Error getting address\n");
        exit(1);
    }

    addrlen = sizeof(sin);
    ret = fi_av_lookup(av, faddr, &sin, &addrlen);
    if (ret != 0) {
        printf("fi_av_lookup %d (%s)\n", ret, fi_strerror(-ret));
        exit(1);
    }

    iters = ntohl(paramp->iters);
    warmup = ntohl(paramp->warmup);
    payload_size = ntohl(paramp->payload_size);

    alloc_bufs(payload_size);

    printf("got setup packet, port=%d, IP=%s, iters=%d, payload=%d\n",
           ntohs(sin.sin_port), inet_ntoa(sin.sin_addr), iters, payload_size);

    ret = fi_connect(ep, &sin, NULL, 0);
    if (ret != 0) {
		errno = -ret;
        perror("fi_connect");
        exit(1);
    }

    /* echo params back to sender to say we are ready */
	fi_send(ep, paramp, sizeof(struct params), NULL, NULL);

    printf("pingponging...\n");

    iters += warmup;
    for (i = 0; i < iters; ++i) {
        buf = recv_wait(rcq);

		fi_send(ep, sendbuf, payload_size, NULL, NULL);

		fi_recv(ep, buf, aligned_pkt_size, NULL, buf);
    }

}

int main(int argc, char **argv)
{
    int ret;
    char *remote = NULL;
    int c;
    struct in_addr remote_addr;
    int iters;
    int payload_size;
    int rq_entries;
    int cq_entries;
	struct fi_info *fi;
	struct fi_cq_attr cq_attr;
	struct fi_info hints;
	struct fi_fabric_attr fabric_attr;
	struct fi_av_attr av_attr;
	char *fabric_name;

    payload_size = 4;
    iters = 100000;
    rq_entries = 64;
    wq_entries = 1024;
    cq_entries = 2048;
    port = 3333;
	fabric_name = NULL;

    while ((c = getopt(argc, argv, "r:w:i:h:p:s:f:")) != EOF)
        switch (c) {
        case 'r':
            rq_entries = atoi(optarg);
            break;
        case 'w':
            wq_entries = atoi(optarg);
            break;
        case 'i':
            iters = atoi(optarg);
            break;
        case 'h':
            remote = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            break;
        case 's':
            payload_size = atoi(optarg);
            break;
		case 'f':
			fabric_name = optarg;
			break;
        }
    if (remote != NULL) {
        inet_aton(remote, &remote_addr);
    }

	memset(&hints, 0, sizeof(hints));
	memset(&fabric_attr, 0, sizeof(fabric_attr));
	hints.ep_type = FI_EP_DGRAM;
	hints.caps = FI_MSG | FI_SEND | FI_RECV | FI_SOURCE;
	hints.mode = FI_LOCAL_MR | FI_MSG_PREFIX;
	hints.fabric_attr = &fabric_attr;
	hints.fabric_attr->name = fabric_name;

    ret = fi_getinfo(FI_VERSION(1, 0), NULL, NULL, 0, &hints, &fi);
    if (ret != 0) {
        errno = -ret;
        perror("fi_getinfo");
        exit(1);
    }
    printf("open %s OK\n", fi->fabric_attr->name);

	ret = fi_fabric(fi->fabric_attr, &fab, NULL);
    if (ret != 0) {
        errno = -ret;
        perror("fi_fabric");
        exit(1);
    }

	ret = fi_domain(fab, fi, &dom, NULL);
    if (ret != 0) {
        errno = -ret;
        perror("fi_domain");
        exit(1);
    }

    /* create a CQ and EP */
    memset(&cq_attr, 0, sizeof cq_attr);
    cq_attr.format = FI_CQ_FORMAT_CONTEXT;
    cq_attr.wait_obj = FI_WAIT_NONE;
    cq_attr.size = cq_entries;
	ret = fi_cq_open(dom, &cq_attr, &scq, NULL);
	ret |= fi_cq_open(dom, &cq_attr, &rcq, NULL);
    if (ret != 0) {
        errno = -ret;
        perror("fi_cq_open");
        exit(1);
    }
    rxbufs = rq_entries - 1;

	if (fi->src_addr != NULL) {

		((struct sockaddr_in *)fi->src_addr)->sin_port = ntohs(port);
		if (remote == NULL) {
			printf("Local address %s:%d\n",
					inet_ntoa(((struct sockaddr_in *)fi->src_addr)->sin_addr),
					ntohs(((struct sockaddr_in *)fi->src_addr)->sin_port));
		}
	}

	fi->rx_attr->size = rq_entries - 1;
	fi->tx_attr->size = wq_entries - 1;
	ret = fi_endpoint(dom, fi, &ep, NULL);
    if (ret != 0) {
        errno = -ret;
        perror("fi_endpoint");
        exit(1);
    }
	if ((fi->mode & FI_MSG_PREFIX) != 0) {
		recv_prefix = fi->ep_attr->msg_prefix_size;
	} else {
		recv_prefix = 0;
	}

    printf("EP create OK, prefix = %lu\n", recv_prefix);

	memset(&av_attr, 0, sizeof(av_attr));
    av_attr.type = FI_AV_MAP;
    av_attr.name = NULL;
    av_attr.flags = 0;
    ret = fi_av_open(dom, &av_attr, &av, NULL);
    if (ret != 0) {
        printf("fi_av_open %s\n", fi_strerror(-ret));
        exit(1);
    }

    ret = fi_bind(&ep->fid, &av->fid, 0);
    if (ret != 0) {
        printf("fi_bind av %d (%s)\n", ret, fi_strerror(-ret));
        exit(1);
    }

	fi_bind(&ep->fid, &scq->fid, FI_SEND);
	fi_bind(&ep->fid, &rcq->fid, FI_RECV);
	ret = fi_enable(ep);
    if (ret != 0) {
        errno = -ret;
        perror("fi_enable");
        exit(1);
    }

    paramlen = sizeof(struct params) + recv_prefix;
    paramlen = (paramlen + 63) & ~63;
	parambuf = calloc(1, paramlen);
    ret = fi_mr_reg(dom, parambuf, paramlen, 0, 0, 0, 0, &param_mr, NULL);
    if (ret != 0) {
        errno = -ret;
        perror("alloc parambuf");
        exit(1);
    }
    paramp = (struct params *) ((char *) parambuf + recv_prefix);

    /*
     * Run the pingpong
     */
    if (remote != NULL) {
        client(remote_addr.s_addr, iters, payload_size);
    } else {
        server();
    }

#if 0
    /*
     * Clean up everything in an orderly fashion
     */
    ret = usd_destroy_qp(qp);
    if (ret != 0) {
        errno = -ret;
        perror("usd_destroy_qp()");
    }

    ret = usd_destroy_cq(cq);
    if (ret != 0) {
        errno = -ret;
        perror("usd_destroy_cq()");
    }

    ret = usd_free_mr(parambuf);
    if (ret != 0) {
        errno = -ret;
        perror("usd_free_mr(parambuf)");
    }

    ret = usd_dereg_mr(send_mr);
    if (ret != 0) {
        errno = -ret;
        perror("usd_dereg_mr(send_mr)");
    }

    ret = usd_dereg_mr(recv_mr);
    if (ret != 0) {
        errno = -ret;
        perror("usd_dereg_mr(recv_mr)");
    }
#endif

    exit(0);
}
