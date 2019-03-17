#!/bin/sh

IDPRODUCT=0x4750

mkdir /sys/kernel/config/usb_gadget/c1
mkdir /sys/kernel/config/usb_gadget/c1/strings/0x409
mkdir /sys/kernel/config/usb_gadget/c1/configs/c.1
mkdir /sys/kernel/config/usb_gadget/c1/functions/ffs.ffs

echo 0x601a > /sys/kernel/config/usb_gadget/c1/idVendor
echo $IDPRODUCT > /sys/kernel/config/usb_gadget/c1/idProduct
echo "Ingenic" > /sys/kernel/config/usb_gadget/c1/strings/0x409/manufacturer
echo "odbootd" > /sys/kernel/config/usb_gadget/c1/strings/0x409/product
ln -s /sys/kernel/config/usb_gadget/c1/functions/ffs.ffs /sys/kernel/config/usb_gadget/c1/configs/c.1/ffs.ffs

mkdir /dev/ffs
mount ffs -t functionfs /dev/ffs
