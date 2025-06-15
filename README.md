# IntelMausiEthernet
OS X driver for Intel onboard LAN

# Support
Please refer to the driver's thread on insanelymac.com

https://www.insanelymac.com/forum/topic/304235-intelmausiethernetkext-for-intel-onboard-lan/

in case you have further questions, need support or want to submit a problem report. Prebuilt binaries can also be found there. Support requests here on Github will be ignored!

# Project Description
A few days before Christmas I started my latest project, a new driver for recent Intel onboard LAN controllers. My intention was not to replace hnak's AppleIntelE1000e.kext completely but to deliver best performance and stability on recent hardware. That's why I dropped support for a number of older NICs. Currently the driver supports:
 
- 5 Series
  - 82578LM
  - 82578LC
  - 82578DM
  - 82578DC
- 6 and 7 Series
  - 82579LM
  - 82579V
- 8 and 9 Series
  - I217LM
  - I217V
  - I218LM
  - I218V
  - I218LM2
  - I218V2
  - I218LM3
- 100 Series
  - I219V
  - I219LM
  - I219V2
  - I219LM2
  - I219LM3
- 200 Series (since V2.3.0d0)
  - I219LM
  - I219V
- 300 Series (since V2.4.0d0)
  - I219LM
  - I219V

Key Features of the Driver
- Support for multisegment packets relieving the network stack of unnecessary copy operations when assembling packets for transmission.
- No-copy receive and transmit. Only small packets are copied on reception because creating a copy is more efficient than allocating a new buffer.
- TCP, UDP and IPv4 checksum offload (receive and transmit).
- Support for TCP/IPv6 and UDP/IPv6 checksum offload.
- Makes use of the chip's TCP Segmentation Offload (TSO) feature with IPv4 and IPv6 in order to reduce CPU load while sending large amounts of data (disabled due to hardware bugs).
- Fully optimized for Yosemite and Mavericks (64bit architecture) but should work with Mountain Lion and Lion too, provided you build from source with the 10.8 or 10.7 SDK.
- Support for Energy Efficient Ethernet (EEE).
- VLAN support is implemented but untested as I have no need for it.
- The driver is published under GPLv2.
