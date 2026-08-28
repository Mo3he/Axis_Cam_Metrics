# Third-party notices

The packaging and app code in this repository is licensed under the BSD 3-Clause
License; bundled upstream components retain their own licenses.

## Axis ACAP Native SDK

- Project: <https://developer.axis.com/acap/>
- Used at build time to compile and package the application.

## Eclipse Paho MQTT C Client

- Project: <https://github.com/eclipse-paho/paho.mqtt.c>
- License: dual Eclipse Public License 2.0 / Eclipse Distribution License 1.0
  (BSD 3-Clause). This project relies on the EDL 1.0 terms.
- Linked statically. The version is pinned by `ARG PAHO_VERSION` in the
  Dockerfile, which is the authoritative value.
- Built with `PAHO_WITH_SSL=TRUE`; TLS uses the device's own OpenSSL 3 and CA
  trust store rather than a bundled copy.

## uPlot

- Project: <https://github.com/leeoniya/uPlot>
- License: MIT. The full text ships in `app/html/vendor/LICENSE`.
- Bundled at `app/html/vendor/uPlot.iife.min.js` and
  `app/html/vendor/uPlot.min.css` so the dashboard renders on devices with no
  internet access. The version is recorded in the banner comment at the top of
  the bundled script.

## Trademarks

All product names, logos, and brands are property of their respective owners.
This is an independent community project and is not affiliated with or endorsed
by Axis Communications.
