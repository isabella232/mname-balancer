


#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <inttypes.h>
#include <unistd.h>
#include <strings.h>
#include <err.h>
#include <errno.h>
#include <port.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>

#include <sys/debug.h>
#include <sys/list.h>
#include <sys/avl.h>

#include <libcbuf.h>
#include <libcloop.h>
#include <bunyan.h>

#include "bbal.h"


static int
bbal_parse_ipv4addr(const char *ipaddr, const char *port,
    struct sockaddr_in *addr)
{
	bzero(addr, sizeof (*addr));

	addr->sin_family = AF_INET;
	addr->sin_port = htons(atoi(port));

	switch (inet_pton(AF_INET, ipaddr, &addr->sin_addr)) {
	case 1:
		return (0);

	case 0:
		bunyan_warn(g_log, "invalid IP address",
		    BUNYAN_T_STRING, "address", ipaddr,
		    BUNYAN_T_END);
		errno = EPROTO;
		return (-1);

	default:
		bunyan_warn(g_log, "failed to parse IP address",
		    BUNYAN_T_STRING, "address", ipaddr,
		    BUNYAN_T_END);
		return (-1);
	}
}

int
bbal_udp_listen(const char *ipaddr, const char *port, int *sockp)
{
	int e;
	int sock = -1;
	struct sockaddr_in addr;
	const char *msg;

	bunyan_debug(g_log, "opening UDP listen socket",
	    BUNYAN_T_STRING, "address", ipaddr,
	    BUNYAN_T_STRING, "port", port,
	    BUNYAN_T_END);

	if (bbal_parse_ipv4addr(ipaddr != NULL ? ipaddr : "0.0.0.0", port,
	    &addr) != 0) {
		e = errno;
		goto fail;
	}

	/*
	 * Create UDP listen socket.
	 */
	if ((sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
	    0)) < 0) {
		e = errno;
		msg = "failed to create UDP listen socket";
		goto fail;
	}

	/*
	 * Set socket options.
	 */
	int opt_on = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt_on,
	    sizeof (opt_on)) != 0) {
		e = errno;
		msg = "failed to set SO_REUSEADDR on UDP listen socket";
		goto fail;
	}

	/*
	 * Bind to the socket.
	 */
	if (bind(sock, (struct sockaddr *)&addr, sizeof (addr)) != 0) {
		e = errno;
		msg = "failed to bind UDP listen socket";
		goto fail;
	}

	bunyan_info(g_log, "listening for UDP packets",
	    BUNYAN_T_STRING, "address", ipaddr,
	    BUNYAN_T_STRING, "port", port,
	    BUNYAN_T_END);

	*sockp = sock;
	return (0);

fail:
	bunyan_fatal(g_log, msg,
	    BUNYAN_T_STRING, "address", ipaddr,
	    BUNYAN_T_STRING, "port", port,
	    BUNYAN_T_INT32, "errno", (int32_t)e,
	    BUNYAN_T_STRING, "strerror", strerror(e),
	    BUNYAN_T_END);
	if (sock != -1) {
		VERIFY0(close(sock));
	}
	errno = e;
	return (-1);
}

void
bbal_udp_read(cloop_ent_t *ent, int event)
{
	cbuf_t *buf = NULL;
	uint32_t packet_count = 0;

another_packet:
	if (cbuf_alloc(&buf, 2048) != 0) {
		goto bail;
	}
	cbuf_byteorder_set(buf, CBUF_ORDER_LITTLE_ENDIAN);

	/*
	 * This packet needs a header, but the header includes the length of
	 * the UDP packet data.  Move the buffer position forward so that we
	 * leave room to come back and write the header after receiving the
	 * packet.
	 */
	size_t hdrsz = 4 * sizeof (uint32_t);
	VERIFY0(cbuf_position_set(buf, hdrsz));

	/*
	 * Read a UDP packet.
	 */
	struct sockaddr_storage from;
	socklen_t fromlen = sizeof (from);
	size_t rsz;
again:
	if (cbuf_sys_recvfrom(buf, cloop_ent_fd(ent),
	    CBUF_SYSREAD_ENTIRE, &rsz, 0, (struct sockaddr *)&from,
	    &fromlen) != 0) {
		switch (errno) {
		case EINTR:
			goto again;

		case EAGAIN:
			bunyan_trace(g_log, "ran out of UDP packets",
			    BUNYAN_T_UINT32, "packet_count", packet_count,
			    BUNYAN_T_END);
			goto bail;

		default:
			VERIFY3S(errno, ==, 0);
		}
	}

	const struct sockaddr_in *sin = (struct sockaddr_in *)&from;
	remote_t *rem = remote_lookup(&sin->sin_addr);
	if (rem == NULL) {
		bunyan_error(g_log, "could not lookup remote (dropping)",
		    BUNYAN_T_IP, "remote_ip", &sin->sin_addr,
		    BUNYAN_T_END);
		goto bail;
	}

	if (rsz > 1500) {
		/*
		 * We do not expect to receive UDP messages reassembled from
		 * multiple fragments.  RFC compliant clients have no reason
		 * to send us particularly large packets.  If we see something
		 * unexpectedly long, log a warning.
		 */
		bunyan_warn(rem->rem_log, "dropping oversized UDP packet",
		    BUNYAN_T_UINT32, "len", (uint32_t)rsz,
		    BUNYAN_T_END);
		goto bail;
	}

	bunyan_trace(rem->rem_log, "received UDP packet from remote",
	    BUNYAN_T_UINT32, "remote_port", (uint32_t)ntohs(sin->sin_port),
	    BUNYAN_T_UINT32, "len", (uint32_t)rsz,
	    BUNYAN_T_END);

	backend_t *be = remote_backend(rem);
	if (be == NULL) {
		bunyan_trace(rem->rem_log, "could not find backend for remote",
		    BUNYAN_T_END);

		rem->rem_stat_udp_drop++;
		goto bail;
	}

	rem->rem_stat_udp++;
	be->be_stat_udp++;

	/*
	 * Preserve the final position so that we can go back and add the
	 * header.
	 */
	size_t p = cbuf_position(buf);
	cbuf_rewind(buf);

	/*
	 * An inbound UDP packet is wrapped in a frame of type 2.  The header
	 * consists of four little endian numbers, followed by the data from
	 * the packet.
	 *
	 * 	OFFSET	LENGTH	DESCRIPTION
	 * 	--------------------------------------------------------------
	 * 	0	4	frame type (INBOUND_UDP = 2)
	 * 	4	4	IPv4 address of remote peer
	 * 	8	4	UDP source port for this packet
	 * 	12	4	length of data in frame (not including header)
	 * 	--------------------------------------------------------------
	 * 	16	N	frame data
	 * 	--------------------------------------------------------------
	 */
	VERIFY0(cbuf_put_u32(buf, 2)); /* FRAME TYPE */
	VERIFY0(cbuf_put_u32(buf, ntohl(sin->sin_addr.s_addr))); /* IP */
	VERIFY0(cbuf_put_u32(buf, ntohs(sin->sin_port))); /* PORT */
	VERIFY0(cbuf_put_u32(buf, (uint32_t)rsz)); /* FRAME DATA LENGTH */

	VERIFY0(cbuf_position_set(buf, p));

	bunyan_trace(be->be_log, "forwarding UDP packet to backend",
	    BUNYAN_T_IP, "remote_ip", &sin->sin_addr,
	    BUNYAN_T_UINT32, "remote_port", (uint32_t)ntohs(sin->sin_port),
	    BUNYAN_T_UINT32, "len", (uint32_t)rsz,
	    BUNYAN_T_END);

	if (cconn_send(be->be_conn, buf) != 0) {
		bunyan_warn(be->be_log, "failed to send packet to backend",
		    BUNYAN_T_IP, "remote_ip", &sin->sin_addr,
		    BUNYAN_T_UINT32, "remote_port", (uint32_t)ntohs(
		    sin->sin_port),
		    BUNYAN_T_UINT32, "len", (uint32_t)rsz,
		    BUNYAN_T_INT32, "errno", (int32_t)errno,
		    BUNYAN_T_STRING, "strerror", strerror(errno),
		    BUNYAN_T_END);

		rem->rem_stat_udp_drop++;

		/*
		 * Abort this connection to the backend.  This will result
		 * in at least a CLOSE callback, where we can trigger a
		 * reconnection.
		 */
		cconn_abort(be->be_conn);
	}
	buf = NULL;

	if (packet_count < 16) {
		/*
		 * In case more than one UDP packet has arrived, try to
		 * read again.  If there's nothing left, we'll get EAGAIN and
		 * bail out.
		 */
		packet_count++;
		goto another_packet;
	}

	/*
	 * Ask for more UDP packets.
	 */
	cloop_ent_want(ent, CLOOP_CB_READ);
	return;

bail:
	cbuf_free(buf);
	cloop_ent_want(ent, CLOOP_CB_READ);
}