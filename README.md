# Less-than-Best-Effort TCP congestion control
A collection of Less-than-Best-Effort TCP congestion control modules for Linux. These mechanisms have been implemented based on code snippets and published descriptions of algorithms.

Included algorithms:
* [LEDBAT](https://tools.ietf.org/html/rfc6817)
* [Nice](https://people.cs.umass.edu/~arun/papers/tcp-nice-osdi.pdf)
* [Westwood Low Priority](https://www.researchgate.net/profile/MY_Sanadidi/publication/31398835_TCP-Westwood_Low-Priority_for_overlay_QoS_mechanism/links/00b4951cfaccac9e86000000.pdf)
* [Apple LEDBAT](https://opensource.apple.com//source/xnu/xnu-1699.32.7/bsd/netinet/tcp_ledbat.c)

Note that these modules have only been tested with Linux 4.4.15 and are not guaranteed to work with other versions.

## Instructions
The Makefile contains the necessary rule to compile all modules against the running kernel:
> make

Move the resulting modules to the library and update dependencies (root access will be required):
> mv *.ko /lib/modules/$(shell uname -r)/kernel/net/ipv4/ \
> depmod -a

Once copied, the modules can be loaded and selected like any other TCP congestion control modules using:
> modprobe tcp_[name] \
> sysctl net.ipv4.tcp_congestion_control=[name]

In this case, the module names always correspond to the names of the source files.