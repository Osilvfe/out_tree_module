# Caihong SSC tools

`ssc-inspect` creates SSC sensors through libssc and prints their identity
properties. With no arguments it checks the active accelerometer, gyroscope,
and magnetometer. Pass one or more SSC data types to inspect arbitrary streams
such as `rgb` or `cct`. It does not open or reconfigure the physical sensor
buses. The tool intentionally avoids non-identity properties whose GObject
types are incorrect in libssc 0.4.4.

Build and run on the tablet:

```sh
make
./ssc-inspect
./ssc-inspect rgb cct wise_rgb ambient_light
./ssc-inspect --stream rgb 5
```

Stream mode subscribes for the requested duration, prints every standard SSC
float-array event, and disables the stream before exiting.
