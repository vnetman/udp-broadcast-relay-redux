/*
******************************************************************
udp-broadcast-relay-redux
    Relays UDP broadcasts to other networks, forging
    the sender address.

Copyright (c) 2017 UDP Broadcast Relay Redux Contributors
  <github.com/udp-redux/udp-broadcast-relay-redux>
Copyright (c) 2003 Joachim Breitner <mail@joachim-breitner.de>
Copyright (C) 2002 Nathan O'Sullivan

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
******************************************************************
*/

#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>

#define MAXIFS 2
#define IF_LEFT 0
#define IF_RIGHT 1

#define DPRINT(...) if (debug_) { \
    if (forked_) { \
    syslog(LOG_DEBUG, __VA_ARGS__); \
    } else { \
    fprintf(stderr, __VA_ARGS__); \
    } \
    }

#define EPRINT(...) if (forked_) { \
    syslog(LOG_ERR, __VA_ARGS__); \
    } else { \
    fprintf(stderr, __VA_ARGS__); \
    }

/* list of addresses and interface numbers on local machine */
struct Iface {
    enum {
        DSTA_INVALID = 0,
        DSTA_BROADCAST,
        DSTA_SPECIFIED
    } dstaddrtype;

    enum {
        SRCA_INVALID = 0,
        SRCA_UNCHANGED,
        SRCA_SPECIFIED,
        SRCA_IFADDR
    } srcaddrtype;

    struct in_addr dstaddr; /* if dstaddrtype == DSTA_SPECIFIED */
    struct in_addr srcaddr; /* if srcaddrtype == SRCA_SPECIFIED */
    unsigned int ifindex;
    int raw_socket;
};
static struct Iface ifs_[MAXIFS] = {0};

#define IFS_LEFT 0
#define IFS_RIGHT 1

static int debug_ = 0;
static int fork_ = 0;
static int forked_ = 0;
static unsigned short udport_ = 0;
static int largest_mtu_ = 0;
static unsigned char echo_marker_ttl_ = 0;

static void print_usage_and_exit(char const *progname) {
    char const *usage =
        "%s --port <udp port> --echo-marker <1-255> --left <interface> --right <interface> \n"
        "--left-src <arg> --left-dest <arg> --right-src <arg> --right-dest <arg>\n"
        "[--debug] [--fork]\n"
        "\n"
        "This program forwards UDP packets addressed to a specific UDP port between\n"
        "two network interfaces (called \"left\" and \"right\"), after rewriting the\n"
        "destination address and, optionally, rewriting the source address before\n"
        "forwarding.\n"
        "\n"
        "--port <udp port>  the UDP destination port to process\n"
	"--echo-marker <n>  the TTL value set on outgoing packets, to recognize\n"
	"                   packets echoed back (1-255)\n"
        "--left <name>      the name of the left interface\n"
        "--right <name>     the name of the right interface\n"
        "--left-src <arg>   this affects the source address that is set on packets\n"
        "                   that arrive on right and are forwarded to left.\n"
        "                   <arg> must have one of the following values:\n"
        "                   \"unchanged\": retain the original source address\n"
        "                   \"ifaddr\"   : use the address of the left interface\n"
        "                   x.x.x.x      : use the specified IP address\n"
        "--left-dst <arg>   this affects the destination address that is set on\n"
        "                   packets that arrive on right and are forwarded to left.\n"
        "                   <arg> must have one of the following values:\n"
        "                   \"broadcast\": use the broadcast address of the network\n"
        "                                  for a point-to-point network, this would\n"
        "                                  use the peer address\n"
        "                   x.x.x.x      : use the specified IP address\n"
        " --right-src <arg> Same as --left-src, except this applies to the left to\n"
        "                   right direction\n"
        " --right-dst <arg> Same as --left-dst, except this applies to the right to\n"
        "                   left direction\n"
        "--debug            enable debug logs on stdout\n"
        "--fork             run in the background\n";
    printf(usage, progname);
    exit(1);
}

/* Utility function to compute the UDP Header checksum */
static unsigned short udp_csum(struct iphdr *ip, struct udphdr *udp,
                               unsigned char *payload,
                               size_t payload_length) {
  unsigned short *sptr;
  unsigned long sum = 0;

  /* Pseudo header */
  /* iphdr fields are already laid out in network order */
  sptr = (unsigned short *) &(ip->saddr);
  sum += ntohs(sptr[0]);  /* ip src msw */
  sum += ntohs(sptr[1]);  /* ip src lsw */
  sum += ntohs(sptr[2]);  /* ip dst msw */
  sum += ntohs(sptr[3]);  /* ip dst lsw */
  sum += 0x0011;          /* reserved + ip proto for udp */
  sum += ntohs(udp->len); /* udp len */

  /* UDP header */
  sptr = (unsigned short *) udp;
  sum += ntohs(sptr[0]);
  sum += ntohs(sptr[1]);
  sum += ntohs(sptr[2]);
  sum += ntohs(sptr[3]);

  /* Payload */
  sptr = (unsigned short *) payload;
  while (payload_length >= 2) {
    sum += ntohs(*sptr++);
    payload_length -= 2;
  }

  if (payload_length != 0) { /* the last byte if payload_length is odd */
    sum += ((unsigned short) *(unsigned char *) sptr) << 8;
  }

  /* Fold the carry until there is no carry */
  while ((sum >> 16) != 0) {
      sum = (sum & 0xffff) + (sum >> 16);
  }

  return ~sum;
}

/* Wrapper around ioctl() */
static int fetch_if_ioctl(int fd_socket, char const *if_name, int req_num,
			  char const *req_num_str, struct ifreq *req) {
    memset(req, 0, sizeof(*req));
    strncpy(req->ifr_name, if_name, IFNAMSIZ);

    if (ioctl(fd_socket, req_num, req) < 0) {
	EPRINT("ioctl error getting %s for %s: %s\n", if_name, req_num_str,
	       strerror(errno));
	return 0;
    }
    return 1;
}

/* Wrapper around the SIOCGIFFLAGS ioctl */
static int fetch_if_flags(int fd_socket, char const *if_name,
			  unsigned short *ptr_flags) {
    struct ifreq req;
    if (!fetch_if_ioctl(fd_socket, if_name, SIOCGIFFLAGS, "SIOCGIFFLAGS", &req)) {
	return 0;
    }
    *ptr_flags = req.ifr_flags;
    return 1;
}

/* Wrapper around the SIOCGIFBRDADDR ioctl */
static int fetch_bcast_address(int fd_socket, char const *if_name,
			       struct in_addr *ptr_result) {
    struct ifreq req;
    if (!fetch_if_ioctl(fd_socket, if_name, SIOCGIFBRDADDR, "SIOCGIFBRDADDR", &req)) {
	return 0;
    }
    memcpy(ptr_result, &((struct sockaddr_in *)&req.ifr_broadaddr)->sin_addr,
	   sizeof(struct in_addr));
    return 1;
}

/* Wrapper around the SIOCGIFADDR ioctl */
static int fetch_if_address(int fd_socket, char const *if_name,
			    struct in_addr *ptr_result) {
    struct ifreq req;
    if (!fetch_if_ioctl(fd_socket, if_name, SIOCGIFADDR, "SIOCGIFADDR", &req)) {
	return 0;
    }
    memcpy(ptr_result, &((struct sockaddr_in *)&req.ifr_addr)->sin_addr,
	   sizeof(struct in_addr));
    return 1;
}

/* Wrapper around the SIOCGIFNETMASK ioctl */
static int fetch_if_netmask(int fd_socket, char const *if_name,
			    struct in_addr *ptr_result) {
    struct ifreq req;
    if (!fetch_if_ioctl(fd_socket, if_name, SIOCGIFNETMASK, "SIOCGIFNETMASK", &req)) {
	return 0;
    }
    memcpy(ptr_result, &((struct sockaddr_in *)&req.ifr_netmask)->sin_addr,
	   sizeof(struct in_addr));
    return 1;
}

/* Wrapper around the SIOCGIFDSTADDR ioctl */
static int fetch_dest_address(int fd_socket, char const *if_name,
			      struct in_addr *ptr_result) {
    struct ifreq req;
    if (!fetch_if_ioctl(fd_socket, if_name, SIOCGIFDSTADDR, "SIOCGIFDSTADDR", &req)) {
	return 0;
    }
    memcpy(ptr_result, &((struct sockaddr_in *)&req.ifr_dstaddr)->sin_addr,
	   sizeof(struct in_addr));
    return 1;
}

/* Wrapper around the SIOCGIFMTU ioctl */
static int fetch_if_mtu(int fd_socket, char const *if_name, int *ptr_mtu) {
    struct ifreq req;
    if (!fetch_if_ioctl(fd_socket, if_name, SIOCGIFMTU, "SIOCGIFMTU", &req)) {
	return 0;
    }
    *ptr_mtu = req.ifr_mtu;
    return 1;
}

/*
 * Set up global variables from command line arguments.
 */
static int parse_command_line(int argc, char **argv) {
    int i;
    char *endptr, *left_if_name = 0, *right_if_name = 0;
    unsigned long ulvalue;
    unsigned int lif = (unsigned int) -1;
    unsigned int rif = (unsigned int) -1;
    int fd_socket_tmp;

    if ((argc < 15) || (argc > 19)) {
        print_usage_and_exit(argv[0]);
    }

    for (i = 1; i < argc; i++) {
        if (0 == strcmp("--port", argv[i])) {
            if (udport_ != 0) {
                EPRINT("\"%s\" specified multiple times\n", argv[i]);
                return 0;
            }
            i++;
            if (i == argc) {
                EPRINT("\"%s\" needs an argument\n", argv[i - 1]);
                return 0;
            }
            ulvalue = strtoul(argv[i], &endptr, 0);
            if (*endptr || !ulvalue || (ulvalue > 65535)) {
                EPRINT("\"%s\" is not a valid value for the UDP port\n", argv[i]);
                return 0;
            }
            udport_ = (unsigned short) ulvalue;
        } else if (0 == strcmp("--echo-marker", argv[i])) {
	    if (echo_marker_ttl_ != 0) {
		EPRINT("ERROR: \"%s\" specified multiple times\n", argv[i]);
                return 0;
	    }
	    i++;
            if (i == argc) {
                EPRINT("\"%s\" needs an argument\n", argv[i - 1]);
                return 0;
            }
            ulvalue = strtoul(argv[i], &endptr, 0);
            if (*endptr || !ulvalue || (ulvalue > 255)) {
                EPRINT("\"%s\" is not a valid value for the echo marker\n", argv[i]);
                return 0;
            }
            echo_marker_ttl_ = (unsigned char) ulvalue;
	} else if (0 == strcmp("--left", argv[i])) {
            if (lif != (unsigned int) -1) {
                EPRINT("ERROR: \"%s\" specified multiple times\n", argv[i]);
                return 0;
            }
            i++;
            if (i == argc) {
                EPRINT("\"%s\" needs an argument\n", argv[i - 1]);
                return 0;
            }
            lif = if_nametoindex(argv[i]);
            if (lif == 0) {
                EPRINT("Interface \"%s\" is invalid: %s\n", argv[i], strerror(errno));
                return 0;
            }
            left_if_name = argv[i];
        } else if (0 == strcmp("--right", argv[i])) {
            if (rif != (unsigned int) -1) {
                EPRINT("ERROR: \"%s\" specified multiple times\n", argv[i]);
                return 0;
            }
            i++;
            if (i == argc) {
                EPRINT("\"--right\" needs an argument\n");
                return 0;
            }
            rif = if_nametoindex(argv[i]);
            if (rif == 0) {
                EPRINT("Interface \"%s\" is invalid: %s\n", argv[i], strerror(errno));
                return 0;
            }
            right_if_name = argv[i];
        } else if ((0 == strcmp("--left-src", argv[i])) ||
                   (0 == strcmp("--right-src", argv[i]))) {
            struct Iface *ifsptr;
            if (argv[i][2] == 'l') {
                ifsptr = &(ifs_[IFS_LEFT]);
            } else {
                ifsptr = &(ifs_[IFS_RIGHT]);
            }

            if (ifsptr->srcaddrtype != SRCA_INVALID) {
                EPRINT("\"%s\" specified more than once\n", argv[i]);
                return 0;
            }
            i++;
            if (i == argc) {
                EPRINT("\"%s\" needs an argument\n", argv[i - 1]);
                return 0;
            }
            if (0 == strcmp(argv[i], "unchanged")) {
                ifsptr->srcaddrtype = SRCA_UNCHANGED;
            } else if (0 == strcmp(argv[i], "ifaddr")) {
                ifsptr->srcaddrtype = SRCA_IFADDR;
            } else {
                if (1 != inet_pton(AF_INET, argv[i], &(ifsptr->srcaddr))) {
                    EPRINT("\"%s\" is not a valid value for \"%s\": "
			   "expecting \"unchanged\", \"ifaddr\" or a valid IPv4 "
			   "address in dotted decimal format.\n", argv[i],
			   argv[i - 1]);
                    return 0;
                } else {
                    ifsptr->srcaddrtype = SRCA_SPECIFIED;
                }
            }
        } else if ((0 == strcmp("--left-dst", argv[i])) ||
                   (0 == strcmp("--right-dst", argv[i]))) {
            struct Iface *ifsptr;
            if (argv[i][2] == 'l') {
                ifsptr = &(ifs_[IFS_LEFT]);
            } else {
                ifsptr = &(ifs_[IFS_RIGHT]);
            }

            if (ifsptr->dstaddrtype != DSTA_INVALID) {
                EPRINT("\"%s\" specified more than once\n", argv[i]);
                return 0;
            }
            i++;
            if (i == argc) {
                EPRINT("\"%s\" needs an argument\n", argv[i - 1]);
                return 0;
            }
            if (0 == strcmp(argv[i], "broadcast")) {
                ifsptr->dstaddrtype = DSTA_BROADCAST;
            } else {
                if (1 != inet_pton(AF_INET, argv[i], &(ifsptr->dstaddr))) {
                    EPRINT("\"%s\" is not a valid value for \"%s\": "
			   "expecting \"broadcast\" or a valid IPv4 address in "
			   "dotted decimal format.\n", argv[i], argv[i - 1]);
                    return 0;
                } else {
                    ifsptr->dstaddrtype = DSTA_SPECIFIED;
                }
            }
        } else if (0 == strcmp("--debug", argv[i])) {
            debug_ = 1;
        } else if (0 == strcmp("--fork", argv[i])) {
            fork_ = 1;
        } else {
            EPRINT("Argument \"%s\" is not understood\n", argv[i]);
            return 0;
        }
    }

    /* Check if we have everything we need */

    if (udport_ == 0) {
        EPRINT("\"--port\" not specified.\n");
        return 0;
    }

    if ((!left_if_name) || (lif == (unsigned int) -1)) {
        EPRINT("\"--left\" not specified.\n");
        return 0;
    }
    ifs_[IFS_LEFT].ifindex = lif;

    if ((!right_if_name) || (rif == (unsigned int) -1)) {
        EPRINT("\"--right\" not specified.\n");
        return 0;
    }
    ifs_[IFS_RIGHT].ifindex = rif;

    if (ifs_[IFS_LEFT].dstaddrtype == DSTA_INVALID) {
        EPRINT("\"--left-dst\" is a mandatory argument.\n");
        return 0;
    }

    if (ifs_[IFS_LEFT].srcaddrtype == SRCA_INVALID) {
        EPRINT("\"--left-src\" is a mandatory argument.\n");
        return 0;
    }

    if (ifs_[IFS_RIGHT].dstaddrtype == DSTA_INVALID) {
        EPRINT("\"--right-dst\" is a mandatory argument.\n");
        return 0;
    }

    if (ifs_[IFS_RIGHT].srcaddrtype == SRCA_INVALID) {
        EPRINT("\"--right-src\" is a mandatory argument.\n");
        return 0;
    }

    if ((ifs_[IFS_LEFT].srcaddrtype == SRCA_UNCHANGED) ||
	(ifs_[IFS_RIGHT].srcaddrtype == SRCA_UNCHANGED)) {
	if (echo_marker_ttl_ == 0) {
	    EPRINT("\"--echo-marker\" is needed when either \"--left-src\" or "
		   "\"--right-dst\" is specified as \"unchanged\"\n");
	    return 0;
	}
    } else {
	if (echo_marker_ttl_ != 0) {
	    printf("Warning: \"--echo-marker\" value set on the command-line is "
		   "ignored because neither \"--left-src\" nor \"--right-dst\" is "
		   "specified as \"unchanged\"\n");
	}
    }

    /* Create a temp raw socket for doing ioctls */
    fd_socket_tmp = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (fd_socket_tmp == -1) {
        EPRINT("Error creating temp raw socket: %s\n", strerror(errno));
        return 0;
    }

    for (i = 0; i < MAXIFS; i++) {
        struct Iface *thisif = &(ifs_[i]);
        char *this_if_name;
        unsigned short flags;
        char display[INET_ADDRSTRLEN + 1];
        int mtu;

        if (i == IFS_LEFT) {
            this_if_name = left_if_name;
        } else {
            this_if_name = right_if_name;
        }

        /* Get the flags for this interface */
	if (!fetch_if_flags(fd_socket_tmp, this_if_name, &flags)) {
            close(fd_socket_tmp);
            return 0;
        }

        /* If the interface is not up or is a loopback, error out */
        if ((flags & IFF_LOOPBACK) != 0) {
            EPRINT("Loopback interface %s is not supported\n", this_if_name);
            close(fd_socket_tmp);
            return 0;
        }
        if ((flags & IFF_UP) == 0) {
            EPRINT("Interface %s is not up\n", this_if_name);
            close(fd_socket_tmp);
            return 0;
        }

        if (thisif->dstaddrtype == DSTA_BROADCAST) {
	    /* If the interface is broadcast-capable, get the broadcast address.
	       If its a point-to-point network, get the peer address */
	    if (flags & IFF_BROADCAST) {
		if (!fetch_bcast_address(fd_socket_tmp, this_if_name,
					 &thisif->dstaddr)) {
		    close(fd_socket_tmp);
		    return 0;
		}

		/* The SIOCGIFBRDADDR ioctl returns 0.0.0.0 when there is no broadcast
		   address explicitly set on the interface. In this situation we
		   calculate the broadcast address from the interface address and
		   netmask */
		if (thisif->dstaddr.s_addr == 0) {
		    struct in_addr if_addr = {0};
		    struct in_addr if_netmask = {0};
		    if (!fetch_if_address(fd_socket_tmp, this_if_name, &if_addr)) {
			close(fd_socket_tmp);
			return 0;
		    }
		    if (!fetch_if_netmask(fd_socket_tmp, this_if_name, &if_netmask)) {
			close(fd_socket_tmp);
			return 0;
		    }
		    thisif->dstaddr.s_addr = if_addr.s_addr | ~(if_netmask.s_addr);
		}
            } else {
		if (!fetch_dest_address(fd_socket_tmp, this_if_name,
					&thisif->dstaddr)) {
		    close(fd_socket_tmp);
		    return 0;
		}
            }
            /* Error out if we got 0.0.0.0 */
            if (thisif->dstaddr.s_addr == 0) {
                EPRINT("Could not determine the destination address for %s; "
		       "try specifying it explicitly.\n", this_if_name);
                close(fd_socket_tmp);
                return 0;
            }
        }

        if (thisif->srcaddrtype == SRCA_IFADDR) {
	    /* Get local IP for interface */
	    if (!fetch_if_address(fd_socket_tmp, this_if_name, &thisif->srcaddr)) {
                close(fd_socket_tmp);
                return 0;
	    }
        }

        /* Get the largest MTU of the two interfaces */
	if (!fetch_if_mtu(fd_socket_tmp, this_if_name, &mtu)) {
            close(fd_socket_tmp);
            return 0;
        }
        if (mtu == 0) {
            mtu = 4096;
        }
        if (mtu > largest_mtu_) {
            largest_mtu_ = mtu;
        }

        /* Display everything we've gleaned until this point */
        printf("%s: index %u ", this_if_name, thisif->ifindex);

        inet_ntop(AF_INET, &(thisif->srcaddr), display, INET_ADDRSTRLEN);
        display[INET_ADDRSTRLEN] = '\0';

        switch (thisif->srcaddrtype) {
            case SRCA_UNCHANGED: printf("src (unchanged) "); break;
            case SRCA_SPECIFIED: printf("src %s (specified) ", display); break;
            case SRCA_IFADDR: printf("src %s (ifaddr) ", display); break;
            default: printf("src (error: %d) ", thisif->srcaddrtype); break;
        }

        inet_ntop(AF_INET, &(thisif->dstaddr), display, INET_ADDRSTRLEN);
        display[INET_ADDRSTRLEN] = '\0';

        switch (thisif->dstaddrtype) {
            case DSTA_BROADCAST: printf("dst %s (broadcast)\n", display); break;
            case DSTA_SPECIFIED: printf("dst %s (specified)\n", display); break;
            default: printf("dst (error: %d)\n", thisif->dstaddrtype); break;
        }
    }

    if (fd_socket_tmp != -1) {
        close(fd_socket_tmp);
    }

    return 1;
}

static int setup_raw_socket(struct Iface *thisif) {
    char ifname[IF_NAMESIZE + 1];
    int yes = 1;
    int no = 0;

    /* Get the interface's name for later use as well as for debug purposes */
    ifname[IF_NAMESIZE] = '\0';
    if (!if_indextoname(thisif->ifindex, ifname)) {
        EPRINT("Failed to lookup interface name from %u", thisif->ifindex);
        return 0;
    }

    if ((thisif->raw_socket = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) < 0) {
        EPRINT("Error creating raw socket on %s: %s\n", ifname, strerror(errno));
        return 0;
    }

    yes = 1;
    if (setsockopt(thisif->raw_socket, SOL_SOCKET, SO_BROADCAST, &yes,
                   sizeof(yes)) < 0) {
        EPRINT("Error setting SO_BROADCAST on %s: %s\n", ifname, strerror(errno));
        return 0;
    }

    no = 1;
    if (setsockopt(thisif->raw_socket, IPPROTO_IP, IP_HDRINCL, &no, sizeof(no)) < 0) {
        EPRINT("Error setting IP_HDRINCL on %s: %s\n", ifname, strerror(errno));
        return 0;
    }

    yes = 1;
    if (setsockopt(thisif->raw_socket, SOL_SOCKET, SO_REUSEPORT, &yes,
                   sizeof(yes)) < 0) {
        EPRINT("Error setting SO_REUSEPORT on %s: %s\n", ifname, strerror(errno));
        return 0;
    }

    // bind socket to dedicated NIC
    if (setsockopt(thisif->raw_socket, SOL_SOCKET, SO_BINDTODEVICE, ifname,
                   strlen(ifname) + 1) < 0) {
        EPRINT("Error setting SO_BINDTODEVICE on %s: %s\n", ifname, strerror(errno));
        return 0;
    }

    return 1;
}

static int setup_udp_socket(unsigned short port) {
    int fd_socket;
    struct sockaddr_in bind_addr;
    int yes = 1;

    if ((fd_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        EPRINT("Failed to create UDP socket: %s\n", strerror(errno));
        return -1;
    }

    yes = 1;
    if (setsockopt(fd_socket, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0) {
        EPRINT("Failed to set SO_BROADCAST on UDP socket: %s\n", strerror(errno));
        return -1;
    }

    yes = 1;
    if (setsockopt(fd_socket, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) < 0) {
        EPRINT("Failed to set SO_REUSEPORT on UDP socket: %s\n", strerror(errno));
        return -1;
    }

    yes = 1;
    if (setsockopt(fd_socket, SOL_IP, IP_PKTINFO, &yes, sizeof(yes)) < 0) {
        EPRINT("Failed to set IP_PKTINFO on UDP socket: %s\n", strerror(errno));
        return -1;
    }

    yes = 1;
    if (setsockopt(fd_socket, SOL_IP, IP_RECVTTL, &yes, sizeof(yes)) < 0) {
        EPRINT("Failed to set IP_RECVTTL on UDP socket: %s\n", strerror(errno));
        return -1;
    }

    yes = 1;
    if (setsockopt(fd_socket, SOL_IP, IP_RECVORIGDSTADDR, &yes, sizeof(yes)) < 0) {
        EPRINT("Failed to set IP_RECVORIGDSTADDR on UDP socket: %s\n",
	       strerror(errno));
        return -1;
    }

    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if(bind(fd_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        EPRINT("Failed to bind UDP socket to port %u: %s\n", (unsigned) port,
	       strerror(errno));
        return -1;
    }

    return fd_socket;
 }

int main(int argc,char **argv) {
    unsigned int i, j;
    unsigned char *buf;
    char ipstr[INET_ADDRSTRLEN + 1];
    char ifname[IF_NAMESIZE + 1];
    int fd_udp_socket;

    openlog("ubrr", LOG_PID | LOG_CONS, LOG_LOCAL1);
    if (!parse_command_line(argc, argv)) {
	closelog();
        exit(1);
    }
    if (debug_ == 0) {
	setlogmask(LOG_UPTO (LOG_INFO));
    }

    for (i = 0; i < MAXIFS; i++) {
        if (!setup_raw_socket(&(ifs_[i]))) {
            for (j = 0; j < i; j++) {
                close(ifs_[j].raw_socket);
            }
	    closelog();
            exit(1);
        }
    }

    /* Create our broadcast receiving socket */
    if ((fd_udp_socket = setup_udp_socket(udport_)) == -1) {
        for (i = 0; i < MAXIFS; i++) {
            close(ifs_[i].raw_socket);
        }
	closelog();
        exit(1);
    }

    printf("Largest MTU: %d\n", largest_mtu_);

    /* Create the buffer that will hold the packet content */
    largest_mtu_ += 32; /* add some extra room just in case */
    /* Add room for the IP and UDP headers */
    largest_mtu_ += sizeof(struct iphdr) + sizeof(struct udphdr);

    buf = malloc(largest_mtu_);
    if (!buf) {
        EPRINT("Failed to create %d-byte packet buffer\n", largest_mtu_);
        for (i = 0; i < MAXIFS; i++) {
            close(ifs_[i].raw_socket);
        }
	closelog();
        exit(1);
    }

    /* Fork to background */

    if (fork_ && fork()) {
	exit(0);
    }
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);
    forked_ = 1;

    for (;;) /* endless loop */
    {
        struct sockaddr_in rcv_addr, snd_addr;
        struct msghdr rcv_msg;
        struct iovec iov;
        struct in_pktinfo rcv_pkt_info;
        struct sockaddr_in rcv_dst_addr;
	unsigned long rcv_pkt_ttl;
        struct Iface *txiface, *rxiface;
        struct cmsghdr *cmsg;
        struct iphdr *ip;
        struct udphdr *udp;

        ssize_t rcv_msg_len;
        u_char pkt_infos[CMSG_SPACE(sizeof(struct in_pktinfo)) +
			 CMSG_SPACE(4) +
			 CMSG_SPACE(sizeof(struct sockaddr_in))];

        /* The IPv4 header goes at the beginning of `buf`*/
        ip = (struct iphdr *) buf;
        /* The UDP header follows */
        udp = (struct udphdr *) (buf + sizeof(*ip));

        /* The received UDP datagram follows the UDP header */
        iov.iov_base = buf + sizeof(*ip) + sizeof(*udp);
        iov.iov_len = largest_mtu_ - (sizeof(*ip) + sizeof(*udp));

        rcv_msg.msg_name = &rcv_addr;
        rcv_msg.msg_namelen = sizeof(rcv_addr);
        rcv_msg.msg_iov = &iov;
        rcv_msg.msg_iovlen = 1;
        rcv_msg.msg_control = pkt_infos;
        rcv_msg.msg_controllen = sizeof(pkt_infos);

        rcv_msg_len = recvmsg(fd_udp_socket, &rcv_msg, 0);
        if (rcv_msg_len <= 0) {
            DPRINT("recvmsg() returned %d, ignoring this packet\n", (int) rcv_msg_len);
            continue;    /* ignore broken packets */
        }

        ipstr[INET_ADDRSTRLEN] = '\0';
        DPRINT("Received %ld bytes of data from %s:%u\n", rcv_msg_len,
               inet_ntop(AF_INET, &(rcv_addr.sin_addr), ipstr, INET_ADDRSTRLEN),
               (unsigned int) ntohs(rcv_addr.sin_port));

        /* We cannot proceed without the ancillary data */
        if (rcv_msg.msg_controllen == 0) {
            DPRINT("rcv_msg.msg_controllen == 0\n");
            continue;
        }

        memset(&rcv_pkt_info, 0, sizeof(rcv_pkt_info));
        memset(&rcv_dst_addr, 0, sizeof(rcv_dst_addr));

        for (cmsg = CMSG_FIRSTHDR(&rcv_msg); cmsg;
             cmsg = CMSG_NXTHDR(&rcv_msg, cmsg)) {
            if (cmsg->cmsg_level != IPPROTO_IP) {
                DPRINT("In ancillary data, unsupported level %u\n",
                       (unsigned) cmsg->cmsg_level);
                continue;
            }
            if (cmsg->cmsg_type == IP_PKTINFO) {
                DPRINT("IP_PKTINFO present in ancillary data\n");
                memcpy(&rcv_pkt_info, CMSG_DATA(cmsg), sizeof(struct in_pktinfo));
                DPRINT("IP_PKTINFO ipi_spec_dst = %s ipi_addr = %s\n",
                       inet_ntop(AF_INET, &(rcv_pkt_info.ipi_spec_dst), ipstr,
				 INET_ADDRSTRLEN),
                       inet_ntop(AF_INET, &(rcv_pkt_info.ipi_addr), ipstr,
				 INET_ADDRSTRLEN));
            } else if (cmsg->cmsg_type == IP_ORIGDSTADDR) {
                DPRINT("IP_ORIGDSTADDR present in ancillary data\n");
                memcpy(&rcv_dst_addr, CMSG_DATA(cmsg), sizeof(rcv_dst_addr));
                ipstr[INET_ADDRSTRLEN] = '\0';
                DPRINT("IP_ORIGDSTADDR is %s\n",
                       inet_ntop(AF_INET, &(rcv_dst_addr.sin_addr), ipstr,
                                 INET_ADDRSTRLEN));
	    } else if (cmsg->cmsg_type == IP_TTL) {
		rcv_pkt_ttl = 0ul;
                DPRINT("IP_TTL present in ancillary data\n");
		memcpy(&rcv_pkt_ttl, CMSG_DATA(cmsg), 4);
		DPRINT("IP_TTL value is %lu\n", rcv_pkt_ttl);
            } else {
                DPRINT("Unasked cmsg type %u encountered\n", cmsg->cmsg_type);
            }
        }

        txiface = 0;
	rxiface = 0;
        if (rcv_pkt_info.ipi_ifindex == ifs_[IFS_LEFT].ifindex) {
            DPRINT("Packet arrived on left\n");
	    rxiface = &(ifs_[IFS_LEFT]);
            txiface = &(ifs_[IFS_RIGHT]);
        } else if (rcv_pkt_info.ipi_ifindex == ifs_[IFS_RIGHT].ifindex) {
            DPRINT("Packet arrived on right\n");
	    rxiface = &(ifs_[IFS_RIGHT]);
            txiface = &(ifs_[IFS_LEFT]);
        } else {
            ifname[IF_NAMESIZE] = '\0';
            if (!if_indextoname(rcv_pkt_info.ipi_ifindex, ifname)) {
                strcpy(ifname, "<???>");
            }
            DPRINT("Packet arrived on uninteresting network interface %s\n", ifname);
            continue;
        }

	/* Echo check. If the srcaddrtype on the rx interface is SRCA_SPECIFIED or
	   SRCA_IFADDR, and if the source address on the packet is the srcaddr of the
	   rx interface, then this must be a packet that we transmitted earlier (we're
	   receiving it because it's a broadcast), and we should not forward it to the
	   tx interface. If the srcaddrtype on the rx interface is SRCA_UNCHANGED, we
	   cannot rely on the source address on the packet, so we have to rely on
	   the "magic" echo marker TTL that we set on all transmitted packets. */
	if (rxiface->srcaddrtype == SRCA_UNCHANGED) {
	    if ((unsigned char) rcv_pkt_ttl == echo_marker_ttl_) {
		DPRINT("Echo (TTL matches echo marker): not forwarding\n");
		continue;
	    }
	} else if (rcv_addr.sin_addr.s_addr == rxiface->srcaddr.s_addr) {
	    DPRINT("Echo (Source IP address is ours): not forwarding\n");
	    DPRINT("(ttl is %lu)\n", rcv_pkt_ttl);
	    continue;
	}

	DPRINT("Forwarding\n");

        /* Manufacture the IP header. `ip` is pointing to the beginning of buf */
        ip->version = 4;
        ip->ihl = 5;
        ip->tos = 0;
        ip->tot_len = 0; /* Kernel will fill this */
        ip->id = 0;  /* Kernel will fill this */
        ip->frag_off = 0;
        ip->ttl = (echo_marker_ttl_ == 0) ? 64 : (unsigned char) echo_marker_ttl_;
        ip->protocol = 17;
        ip->check = 0; /* Kernel will fill this */
        if (txiface->srcaddrtype == SRCA_UNCHANGED) {
            ip->saddr = rcv_addr.sin_addr.s_addr;
        } else {
            ip->saddr = txiface->srcaddr.s_addr;
        }
        ip->daddr = txiface->dstaddr.s_addr;

        /* Manufacture the UDP header.
           `udp` is pointing to the offset in `buf` after the IP header */
        udp->source = rcv_addr.sin_port;
        udp->dest = htons(udport_);
        udp->len = htons((unsigned short) (rcv_msg_len + sizeof(*udp)));
        udp->check = 0;

        /* Compute and fill in the UDP checksum */
        udp->check = htons(udp_csum(ip, udp, iov.iov_base, rcv_msg_len));

        snd_addr.sin_family = AF_INET;
        snd_addr.sin_port = htons(udport_);
        snd_addr.sin_addr.s_addr = ip->daddr;

        if (sendto(txiface->raw_socket, buf, rcv_msg_len + sizeof(*ip) + sizeof(*udp),
                   0, (struct sockaddr *) &snd_addr, sizeof(snd_addr)) < 0) {
            EPRINT("Failed to transmit: %s\n", strerror(errno));
        }
    }
}
