# esp32-current-logger

This is just a small project of mine designed to run on an ESP32.
It will monitor the current flow over time and display a graph on its web interface.

Its preliminary purpose is to be used in electrolysis to count the number of electrons having passed through the
medium. (Unit is Moles of Electrons)

## Used libraries
- [chart.js](https://www.chartjs.org/)
- [moment.js](https://momentjs.com/)
- [chartjs-adapter-moment](https://github.com/chartjs/chartjs-adapter-moment)
- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)

## Wiring
This software was tested on an ESP32-WROOM-32.

Required Modules:
- A current sensor based on the ACS712-chip. (Or similar)
  * The sensor must be bidirectional and have its zero-point at half of VCC.
- A SPI SD-Card module such as the HW-125.

SPI Pinout of the ESP32:
| SPI PIN       | ESP32 PIN       |
| ------------- |:---------------:|
| MOSI          | GPIO 23         |
| MISO          | GPIO 19         |
| SCK/CLK       | GPIO 18         |
| CS/SS         | GPIO 5 was used |

*Note:* In my experience the HW-125 has to be powered by 5V in order to properly work.


Current Sensor Pin: GPIO35

## Usage
The contents of the [SdCard](/SdFiles/) folder have to be at the root directory of the inserted sdcard.

After flashing your ESP32 it should be ready.
Serial communication happens at 115200 baud.
Line terminator is just a newline character `\n`.

The first thing necessary is to setup wifi using the `wifi` command:
```
wifi <SSID> <Password>
```

### Referance of all available commands
(Also printable from the ESP32 using the command `help`)
| Command                    | Action                                                             |
| -------------------------- | ------------------------------------------------------------------ |
| help                       | Displays this help                                                 |
| wifi &lt;SSID&gt; &lt;pwd&gt;          | Sets WIFI SSID and password                                        |
| printwifi                  | Display Wifi settings                                              |
| wifistatus                 | Display Wifi status                                                |
| setmes &lt;interval&gt;          | Set Graph measurement interval (in seconds)                        |
| getmes                     | Display Graph measurement interval                                 |
| setoff &lt;voltage&gt;           | Set current sensor output offset                                   |
| setmul &lt;factor&gt;            | Set current sensor output coefficient                              |
| getcalib                   | Display current sensor calibration values (offset and coefficient) |
| startcalib1 &lt;count&gt; &lt;aref&gt; | Calibrate Offset (aref is applied current)                         |
| startcalib2 &lt;count&gt; &lt;aref&gt; | Calibrate Coefficient (aref is applied current)                    |

### Calibration procedure
Use an external power supply which can be current limited.
Supply a known amount of current through the current sensor.

Then use the startcalib commands to calibrate.
Generally each of them only has to be performed once.

For example:
```
startcalib1 100 0
```
This will calibrate the offset when no current is applied.
It will take record of 100 measurements and average them out.
Afterwards a fitting offset will be calculated to bring the measured values closer the the real current that
has been flowing (in this case 0).

```
startcalib2 100 5
```
This will find the correct multiplier so that the read value becomes the applied current (in this case 5 amps).
Before the calibration has not finished do not change the applied current.

## Screenshots
Start-Page

![Startpage](/assets/interface1.png)

Graph of a run

![Graph](/assets/interface2.png)