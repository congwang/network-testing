/* -*- c-file-style: "linux" -*- */

/*
 * UDP echo program that handles the UDP multihomed problem.
 *  - For both IPv4 and IPv6
 *
 * Author: Jesper Dangaard Brouer <brouer@redhat.com>
 *
 * Based upon UDP example, by Eric Dumazet
 *  http://article.gmane.org/gmane.linux.network/239543
 *  http://thread.gmane.org/gmane.linux.network/239487/focus=239543
 *
 * This example show howto avoid the UDP multihomed IP problem.
 * - Where the kernel chooses the "wrong" IP source when "replying"
 * - recvfrom() does not give info on the local dest IP of the UDP packet
 * - sendto() does not specify which source IP to use (it upto the kernel)
 * - When a host have several IPs, this can cause issues.
 * - This can be solved by using recvmsg()/sendmsg()
 * - And setting socket opt IP_PKTINFO to request this as ancillary info
 * - For IPv6 the socket opt is IPV6_RECVPKTINFO.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/udp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 4040 /* Default port, change with option "-l" */
#define DEBUG 1
static volatile int verbose = 1;

void error(char *msg)
{
	perror(msg);
	exit(1);
}

#ifndef __USE_GNU
/* IPv6 packet information - in cmsg_data[] */
struct in6_pktinfo
{
	struct in6_addr ipi6_addr;	/* src/dst IPv6 address */
	unsigned int ipi6_ifindex;	/* send/recv interface index */
};
#endif

int pktinfo_get(struct msghdr *my_hdr, struct in_pktinfo *pktinfo, struct in6_pktinfo *pktinfo6)
{
	int res = -1;

	if (my_hdr->msg_controllen > 0) {
		struct cmsghdr *get_cmsg;
		for (get_cmsg = CMSG_FIRSTHDR(my_hdr); get_cmsg;
		     get_cmsg = CMSG_NXTHDR(my_hdr, get_cmsg)) {
			if (get_cmsg->cmsg_level == IPPROTO_IP &&
			    get_cmsg->cmsg_type  == IP_PKTINFO) {
				struct in_pktinfo *get_pktinfo = (struct in_pktinfo *)CMSG_DATA(get_cmsg);
				memcpy(pktinfo, get_pktinfo, sizeof(*pktinfo));
				res = AF_INET;
			} else if (get_cmsg->cmsg_level == IPPROTO_IPV6 &&
				   get_cmsg->cmsg_type  == IPV6_PKTINFO
				) {
				struct in6_pktinfo *get_pktinfo = (struct in6_pktinfo *)CMSG_DATA(get_cmsg);
				memcpy(pktinfo6, get_pktinfo, sizeof(*pktinfo6));
				res = AF_INET6;
			} else if (DEBUG) {
				fprintf(stderr, "Unknown ancillary data, len=%d, level=%d, type=%d\n",
					get_cmsg->cmsg_len, get_cmsg->cmsg_level, get_cmsg->cmsg_type);
			}
		}
	}
	return res;
}

int print_info(struct msghdr *my_hdr)
{
	struct in_pktinfo pktinfo;
	struct in6_pktinfo pktinfo6;
	char to_addr[INET6_ADDRSTRLEN]; /* see man inet_ntop(3) */
	char from_addr[INET6_ADDRSTRLEN];
	uint16_t rem_port;

	int addr_family = pktinfo_get(my_hdr, &pktinfo, &pktinfo6);

	/* msghdr->msg_name -- contains remote addr pointer */
	if (addr_family == AF_INET) {
		struct sockaddr_in *rem_addr = (struct sockaddr_in *) my_hdr->msg_name;
		rem_port = rem_addr->sin_port;
		if (!inet_ntop(addr_family, (void*)&rem_addr->sin_addr, from_addr, sizeof(from_addr)))
			perror("inet_ntop");
		if (!inet_ntop(addr_family, (void*)&pktinfo.ipi_spec_dst, to_addr, sizeof(to_addr)))
			perror("inet_ntop");
		//printf(" From src addr=%s port=%d\n", inet_ntoa(rem_addr->sin_addr), htons(rem_addr->sin_port));

	} else if (addr_family == AF_INET6) {
		struct sockaddr_in6 *rem_addr = (struct sockaddr_in6 *) my_hdr->msg_name;
		rem_port = rem_addr->sin6_port;
		if (!inet_ntop(addr_family, (void*)&rem_addr->sin6_addr, from_addr, sizeof(from_addr)))
			perror("inet_ntop");
		if (!inet_ntop(addr_family, (void*)&pktinfo6.ipi6_addr, to_addr, sizeof(to_addr)))
			perror("inet_ntop");
	} else {
		fprintf(stderr, "No destination IP data found (ancillary data)\n");
	}

	printf("Got contacted on dst addr=%s ", to_addr);
	printf("From src addr=%s ", from_addr);
	printf("port=%d\n", htons(rem_port));

	if (DEBUG && verbose && addr_family == AF_INET) {
		printf(" Extra data:\n");
		printf(" - Header destination address (pktinfo.ipi_addr)=%s\n", inet_ntoa(pktinfo.ipi_addr));
		printf(" - Interface index (pktinfo.ipi_ifindex)=%d\n", pktinfo.ipi_ifindex);
	}
}

int main(int argc, char *argv[])
{
	int fd;
	struct sockaddr_storage addr, rem_addr; /* Can contain both sockaddr_in and sockaddr_in6 */
	int res, on = 1;
	struct msghdr msghdr;
	struct iovec vec[1];
	char cbuf[512];  /* Buffer for ancillary data */
	char frame[8192];/* Buffer for packet data */
	int c, count = 1000000;
	uint16_t listen_port = PORT;
	int addr_family = AF_INET6; /* Default address family */

	while ((c = getopt(argc, argv, "c:l:64v:")) != -1) {
		if (c == 'c') count = atoi(optarg);
		if (c == 'l') listen_port  = atoi(optarg);
		if (c == '4') addr_family = AF_INET;
		if (c == '6') addr_family = AF_INET6;
		if (c == 'v') verbose = atoi(optarg);
	}

	fd = socket(addr_family, SOCK_DGRAM, 0);

	memset(&addr, 0, sizeof(addr));

	if (addr_family == AF_INET) {
		struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
		addr4->sin_family = addr_family;
		addr4->sin_port   = htons(listen_port);
	} else if (addr_family == AF_INET6) {
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
		addr6->sin6_family= addr_family;
		addr6->sin6_port  = htons(listen_port);
		// addr6->sin6_addr  = in6addr_any;
		// inet_pton( AF_INET6, "::", (void *)&addr6->sin6_addr.s6_addr);
	}

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		perror("bind");
		return 1;
	}

	/* Socket options to get data on local destination IP */
	setsockopt(fd, SOL_IP, IP_PKTINFO, &on, sizeof(on)); /* man ip(7) */
	setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on)); /* man ipv6(7)*/

	while (1) {
		memset(&msghdr, 0, sizeof(msghdr));
		msghdr.msg_control = cbuf;
		msghdr.msg_controllen = sizeof(cbuf);
		msghdr.msg_iov = vec;
		msghdr.msg_iovlen = 1;
		vec[0].iov_base = frame;
		vec[0].iov_len = sizeof(frame);
		msghdr.msg_name = &rem_addr; /* Remote addr, updated on recv, used on send */
		msghdr.msg_namelen = sizeof(rem_addr);
		res = recvmsg(fd, &msghdr, 0);
		if (res == -1)
			break;

		if (verbose > 0) {
			print_info(&msghdr);
			printf(" Echo back packet, size=%d\n", res);
		}
		/* ok, just echo reply this frame.
		 * Using sendmsg() will provide IP_PKTINFO back to kernel
		 * to let it use the 'right' source address
		 * (destination address of the incoming packet)
		 */
		vec[0].iov_len = res;
		sendmsg(fd, &msghdr, 0);
		if (--count == 0)
			break;
	}
	return 0;
}
