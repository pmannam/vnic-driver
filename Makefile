obj-m += vnic.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod vnic.ko

unload:
	sudo rmmod vnic

reload: unload load

.PHONY: all clean load unload reload
