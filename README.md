# esp32-current-logger

This is just a small project of mine designed to run on an ESP32.
It will monitor the current flow over time and display a graph on its web interface.

Its preliminary purpose is to be used in electrolysis to count the number of electrons having passed through the
medium. (Unit is Moles of Electrons)

## Used libraries
- [chart.js](https://www.chartjs.org/)
- [moment.js](https://momentjs.com/)
- [chartjs-adapter-moment](https://github.com/chartjs/chartjs-adapter-moment)

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

## Screenshots
Start-Page

![Startpage](/assets/interface1.png)

Graph of a run

![Graph](/assets/interface2.png)