#!/bin/sh
# SPDX-License-Identifier: MIT

set -e

CONFIGFS="/sys/kernel/config"
GADGET="$CONFIGFS/usb_gadget"
VID="0x1d6b"
PID="0x0104"
SERIAL="eacfba9876545332"
MANUF="Pawkow"
PRODUCT="ASSoundCM5"
CONFIGURATION="UAC2"
MAXPOWERMW=250

BOARD=$(strings /proc/device-tree/model)

case $BOARD in
	"Renesas Salvator-X board based on r8a7795 ES1.x")
		UDC_USB2=e6590000.usb
		UDC_USB3=ee020000.usb

		UDC_ROLE2=/sys/devices/platform/soc/ee080200.usb-phy/role
		UDC_ROLE2=/dev/null #Not needed - always peripheral
		UDC_ROLE3=/sys/devices/platform/soc/ee020000.usb/role

		UDC=$UDC_USB2
		UDC_ROLE=$UDC_ROLE2
		;;

	"TI OMAP4 PandaBoard-ES")
		UDC=`ls /sys/class/udc` # Should be musb-hdrc.0.auto
		UDC_ROLE=/dev/null # Not needed - peripheral enabled
		;;

	*)
		UDC=`ls /sys/class/udc` # will identify the 'first' UDC
		UDC_ROLE=/dev/null # Not generic
		;;
esac

echo "Detecting platform:"
echo "  board : $BOARD"
echo "  udc   : $UDC"

create_uac2() {
	# Example usage:
	#       create_uac <target config> <function name>
	#       create_uac config/c.1 uac.0
	CONFIG=$1
	FUNCTION=$2
	uac2name="$3"
	uac2alsadevname="$4"
	uac2alsadevpcmname="$5"

	# UAC2 audio
	# https://www.kernel.org/doc/html/latest/usb/gadget-testing.html#uac2-function
	pwd
	echo "	Creating UAC gadget functionality : $FUNCTION"
	mkdir -p functions/$FUNCTION
	echo 48000 > functions/$FUNCTION/c_srate
	echo 48000 > functions/$FUNCTION/p_srate
	#echo 96000 > functions/$FUNCTION/c_srate
	#echo 96000 > functions/$FUNCTION/p_srate
	echo "$uac2name" > functions/$FUNCTION/function_name
	#echo "$uac2alsadevname" > functions/$FUNCTION/local_dev_name
	#echo "$uac2alsadevpcmname" > functions/$FUNCTION/local_dev_pcm_name
	echo 192 > functions/$FUNCTION/c_chmask
	echo 192 > functions/$FUNCTION/p_chmask
	#echo 1779 > functions/$FUNCTION/c_chmask
	#echo 1779 > functions/$FUNCTION/p_chmask
	#echo 504 > functions/$FUNCTION/p_chmask
	#echo 2 > functions/$FUNCTION/c_ssize
	#echo 2 > functions/$FUNCTION/p_ssize
	#echo 3 > functions/$FUNCTION/c_ssize
	#echo 3 > functions/$FUNCTION/p_ssize
	echo 4 > functions/$FUNCTION/c_ssize
	echo 4 > functions/$FUNCTION/p_ssize
	ln -s functions/$FUNCTION $CONFIG
	echo "\tOK"
}

create_uac2_2ch() {
	# Example usage:
	#       create_uac <target config> <function name>
	#       create_uac config/c.1 uac.0
	CONFIG=$1
	FUNCTION=$2
	uac2name="$3"
	uac2alsadevname="$4"
	uac2alsadevpcmname="$5"

	# UAC2 audio
	# https://www.kernel.org/doc/html/latest/usb/gadget-testing.html#uac2-function
	pwd
	echo "	Creating UAC gadget functionality : $FUNCTION"
	mkdir -p functions/$FUNCTION
	echo 48000 > functions/$FUNCTION/c_srate
	echo 48000 > functions/$FUNCTION/p_srate
	echo "$uac2name" > functions/$FUNCTION/function_name
	echo "$uac2alsadevname" > functions/$FUNCTION/local_dev_name
	echo "$uac2alsadevpcmname" > functions/$FUNCTION/local_dev_pcm_name
	echo 192 > functions/$FUNCTION/c_chmask
	echo 192 > functions/$FUNCTION/p_chmask
	echo 2 > functions/$FUNCTION/c_ssize
	echo 2 > functions/$FUNCTION/p_ssize
	ln -s functions/$FUNCTION $CONFIG
	echo "\tOK"
}

delete_uac2() {
	# Example usage:
	#	delete_uac <target config> <function name>
	#	delete_uac config/c.1 uac.0
	CONFIG=$1
	FUNCTION=$2

	echo "Removing UAC audio interface : $FUNCTION"
	rm -f $CONFIG/$FUNCTION
	rmdir functions/$FUNCTION
	echo "OK"
}

case "$1" in
    start)
	echo "Creating the USB gadget"
	echo "Loading composite module"
	modprobe libcomposite

	echo "Creating gadget directory g_multi.0"
	mkdir -p $GADGET/g_multi.0

	cd $GADGET/g_multi.0
	if [ $? -ne 0 ]; then
	    echo "Error creating usb gadget in configfs"
	    exit 1;
	else
	    echo "OK"
	fi

	echo "Setting Vendor and Product ID's"
	echo $VID > idVendor
	echo $PID > idProduct
	echo "OK"

	echo "Setting English strings"
	mkdir -p strings/0x409
	echo $SERIAL > strings/0x409/serialnumber
	echo $MANUF > strings/0x409/manufacturer
	echo $PRODUCT > strings/0x409/product
	echo 0x0100 > bcdDevice
	#echo 0xEF > bDeviceClass
	#echo 0x02 > bDeviceSubClass
	#echo 0x01 > bDeviceProtocol
	echo 0x200 > bcdUSB
	echo "OK"

	echo "Creating Config"
	mkdir configs/c.1
	mkdir configs/c.1/strings/0x409
	echo $MAXPOWERMW > configs/c.1/MaxPower
	echo $CONFIGURATION > configs/c.1/strings/0x409/configuration

	echo "Creating functions..."
	create_uac2 configs/c.1 uac2.usb0 "ASSoundCM5" "ASSoundCM5" "ASSoundCM5"
	create_uac2 configs/c.1 uac2.usb1 "ASSoundCM5Alt" "ASSoundCM5Alt" "ASSoundCM5Alt"
	echo "OK"

	echo "Binding USB Device Controller"
	echo $UDC > UDC
	echo peripheral > $UDC_ROLE
	cat $UDC_ROLE
	echo "OK"
	;;

    stop)
	echo "Stopping the USB gadget"

	set +e # Ignore all errors here on a best effort

	cd $GADGET/g_multi.0

	if [ $? -ne 0 ]; then
	    echo "Error: no configfs gadget found"
	    exit 1;
	fi

	echo "Unbinding USB Device Controller"
	grep $UDC UDC && echo "" > UDC
	echo "OK"

	delete_uac2 configs/c.1 uac2.usb0
	delete_uac2 configs/c.1 uac2.usb1
	delete_uac2 configs/c.1 uac2.usb2
	delete_uac2 configs/c.1 uac1.usb0
	delete_uac2 configs/c.1 uac1.usb1
	delete_uac2 configs/c.1 uac1.usb2

	delete_uac2 configs/c.2 uac2.usb0
	delete_uac2 configs/c.2 uac2.usb1
	delete_uac2 configs/c.2 uac2.usb2
	delete_uac2 configs/c.2 uac1.usb0
	delete_uac2 configs/c.2 uac1.usb1
	delete_uac2 configs/c.2 uac1.usb2

	delete_uac2 configs/c.3 uac2.usb0
	delete_uac2 configs/c.3 uac2.usb1
	delete_uac2 configs/c.3 uac2.usb2
	delete_uac2 configs/c.3 uac1.usb0
	delete_uac2 configs/c.3 uac1.usb1
	delete_uac2 configs/c.3 uac1.usb2
	#delete_uac2 configs/c.1 uac2.usb1

	echo "Clearing English strings"
	rmdir strings/0x409
	echo "OK"

	echo "Cleaning up configuration"
	rmdir configs/c.1/strings/0x409
	rmdir configs/c.1
	echo "OK"

	echo "Removing gadget directory"
	cd $GADGET
	rmdir g_multi.0
	cd /
	echo "OK"

	#echo "Disable composite USB gadgets"
	modprobe -r libcomposite
	#echo "OK"
	;;
    *)
	echo "Usage : $0 {start|stop}"
        ;;
esac
