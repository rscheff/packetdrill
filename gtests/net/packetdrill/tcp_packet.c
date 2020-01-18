/*
 * Copyright 2013 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */
/*
 * Author: ncardwell@google.com (Neal Cardwell)
 *
 * Implementation for module for formatting TCP packets.
 */

#include "tcp_packet.h"

#include "ip_packet.h"
#include "tcp.h"

/*
 * The full list of valid TCP bit flag characters
 * numeric 0..7 used as shorthands for the ACE field
 * Note that the parser will accept the dot only as
 * last character in a packetdrill script, and numerals
 * should come after any letters.
 *
 * In the list of valid flags, the dot as the most
 * common flag, is placed first
 */
static const char valid_tcp_flags[] = ".FSRPEWA01234567";
static const char ace_tcp_flags[] = "01234567";
static const char ecn_tcp_flags[] = "EWA";

/* Are all the TCP flags in the given string valid? */
static bool is_tcp_flags_spec_valid(const char *flags, char **error)
{
	const char *s;
	bool has_ecn_flag = false;
	bool has_ace_flag = false;

	for (s = flags; *s != '\0'; ++s) {
		if (!strchr(valid_tcp_flags, *s)) {
			asprintf(error, "Invalid TCP flag: '%c'", *s);
			return false;
		}
		if (strchr(ecn_tcp_flags, *s)) {
			has_ecn_flag = true;
			if (has_ace_flag) {
				asprintf(error, "Conflicting TCP flag: '%c'", *s);
				return false;
			}
		}
		if (strchr(ace_tcp_flags, *s)) {
			if (has_ecn_flag) {
				asprintf(error, "Conflicting TCP flag: '%c'", *s);
				return false;
			}
			if (!has_ace_flag) {
				has_ace_flag = true;
			} else {
				asprintf(error, "Conflicting TCP flag: '%c'", *s);
				return false;
			}
		}
	}
	return true;
}

/* Parse tcpdump-style ASCII representation of flags to look for a flag */
static inline int is_tcp_flag_set(char flag, const char *flags)
{
	return (strchr(flags, flag) != NULL) ? 1 : 0;
}

/* find first numeric flag for ACE */
static inline int tcp_flag_ace_count( const char *flags)
{
	const char *s;

	for (s = flags; *s != '\0'; ++s) {
		if (strchr(ace_tcp_flags, *s)) {
			return ((int)*s - (int)'0');
		}
	}
	return 0;
}

struct packet *new_tcp_packet(int address_family,
			       enum direction_t direction,
			       enum ip_ecn_t ecn,
			       const char *flags,
			       u32 start_sequence,
			       u16 tcp_payload_bytes,
			       u32 ack_sequence,
			       s32 window,
			       struct tcp_options *tcp_options,
			       bool ignore_ts_val,
			       bool abs_ts_ecr,
			       bool abs_seq,
			       bool ignore_seq,
			       u16 udp_src_port,
			       u16 udp_dst_port,
			       char **error)
{
	struct packet *packet = NULL;  /* the newly-allocated result packet */
	struct header *tcp_header, *udp_header;
	/* Calculate lengths in bytes of all sections of the packet */
	const int ip_option_bytes = 0;
	const int tcp_option_bytes = tcp_options ? tcp_options->length : 0;
	const int ip_header_bytes = (ip_header_min_len(address_family) +
				     ip_option_bytes);
	const int udp_header_bytes = sizeof(struct udp);
	const int tcp_header_bytes = sizeof(struct tcp) + tcp_option_bytes;
	int ip_bytes;
	bool encapsulate = (udp_src_port > 0) || (udp_dst_port > 0);
	int ace;

	/* Sanity-check all the various lengths */
	if (ip_option_bytes & 0x3) {
		asprintf(error, "IP options are not padded correctly "
			 "to ensure IP header is a multiple of 4 bytes: "
			 "%d excess bytes", ip_option_bytes & 0x3);
		return NULL;
	}
	if (tcp_option_bytes & 0x3) {
		asprintf(error,
			 "TCP options are not padded correctly "
			 "to ensure TCP header is a multiple of 4 bytes: "
			 "%d excess bytes", tcp_option_bytes & 0x3);
		return NULL;
	}
	assert((tcp_header_bytes & 0x3) == 0);
	assert((ip_header_bytes & 0x3) == 0);

	if (tcp_header_bytes > MAX_TCP_HEADER_BYTES) {
		asprintf(error, "TCP header too large");
		return NULL;
	}

	ip_bytes = ip_header_bytes + tcp_header_bytes + tcp_payload_bytes;
	if (encapsulate) {
		ip_bytes += udp_header_bytes;
	}
	if (ip_bytes > MAX_TCP_DATAGRAM_BYTES) {
		asprintf(error, "TCP segment too large");
		return NULL;
	}

	if (!is_tcp_flags_spec_valid(flags, error))
		return NULL;

	/* Allocate and zero out a packet object of the desired size */
	packet = packet_new(ip_bytes);
	memset(packet->buffer, 0, ip_bytes);

	packet->direction = direction;
	packet->flags = encapsulate ? FLAGS_UDP_ENCAPSULATED : 0;
	packet->ecn = ecn;

	/* Set IP header fields */
	if (encapsulate) {
		set_packet_ip_header(packet, address_family, ip_bytes, ecn,
				     IPPROTO_UDP);
		udp_header = packet_append_header(packet, HEADER_UDP, udp_header_bytes);
		udp_header->total_bytes = udp_header_bytes + tcp_header_bytes + tcp_payload_bytes;
		udp_header->h.udp->src_port = htons(udp_src_port);
		udp_header->h.udp->dst_port = htons(udp_dst_port);
		udp_header->h.udp->len = htons(udp_header_bytes + tcp_header_bytes + tcp_payload_bytes);
		udp_header->h.udp->check = htons(0);
	} else {
		set_packet_ip_header(packet, address_family, ip_bytes, ecn,
				     IPPROTO_TCP);
	}

	tcp_header = packet_append_header(packet, HEADER_TCP, tcp_header_bytes);
	tcp_header->total_bytes = tcp_header_bytes + tcp_payload_bytes;

	/* Find the start of TCP sections of the packet */
	if (encapsulate) {
		packet->tcp = (struct tcp *) (ip_start(packet) + ip_header_bytes + udp_header_bytes);
	} else {
		packet->tcp = (struct tcp *) (ip_start(packet) + ip_header_bytes);
	}
	u8 *tcp_option_start = (u8 *) (packet->tcp + 1);

	/* Set TCP header fields */
	packet->tcp->src_port = htons(0);
	packet->tcp->dst_port = htons(0);
	packet->tcp->seq = htonl(start_sequence);
	packet->tcp->ack_seq = htonl(ack_sequence);
	packet->tcp->doff = tcp_header_bytes / 4;
	if (window == -1) {
		if (direction == DIRECTION_INBOUND) {
			asprintf(error, "window must be specified"
				 " for inbound packets");
			packet_free(packet);
			return NULL;
		}
		packet->tcp->window = 0;
		packet->flags |= FLAG_WIN_NOCHECK;
	} else {
		packet->tcp->window = htons(window);
	}
	packet->tcp->check = 0;
	packet->tcp->urg_ptr = 0;
	packet->tcp->fin = is_tcp_flag_set('F', flags);
	packet->tcp->syn = is_tcp_flag_set('S', flags);
	packet->tcp->rst = is_tcp_flag_set('R', flags);
	packet->tcp->psh = is_tcp_flag_set('P', flags);
	packet->tcp->ack = is_tcp_flag_set('.', flags);
	packet->tcp->urg = 0;

	if ((ace = tcp_flag_ace_count(flags)) != 0) {
		/* after validity check, ACE value doesn't coexist with ECN flags */
		packet->tcp->ece = ((ace & 1) == 1);
		packet->tcp->cwr = ((ace & 2) == 2);
		packet->tcp->ae  = ((ace & 4) == 4);
	} else {
		packet->tcp->ece = is_tcp_flag_set('E', flags);
		packet->tcp->cwr = is_tcp_flag_set('W', flags);
		packet->tcp->ae  = is_tcp_flag_set('A', flags);
	}

	if (tcp_options == NULL) {
		packet->flags |= FLAG_OPTIONS_NOCHECK;
	} else if (tcp_options->length > 0) {
		/* Copy TCP options into packet */
		memcpy(tcp_option_start, tcp_options->data,
		       tcp_options->length);
	}

	if (ignore_ts_val) {
		packet->flags |= FLAG_IGNORE_TS_VAL;
	}
	if (abs_ts_ecr) {
		packet->flags |= FLAG_ABSOLUTE_TS_ECR;
	}
	if (abs_seq) {
		packet->flags |= FLAG_ABSOLUTE_SEQ;
	}
	if (ignore_seq) {
		packet->flags |= FLAG_IGNORE_SEQ;
	}

	packet->ip_bytes = ip_bytes;
	return packet;
}
