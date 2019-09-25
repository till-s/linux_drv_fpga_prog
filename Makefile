-include $(M)../config.mk

# if there is no ../config.mk define KERNELDIR here:
#
#KERNELDIR:=<abs_path_to_kernel_sources>
#

INSTALL_MOD_PATH=$(KERNELDIR)/../../target/

obj-m:=fpga_prog.o

CROSS_COMPILE=arm-linux-

ARCHOPT=ARCH=arm

all:
	make $(ARCHOPT) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNELDIR) M=$(PWD)/ modules

install modules_install:
	make $(ARCHOPT) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNELDIR) M=$(PWD)/ INSTALL_MOD_PATH=$(INSTALL_MOD_PATH) modules_install

clean:
	make $(ARCHOPT) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNELDIR) M=$(PWD)/ clean
