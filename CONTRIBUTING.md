# Contributing

## Development setup

The package is built with Docker or Podman and the Axis ACAP Native SDK image.

Build both ACAP4 architectures with:

```sh
./build.sh
```

Build one architecture with:

```sh
ARCHES=aarch64 ./build.sh
```

Install on a camera:

```sh
PW='...'
curl -s --digest -u "admin:$PW" 'http://DEVICE/axis-cgi/applications/control.cgi?action=stop&package=Metrics'
curl -s --digest -u "admin:$PW" -F "packfil=@releases/Metrics_0_1_0_aarch64.eap" 'http://DEVICE/axis-cgi/applications/upload.cgi'
curl -s --digest -u "admin:$PW" 'http://DEVICE/axis-cgi/applications/control.cgi?action=start&package=Metrics'
```

Do not commit API keys, camera passwords, generated `.eap` files, or local state.

## Pull requests

Keep changes focused, update `CHANGELOG.md` when behavior changes, and include
the camera model and AXIS OS version when reporting hardware results. Verify the
app still works without root privileges.
