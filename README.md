  ecspi
=======

Linux SPI driver for Elias Crespin art projects involving Gumstix Overo 
computers.

  Kernel Config
-------

The default gumstix kernels assign drivers to the two SPI CS pins that are 
exposed on the expansion card header.

Since only one SPI driver can be assigned to each BUS.CS pin, the ecspi module 
won't load unless you first modify your kernel.

This is the code in the board-overo.c file that we have to disable.

	static struct spi_board_info overo_spi_board_info[] __initdata = {
	#if defined(CONFIG_TOUCHSCREEN_ADS7846) || \
		defined(CONFIG_TOUCHSCREEN_ADS7846_MODULE)
		{
			.modalias		= "ads7846",
			.bus_num		= 1,
			.chip_select		= 0,
			.max_speed_hz		= 1500000,
			.controller_data	= &ads7846_mcspi_config,
			.irq			= OMAP_GPIO_IRQ(OVERO_GPIO_PENDOWN),
			.platform_data		= &ads7846_config,
		},
	#elif defined(CONFIG_SPI_SPIDEV) || defined(CONFIG_SPI_SPIDEV_MODULE)
		{
			.modalias		= "spidev",
			.bus_num		= 1,
			.chip_select		= 0,
			.max_speed_hz		= 1000000,
			.mode			= SPI_MODE_0,
		},
	#endif
	#if defined(CONFIG_PANEL_LGPHILIPS_LB035Q02) || \
		defined(CONFIG_PANEL_LGPHILIPS_LB035Q02_MODULE)
		{
			.modalias		= "lgphilips_lb035q02_panel-spi",
			.bus_num		= 1,
			.chip_select		= 1,
			.max_speed_hz		= 500000,
			.mode			= SPI_MODE_3,
		},
	#elif defined(CONFIG_SPI_SPIDEV) || defined(CONFIG_SPI_SPIDEV_MODULE)
		{
			.modalias		= "spidev",
			.bus_num		= 1,
			.chip_select		= 1,
			.max_speed_hz		= 1000000,
			.mode			= SPI_MODE_0,
		},
	#endif
	};


If you are building from the latest gumstix kernel, linux-omap3-2.6.36, then the
changes to the defconfig can be accomplished with the following sed calls.

	sed -i 's:CONFIG_TOUCHSCREEN_ADS7846=m:# CONFIG_TOUCHSCREEN_ADS7846 is not set:g' defconfig
	sed -i 's:CONFIG_PANEL_LGPHILIPS_LB035Q02=y:# CONFIG_PANEL_LGPHILIPS_LB035Q02 is not set:g' defconfig
	sed -i 's:CONFIG_SPI_SPIDEV=y:# CONFIG_SPI_SPIDEV is not set:g' defconfig

The defconfig file can be found here

	overo-oe/org.openembedded.dev/recipes/linux/linux-omap3-2.6.36/defconfig

After making the changes, clean the old kernel and rebuild it and the rootfs.

	$ bitbake -c clean virtual/kernel
	$ bitbake virtual/kernel
	$ bitbake <image>   // something like omap3-console-image


  Build Instructions
-------

Standard external to tree Linux kernel module Makefile is included.

There is also a environment file that can be sourced if using the standard
OE build system that Gumstix suggests. 

The build assumes that you have a built kernel in your OETMP working directories.
You will if you built a kernel as above.


  Operation
-------

To be completed. 

For testing, the driver repeatedly outputs two 192 byte spi_transfers out SPI
bus 1, CS 1 (SPI1.1). The two spi_transfers are packaged in one spi_message 
with a CS line pulse between the spi_transfers.

The data is monotonically increasing integers.

The time between spi_messages is controlled via an hrtimer whose frequency is
currently constant, determined by the module parameter write_frequency.



