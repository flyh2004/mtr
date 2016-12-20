/*
    mtr  --  a network diagnostic tool
    Copyright (C) 2016  Matt Kimball

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2 as
    published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "probe.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "platform.h"
#include "protocols.h"
#include "construct_unix.h"
#include "deconstruct_unix.h"
#include "timeval.h"

/*  A wrapper around sendto for mixed IPv4 and IPv6 sending  */
static
int send_packet(
    const struct net_state_t *net_state,
    const struct probe_param_t *param,
    const char *packet,
    int packet_size,
    const struct sockaddr_storage *sockaddr)
{
    int send_socket = 0;
    int sockaddr_length;

    if (sockaddr->ss_family == AF_INET6) {
        sockaddr_length = sizeof(struct sockaddr_in6);

        if (param->protocol == IPPROTO_ICMP) {
            send_socket = net_state->platform.icmp6_send_socket;
        } else if (param->protocol == IPPROTO_UDP) {
            send_socket = net_state->platform.udp6_send_socket;
        }
    } else if (sockaddr->ss_family == AF_INET) {
        sockaddr_length = sizeof(struct sockaddr_in);

        send_socket = net_state->platform.ip4_send_socket;
    }

    if (send_socket == 0) {
        errno = EINVAL;
        return -1;
    }

    return sendto(
        send_socket, packet, packet_size, 0,
        (struct sockaddr *)sockaddr, sockaddr_length);
}

/*
    Nearly all fields in the IP header should be encoded in network byte
    order prior to passing to send().  However, the required byte order of
    the length field of the IP header is inconsistent between operating
    systems and operating system versions.  FreeBSD 11 requires the length
    field in network byte order, but some older versions of FreeBSD
    require host byte order.  OS X requires the length field in host
    byte order.  Linux will accept either byte order.

    Test for a byte order which works by sending a ping to localhost.
*/
static
void check_length_order(
    struct net_state_t *net_state)
{
    char packet[PACKET_BUFFER_SIZE];
    struct probe_param_t param;
    struct sockaddr_storage dest_sockaddr;
    ssize_t bytes_sent;
    int packet_size;

    memset(&param, 0, sizeof(struct probe_param_t));
    param.ip_version = 4;
    param.protocol = IPPROTO_ICMP;
    param.ttl = 255;
    param.address = "127.0.0.1";

    if (decode_dest_addr(&param, &dest_sockaddr)) {
        fprintf(stderr, "Error decoding localhost address\n");
        exit(1);
    }

    /*  First attempt to ping the localhost with network byte order  */
    net_state->platform.ip_length_host_order = false;

    packet_size = construct_packet(
        net_state, NULL, MIN_PORT,
        packet, PACKET_BUFFER_SIZE, &dest_sockaddr, &param);
    if (packet_size < 0) {
        perror("Unable to send to localhost");
        exit(1);
    }

    bytes_sent = send_packet(
        net_state, &param, packet, packet_size, &dest_sockaddr);
    if (bytes_sent > 0) {
        return;
    }

    /*  Since network byte order failed, try host byte order  */
    net_state->platform.ip_length_host_order = true;

    packet_size = construct_packet(
        net_state, NULL, MIN_PORT,
        packet, PACKET_BUFFER_SIZE, &dest_sockaddr, &param);
    if (packet_size < 0) {
        perror("Unable to send to localhost");
        exit(1);
    }

    bytes_sent = send_packet(
        net_state, &param, packet, packet_size, &dest_sockaddr);
    if (bytes_sent < 0) {
        perror("Unable to send with swapped length");
        exit(1);
    }
}

/*
    Check to see if SCTP is support.  We can't just rely on checking
    if IPPROTO_SCTP is defined, because while that is necessary,
    MacOS as of "Sierra" defines IPPROTO_SCTP, but creating an SCTP
    socket results in an error.
*/
static
void check_sctp_support(
    struct net_state_t *net_state)
{
    int sctp_socket;

#ifdef IPPROTO_SCTP
    sctp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (sctp_socket != -1) {
        close(sctp_socket);

        net_state->platform.sctp_support = true;
    }
#endif
}

/*  Set a socket to non-blocking mode  */
void set_socket_nonblocking(
    int socket)
{
    int flags;

    flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1) {
        perror("Unexpected socket F_GETFL error");
        exit(1);
    }

    if (fcntl(socket, F_SETFL, flags | O_NONBLOCK)) {
        perror("Unexpected socket F_SETFL O_NONBLOCK error");
        exit(1);
    }
}

/*  Open the raw sockets for sending/receiving IPv4 packets  */
static
void open_ip4_sockets(
    struct net_state_t *net_state)
{
    int send_socket;
    int recv_socket;
    int trueopt = 1;

    send_socket = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (send_socket == -1) {
        perror("Failure opening IPv4 send socket");
        exit(1);
    }

    /*
        We will be including the IP header in transmitted packets.
        Linux doesn't require this, but BSD derived network stacks do.
    */
    if (setsockopt(
        send_socket, IPPROTO_IP, IP_HDRINCL, &trueopt, sizeof(int))) {

        perror("Failure to set IP_HDRINCL");
        exit(1);
    }

    /*
        Open a second socket with IPPROTO_ICMP because we are only
        interested in receiving ICMP packets, not all packets.
    */
    recv_socket = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (recv_socket == -1) {
        perror("Failure opening IPv4 receive socket");
        exit(1);
    }

    net_state->platform.ip4_send_socket = send_socket;
    net_state->platform.ip4_recv_socket = recv_socket;
}

/*  Open the raw sockets for sending/receiving IPv6 packets  */
static
void open_ip6_sockets(
    struct net_state_t *net_state)
{
    int send_socket_icmp;
    int send_socket_udp;
    int recv_socket;

    send_socket_icmp = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (send_socket_icmp == -1) {
        perror("Failure opening ICMPv6 send socket");
        exit(1);
    }

    send_socket_udp = socket(AF_INET6, SOCK_RAW, IPPROTO_UDP);
    if (send_socket_udp == -1) {
        perror("Failure opening UDPv6 send socket");
        exit(1);
    }

    recv_socket = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (recv_socket == -1) {
        perror("Failure opening IPv6 receive socket");
        exit(1);
    }

    set_socket_nonblocking(recv_socket);

    net_state->platform.icmp6_send_socket = send_socket_icmp;
    net_state->platform.udp6_send_socket = send_socket_udp;
    net_state->platform.ip6_recv_socket = recv_socket;
}

/*
    The first half of the net state initialization.  Since this
    happens with elevated privileges, this is kept as minimal
    as possible to minimize security risk.
*/
void init_net_state_privileged(
    struct net_state_t *net_state)
{
    memset(net_state, 0, sizeof(struct net_state_t));

    net_state->platform.next_port = MIN_PORT;

    open_ip4_sockets(net_state);
    open_ip6_sockets(net_state);
}

/*
    The second half of net state initialization, which is run
    at normal privilege levels.
*/
void init_net_state(
    struct net_state_t *net_state)
{
    set_socket_nonblocking(net_state->platform.ip4_recv_socket);
    set_socket_nonblocking(net_state->platform.ip6_recv_socket);

    check_length_order(net_state);
    check_sctp_support(net_state);
}

/*  Returns true if we can transmit probes using the specified protocol  */
bool is_protocol_supported(
    struct net_state_t *net_state,
    int protocol)
{
    if (protocol == IPPROTO_ICMP) {
        return true;
    }

    if (protocol == IPPROTO_UDP) {
        return true;
    }

    if (protocol == IPPROTO_TCP) {
        return true;
    }

#ifdef IPPROTO_SCTP
    if (protocol == IPPROTO_SCTP) {
        return net_state->platform.sctp_support;
    }
#endif

    return false;
}

/*  Report an error during send_probe based on the errno value  */
static
void report_packet_error(
    int command_token)
{
    if (errno == EINVAL) {
        printf("%d invalid-argument\n", command_token);
    } else if (errno == ENETDOWN) {
        printf("%d network-down\n", command_token);
    } else if (errno == ENETUNREACH) {
        printf("%d no-route\n", command_token);
    } else if (errno == EPERM) {
        printf("%d permission-denied\n", command_token);
    } else if (errno == EADDRINUSE) {
        printf("%d address-in-use\n", command_token);
    } else {
        printf("%d unexpected-error errno %d\n", command_token, errno);
    }
}

/*  Craft a custom ICMP packet for a network probe.  */
void send_probe(
    struct net_state_t *net_state,
    const struct probe_param_t *param)
{
    char packet[PACKET_BUFFER_SIZE];
    struct probe_t *probe;
    int packet_size;

    probe = alloc_probe(net_state, param->command_token);
    if (probe == NULL) {
        printf("%d probes-exhausted\n", param->command_token);
        return;
    }

    if (decode_dest_addr(param, &probe->remote_addr)) {
        printf("%d invalid-argument\n", param->command_token);
        free_probe(probe);
        return;
    }

    if (gettimeofday(&probe->platform.departure_time, NULL)) {
        perror("gettimeofday failure");
        exit(1);
    }

    packet_size = construct_packet(
        net_state, &probe->platform.socket, probe->port,
        packet, PACKET_BUFFER_SIZE, &probe->remote_addr, param);

    if (packet_size < 0) {
        /*
            When using a stream protocol, FreeBSD will return ECONNREFUSED
            when connecting to localhost if the port doesn't exist,
            even if the socket is non-blocking, so we should be
            prepared for that.
        */
        if (errno == ECONNREFUSED) {
            receive_probe(probe, ICMP_ECHOREPLY, &probe->remote_addr, NULL);
        } else {
            report_packet_error(param->command_token);
            free_probe(probe);
        }

        return;
    }

    if (packet_size > 0) {
        if (send_packet(
                net_state, param,
                packet, packet_size, &probe->remote_addr) == -1) {

            report_packet_error(param->command_token);
            free_probe(probe);
            return;
        }
    }

    probe->platform.timeout_time = probe->platform.departure_time;
    probe->platform.timeout_time.tv_sec += param->timeout;
}

/*  When allocating a probe, assign it a unique port number  */
void platform_alloc_probe(
    struct net_state_t *net_state,
    struct probe_t *probe)
{
    probe->port = net_state->platform.next_port++;

    if (net_state->platform.next_port > MAX_PORT) {
        net_state->platform.next_port = MIN_PORT;
    }
}

/*
    When freeing the probe, close the socket for the probe,
    if one has been opened
*/
void platform_free_probe(
    struct probe_t *probe)
{
    if (probe->platform.socket) {
        close(probe->platform.socket);
        probe->platform.socket = 0;
    }
}

/*
    Compute the round trip time of a just-received probe and pass it
    to the platform agnostic response handling.
*/
void receive_probe(
    struct probe_t *probe,
    int icmp_type,
    const struct sockaddr_storage *remote_addr,
    struct timeval *timestamp)
{
    unsigned int round_trip_us;
    struct timeval *departure_time = &probe->platform.departure_time;
    struct timeval now;

    if (timestamp == NULL) {
        if (gettimeofday(&now, NULL)) {
            perror("gettimeofday failure");
            exit(1);
        }

        timestamp = &now;
    }

    round_trip_us =
        (timestamp->tv_sec - departure_time->tv_sec) * 1000000 +
        timestamp->tv_usec - departure_time->tv_usec;

    respond_to_probe(probe, icmp_type, remote_addr, round_trip_us);
}

/*
    Read all available packets through our receiving raw socket, and
    handle any responses to probes we have preivously sent.
*/
static
void receive_replies_from_icmp_socket(
    struct net_state_t *net_state,
    int socket,
    received_packet_func_t handle_received_packet)
{
    char packet[PACKET_BUFFER_SIZE];
    int packet_length;
    struct sockaddr_storage remote_addr;
    socklen_t sockaddr_length;
    struct timeval timestamp;

    /*  Read until no more packets are available  */
    while (true) {
        sockaddr_length = sizeof(struct sockaddr_storage);
        packet_length = recvfrom(
            socket, packet, PACKET_BUFFER_SIZE, 0,
            (struct sockaddr *)&remote_addr, &sockaddr_length);

        /*
            Get the time immediately after reading the packet to
            keep the timing as precise as we can.
        */
        if (gettimeofday(&timestamp, NULL)) {
            perror("gettimeofday failure");
            exit(1);
        }

        if (packet_length == -1) {
            /*
                EAGAIN will be returned if there is no current packet
                available.
            */
            if (errno == EAGAIN) {
                return;
            }

            /*
                EINTER will be returned if we received a signal during
                receive.
            */
            if (errno == EINTR) {
                continue;
            }

            perror("Failure receiving replies");
            exit(1);
        }

        handle_received_packet(
            net_state, &remote_addr, packet, packet_length, &timestamp);
    }
}

/*
    Attempt to send using the probe's socket, in order to check whether
    the connection has completed, for stream oriented protocols such as
    TCP.
*/
static
void receive_replies_from_probe_socket(
    struct net_state_t *net_state,
    struct probe_t *probe)
{
    int probe_socket;
    struct timeval zero_time;
    int err;
    int err_length = sizeof(int);
    fd_set write_set;

    probe_socket = probe->platform.socket;
    if (!probe_socket) {
        return;
    }

    FD_ZERO(&write_set);
    FD_SET(probe_socket, &write_set);

    zero_time.tv_sec = 0;
    zero_time.tv_usec = 0;

    if (select(probe_socket + 1, NULL, &write_set, NULL, &zero_time) == -1) {
        if (errno == EAGAIN) {
            return;
        } else {
            perror("probe socket select error");
            exit(1);
        }
    }

    /*
        If the socket is writable, the connection attempt has completed.
    */
    if (!FD_ISSET(probe_socket, &write_set)) {
        return;
    }

    if (getsockopt(probe_socket, SOL_SOCKET, SO_ERROR, &err, &err_length)) {
        perror("probe socket SO_ERROR");
        exit(1);
    }

    /*
        If the connection complete successfully, or was refused, we can
        assume our probe arrived at the destination.
    */
    if (!err || err == ECONNREFUSED) {
        receive_probe(probe, ICMP_ECHOREPLY, &probe->remote_addr, NULL);
    } else {
        errno = err;
        report_packet_error(probe->token);
        free_probe(probe);
    }
}

/*  Check both the IPv4 and IPv6 sockets for incoming packets  */
void receive_replies(
    struct net_state_t *net_state)
{
    int i;
    struct probe_t *probe;

    receive_replies_from_icmp_socket(
        net_state, net_state->platform.ip4_recv_socket,
        handle_received_ip4_packet);

    receive_replies_from_icmp_socket(
        net_state, net_state->platform.ip6_recv_socket,
        handle_received_ip6_packet);

    for (i = 0; i < MAX_PROBES; i++) {
        probe = &net_state->probes[i];

        if (probe->used) {
            receive_replies_from_probe_socket(net_state, probe);
        }
    }
}

/*
    Put all of our probe sockets in the read set used for an upcoming
    select so we can wake when any of them become readable.
*/
int gather_probe_sockets(
    const struct net_state_t *net_state,
    fd_set *write_set)
{
    int i;
    int probe_socket;
    int nfds;
    const struct probe_t *probe;

    nfds = 0;

    for (i = 0; i < MAX_PROBES; i++) {
        probe = &net_state->probes[i];
        probe_socket = probe->platform.socket;

        if (probe->used && probe_socket) {
            FD_SET(probe_socket, write_set);
            if (probe_socket >= nfds) {
                nfds = probe_socket + 1;
            }
        }
    }

    return nfds;
}

/*
    Check for any probes for which we have not received a response
    for some time, and report a time-out, assuming that we won't
    receive a future reply.
*/
void check_probe_timeouts(
    struct net_state_t *net_state)
{
    struct timeval now;
    struct probe_t *probe;
    int i;

    if (gettimeofday(&now, NULL)) {
        perror("gettimeofday failure");
        exit(1);
    }

    for (i = 0; i < MAX_PROBES; i++) {
        probe = &net_state->probes[i];

        /*  Don't check probes which aren't currently outstanding  */
        if (!probe->used) {
            continue;
        }

        if (compare_timeval(probe->platform.timeout_time, now) < 0) {
            /*  Report timeout to the command stream  */
            printf("%d no-reply\n", probe->token);

            free_probe(probe);
        }
    }
}

/*
    Find the remaining time until the next probe times out.
    This may be a negative value if the next probe timeout has
    already elapsed.

    Returns false if no probes are currently outstanding, and true
    if a timeout value for the next probe exists.
*/
bool get_next_probe_timeout(
    const struct net_state_t *net_state,
    struct timeval *timeout)
{
    int i;
    bool have_timeout;
    const struct probe_t *probe;
    struct timeval now;
    struct timeval probe_timeout;

    if (gettimeofday(&now, NULL)) {
        perror("gettimeofday failure");
        exit(1);
    }

    have_timeout = false;
    for (i = 0; i < MAX_PROBES; i++) {
        probe = &net_state->probes[i];
        if (!probe->used) {
            continue;
        }

        probe_timeout.tv_sec =
            probe->platform.timeout_time.tv_sec - now.tv_sec;
        probe_timeout.tv_usec =
            probe->platform.timeout_time.tv_usec - now.tv_usec;

        normalize_timeval(&probe_timeout);
        if (have_timeout) {
            if (compare_timeval(probe_timeout, *timeout) < 0) {
                /*  If this probe has a sooner timeout, store it instead  */
                *timeout = probe_timeout;
            }
        } else {
            *timeout = probe_timeout;
            have_timeout = true;
        }
    }

    return have_timeout;
}
