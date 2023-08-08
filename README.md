# UDP Broadcast Relay for Linux

This is a fork of [udp-redux/udp-broadcast-relay-redux](https://github.com/udp-redux/udp-broadcast-relay-redux).

This program listens for packets on a specified UDP broadcast port on two network interfaces (labelled **left** and **right**). When a packet is received on one of the two interfaces, this program transmits that packet over the other interface, optionally overwriting the source and/or destination IP addresses in the process.

The primary purpose of this is to allow devices or game servers on separated local networks (Ethernet, WLAN, VLAN) that use udp broadcasts to find each other to do so.

## Usage

```

./udp-broadcast-relay-redux --port <udp port> --echo-marker <1-255> --left <interface> --right <interface> --left-src <arg> --left-dest <arg> --right-src <arg> --right-dest <arg> [--debug] [--fork]

```

## Command line arguments

| Argument                | Meaning                                                                                                                                           |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--port <1-65535>`      | Mandatory. The UDP port to process.                                                                                                               |
| `--left <name>`         | Mandatory. The name of the *left* network interface.                                                                                              |
| `--right <name>`        | Mandatory. The name of the *right* network interface.                                                                                             |
| `--left-src <arg>`      | Mandatory. The source IP address that is set on packets that arrive on right and are forwarded to left. `<arg>` can have the following values:    |
|                         |  `unchanged` : The original source IP address from the received packet is retained in the transmitted packet.                                     |
|                         |  `ifaddr`    : The source IP address in the transmitted packet is set to the IP address of the *left* interface.                                  |
|                         |  x.x.x.x     : The source IP address in the transmitted packet is set to the specified IP address.                                                |
| `--left-dst <arg>`      | Mandatory. The destination IP address that is set on packets that arrive on right and are forwarded to left. <arg> can have the following values: |
|                         |  `broadcast` : The destination IP address on the transmitted packet is set to the broadcast address of the *left* interface.                      |
|                         |  x.x.x.x     : The destination IP address on the transmitted packet is set to the specified IP address.                                           |
| `--right-src <arg>`     | Same as the `--left-src` argument, but for packets received on *left* and forwarded to *right*                                                    |
| `--right-dst <arg>`     | Same as the `--left-dst` argument, but for packets received on *left* and forwarded to *right*                                                    |
| `--echo-marker <1-255>` | Mandatory if either `--left-src` or `--right-src` is set to `unchanged`. This value is set as the TTL in the IP header of transmitted packets, to enable the application to identify "echos", i.e.broadcast packets sent by the application and received on account of being broadcasts.               |
| `--debug`               | Print debug messages on stderr or syslog                                                                                      |
| `--fork`                | Fork to the background just before starting the packet processing operation                                                   |


## Differences from [udp-redux/udp-broadcast-relay-redux](https://github.com/udp-redux/udp-broadcast-relay-redux)

* Only two interfaces, labelled *left* and *right*
* Multicast support removed
* Linux only. Removed code specific to FreeBSD and MacOS
* Explicit command line keywords (`unchanged`, `ifaddr`, `broadcast`) to indicate IP address rewrite rules
* Syslog support
* More conservative use of memory (e.g. use MTU to size packet buffer)
* Transmitted datagrams have checksums recalculated and set in the UDP header
* Extensive code refactoring
