DRIVER = ecspi

ifneq ($(KERNELRELEASE),)
    obj-m := $(DRIVER).o
else
    PWD := $(shell pwd)

default:
ifeq ($(strip $(KERNELDIR)),)
	$(error "KERNELDIR is undefined!")
else
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules 
endif


install:
	scp ecspi.ko root@192.168.10.50:/home/root


clean:
	rm -rf *~ *.ko *.o *.mod.c modules.order Module.symvers .$(DRIVER)* .tmp_versions

endif

