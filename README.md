  ecspi
=======

Linux SPI driver for Elias Crespin art projects involving Gumstix Overo 
computers.


  Build Instructions
-------

Standard external to tree Linux kernel module Makefile is included.

There is also a environment file that can be sourced if using the standard
OE build system that Gumstix suggests. 

  Operation
-------

To be completed. 

For testing, the driver repeatedly outputs two 192 byte spi_transfers out SPI
bus 1, CS 1 (SPI1.1). The two spi_transfers are packaged in one spi_message 
with a CS line pulse between the spi_transfers.

The data is monotonically increasing integers.

The time between spi_messages is controlled via an hrtimer whose frequency is
currently constant, determined by the module parameter write_frequency.



