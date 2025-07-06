Primary Contribitor: Michael Castiglia

How to build PlatformIO based project
=====================================

1. [Install PlatformIO Core](https://docs.platformio.org/page/core.html)
2. Clone this project
3. Run these commands:

```shell
# Change directory
$ cd path/to/mcu-code

# Build project
$ pio run

# Upload firmware
$ pio run --target upload

# Build specific environment
$ pio run -e esp32dev

# Upload firmware for the specific environment
$ pio run -e esp32dev --target upload

# Clean build files
$ pio run --target clean
```

Note: If PIO is installed through VS Code installation, it may not be included in your PATH by default. You may have to execute the following command. Add it to your `.bashrc` if you would like it to persist across sessions.
```shell
export PATH=/home/michael/.platformio/penv/bin/:$PATH
```
