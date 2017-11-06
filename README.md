# Copyright Notice

 This file is part of the *fpga_prog* linux kernel module.
 It is subject to the license terms in the LICENSE.txt
 file found in the top-level directory of this distribution and at
 https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.

 No part of the software, including this file, may be copied, modified,
 propagated, or distributed except according to the terms contained in
 the LICENSE.txt file.

# What is fpga_prog

 A simple 'glue' driver to use the linux FPGA-manager without device-tree
 overlays etc. Just write the path/name of your firmware file to a sysfs
 property and have it loaded to the FPGA.

# Use Cases

 This driver can be used in two ways:

   1. together with device-tree definition
   2. w/o device-tree modification

 --------

## 1. Device-tree

 In the first use case a device-tree entry must be created (at the top-level, i.e.,
 the 'platform bus' level):

    prog_fpga0: prog-fpga0 {
      compatible = "tills,fpga-programmer-1.0";
      fpga-mgr   = <&devcfg>;     # reference must point to the fpga controller (e.g., zynq devcfg)
      file       = "zzz.bin";     # optional firmware file name (must be present in one of the directories
                                  # automatically searched by the kernel or in a path as given in
                                  # /sys/module/firmware_class/parameters/path)
      autoload   = 1;             # when autoload is nonzero then firmware is loaded when the driver
                                  # is bound or whenever the `file` property is written in sysfs
                                  # (see below). If autoload is `0` then you must explicitly write
                                  # to the `program` property in sysfs (see below).
    };


 When the driver is bound then it will add a few sysfs properties to the device

    /sys/bus/platform/devices/prog-fpga0/

    file:     name of firmware file; if `autoload` is nonzero then writing a filename
              to `file` triggers programming.

    autoload: whether binding the driver or writing `file` triggers programming

    program:  writing nonzero here triggers programming (required if autoload is zero)

 The device-tree use-case allows to automatically load a default firmware file during
 boot-up.

## 2. No device-tree

 This driver can also be used without a modified device tree. In this case, the user must
 instruct the driver to create 'soft' devices which replace the auto-generated ones that
 the kernel builds when processing the device tree.

 You can simply create a soft device by writing the device-tree path to the controller
 to the driver's `add_programmer` property.
 
 Note that the controller itself still must be defined in the device tree. E.g., for
 zynq we have (on amba bus):

    amba: amba {
 
      ...

      devcfg: devcfg@f8007000 {
          compatible = "xlnx,zynq-devcfg-1.0";
          reg = <0xf8007000 0x100>;
          interrupt-parent = <&intc>;
          interrupts = <0 8 4>;
          clocks = <&clkc 12>;
          clock-names = "ref_clk";
          syscon = <&slcr>;
      };
    };
  
 Thus the device-tree path to the controller is `/amba/devcfg@f8007000` and

    echo -n '/amba/devcfg@f8007000' > /sys/bus/platform/driver/fpga_programmer/add_programmer

 generates a new device (note that the naming is slightly different than in use case 1.)

    /sys/bus/platform/devices/prog-fpga.0/

 which is bound to the driver and features the same `file`, `autoload` etc. properties.

 Soft devices can be removed (write nonzero to `remove`).

## PROGRAMMING (identical for use case 1. and 2.):

  E.g.:

      echo -n '/' > /sys/module/firmware_class/parameters/path

      echo -n '/mnt/somewhere/something.bin' > /sys/bus/platform/devices/prog-fpga.0/file
