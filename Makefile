obj-m := tcp_apledbat.o tcp_ledbat.o tcp_nice.o tcp_westwoodlp.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

.PHONY: install
install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: clean
clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean
