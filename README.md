This program allows displays to be used with MQTT equipped versions of the MMDVM. It takes
the structured information, in JSON format, provided by the Host and Info programs, and converts it for use by
displays. It currently supports the display types that were in the MMDVM-Host, but making the display
handling external and simpler should allow for easier modifications and addition of new display
types.

Environmental information that is not supplied by the Host is obtained via JSON created by the MMDVM-Info
program. Some of this is supplied periodically automatically by the Info program, and some of it needs to be polled by
the Display program since it is only needed once and will not change for the duration of running of the
MMDVM system.

It builds on 32-bit and 64-bit Linux. It can control various displays, these are:

- HD44780 (sizes 2x16, 2x40, 4x16, 4x20)
	- Support for HD44780 via 4 bit GPIO connection (user selectable pins)
	- Adafruit 16x2 LCD+Keypad Kits (I2C)
	- Connection via PCF8574 GPIO Extender (I2C)
- Nextion TFTs (all sizes, both Basic and Enhanced versions)
- OLED 128x64 (SSD1306)
- LCDproc

The Nextion displays can connect to the UART on the Raspberry Pi, or via a USB
to TTL serial converter like the FT-232RL. It may also be connected to the UART
output of the MMDVM modem (Arduino Due, STM32, Teensy).

Commands sent by a Nextion screen's buttons (ON7LDS layouts: 0x2A, a command
byte and its parameters, such as "2A F0 reboot FF FF FF") are published as they
came, without the 0x2A and the FF FF FF, on the "<Name>/nextion-out" topic. This
program runs none of them: another program may act on them. Each message on the
"<Name>/nextion-in" topic is sent to the screen as one Nextion instruction
(without its FF FF FF), so that program can answer, e.g. 'msg.txt="Rebooting"'.

With the ON7LDS DIY layout (ScreenLayout=3), this program also answers what
that layout asks for itself, as NextionDriver did, so it needs nothing else:

- 2A FE <page>: the page's fields again (it asks after its own pages).
- 2A FC 1 / 2A FC 0: hold updates while it shows its own pages, then carry on;
  a call that came meanwhile is shown as it is now.
- 2A FC 2: the screen's model and serial number (msg1, msg2).
- 2A FD <page> <lines>: the page's last heard list (LH<n>t<f>).
- The modes MMDVM-Host has enabled (A1-A6) and its networks with their
  gateways running, by MMDVM-Info's list of programs, and the internet (N0-N6).
- The free space of the disk (t23), and talkgroup names (t9 slot 1, t8 slot 2)
  from the GroupsFile set in [Nextion].
- Callers' details (t13-t17 DMR, t18-t19 NXDN) from DMRIdFile and NXDNIdFile in
  [Nextion]: the same files as MMDVM-Host's [DMR Id Lookup] and [NXDN Id Lookup]
  (DMRIds.dat or RadioID's user.csv, and NXDN.csv), read on disk as each call
  starts, not loaded into memory.
- Sleep after SleepWhenIdle seconds idle; a call or a touch wakes it.

These commands are not published on "<Name>/nextion-out"; the others are.

The HD44780 displays are integrated with wiringPi for Raspberry Pi based
platforms.

The OLED display needs an extra library see OLED.md

The LCDproc support enables the use of a multitude of other LCD screens. See
the [supported devices](http://lcdproc.omnipotent.net/hardware.php3) page on
the LCDproc website for more info.

This software is licenced under the GPL v2 and is primarily intended for amateur and
educational use.
