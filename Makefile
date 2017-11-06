
KERNELDIR:=$(shell pwd)/../../buildroot-2017.08-zynq/output/build/linux-4.13.10

obj-m:=fpga_prog.o

CROSS_COMPILE=arm-linux-

ARCHOPT=ARCH=arm

all:
	make $(ARCHOPT) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNELDIR) M=$(PWD) modules

