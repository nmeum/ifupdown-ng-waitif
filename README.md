## Status

This is a small proof of concept experiment for integrating [ifupdown-ng][ifupdown-ng github] with [netlink][netlink wikipedia] link state detection via [libmnl][libmnl web].
The implementation is quite hacky but works well for my own ifupdown-ng setup so far.

## Background

[ifupdown-ng][ifupdown-ng github] is a re-implementation of the `ifup` and `ifdown` commands provided by Debian and BusyBox.
These commands configure network interfaces according to the `/etc/network/interfaces` configuration file.
Interface configuration is split into multiple [phases][ifupdown-ng phases].
For each phase, different [executors][ifupdown-ng executor] are run.
For example, a DHCP client may be run as an executor in the `up` phase.
Unfortunately, ifupdown-ng doesn't check the interface link state before running executors of the `up` phase.
As such, executors which require the link to be up (e.g. the DHCP executor) usually retry requests in a configured interval until the requests succeed.
As it turns out, choosing an appropriate interval is [difficult][alpine #13795].
Inspired by [bncm-waitif][bncm-waitif web], this repository provides an executor which blocks until the interface state is changed to running by the Linux [netlink subsystem][netlink wikipedia].

## Installation

In order to build this software, install the following dependencies:

* [libmnl][libmnl web]
* [GNU make][make web]

Afterwards, run the following commands:

	$ make
	# make install

Refer to the `GNUmakefile` regarding available configuration variables.

## Usage

In order to use this executor, add a `use waitif` declaration to your interface configuration in `/etc/network/interfaces`.
For example:

	iface wlan0
		use waitif
		use dhcp

In this case, the `waitif` executor will block in the `up` phase until the interface switched to the `IFF_RUNNING` state.
Afterwards, the `dhcp` executor is run and it will hopefully be able to retrieve a lease immediately.
By default, the `waitif` executor will wait for up to 30 seconds for the interface to come up.
If the interface doesn't change to `IFF_RUNNING` within this timespan, then the `waitif` executor exits with `EXIT_FAILURE`.
It is possible to configure a different timeout value using `waitif-timeout`, for example:

	iface wlan0
		waitif-timeout 10
		use waitif
		use dhcp

The `waitif-timeout` keyword specifies a timeout in seconds, the timeout can be set to zero to wait indefinitely.

## Caveats

ifupdown-ng currently does not stop executing executors of a given phase if one of them fails.
As such, if the `waitif` up executor fails (e.g. due to a timeout) ifupdown-ng will still attempt to retrieve a DCHP lease.
See: [ifupdown-ng#179][ifupdown-ng #179].

## License

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <http://www.gnu.org/licenses/>.

[ifupdown-ng github]: https://github.com/ifupdown-ng/ifupdown-ng
[ifupdown-ng executor]: https://github.com/ifupdown-ng/ifupdown-ng/blob/ce6954d62879c3532697a5b0cd800a506fae9da6/doc/ifupdown-executor.scd
[ifupdown-ng phases]: https://github.com/ifupdown-ng/ifupdown-ng/blob/ce6954d62879c3532697a5b0cd800a506fae9da6/doc/ifupdown-executor.scd#phases
[alpine #13795]: https://gitlab.alpinelinux.org/alpine/aports/-/issues/13795
[bncm-waitif web]: https://skarnet.org/software/bcnm/bcnm-waitif.html
[netlink wikipedia]: https://en.wikipedia.org/wiki/Netlink
[libmnl web]: https://netfilter.org/projects/libmnl/
[make web]: https://github.com/rocky/remake
[ifupdown-ng #179]: https://github.com/ifupdown-ng/ifupdown-ng/pull/179
