# Original module and separate C controller

The working `nvfancontrol/` sources now build 1.0.1 with numeric speed support;
see [speed usage](speed.md). This separate controller still uses only auto/max.
The separate `watcher/nvfanwatch.c` adds no kernel features and cannot request
intermediate RPMs. It polls ACPI sensors every two seconds and switches between
auto and max. No Python, YAML, or systemd service is required.

## Build

```sh
make -C watcher all test
mkdir -p dist
dpkg-deb --root-owner-group --build nvfancontrol dist/nvfancontrol_1.0.1_arm64.deb
```

The C binary links to libc. Build on the target architecture. Kernel headers are
needed when installing the original Debian package; Secure Boot signing remains
the upstream installation's responsibility.

## Install the module package

Stop an old foreground profile with Ctrl+C. If the earlier curve service is
running, stop it before installing. Then, from the repository:

```sh
sudo nvfancontrol auto
sudo dpkg -i dist/nvfancontrol_1.0.1_arm64.deb
sudo nvfancontrol auto
```

This repository change does not
itself install anything or modify running fan settings.

## Run

```sh
sudo ./dist/nvfanwatch --profile 1
sudo ./dist/nvfanwatch --status
sudo ./dist/nvfanwatch --profile 2
sudo ./dist/nvfanwatch --stop
```

The installed nvfancontrol module is loaded automatically if necessary.
Startup diagnostics are printed directly to the terminal.
Profile selection starts a background process and returns after the first
successful evaluation. Repeating it changes the running process's profile;
the reply reports that the new profile will be evaluated next. No boot-time
registration is made. `--foreground` is available for troubleshooting.
`--stop` waits for automatic-control restoration before acknowledging success.
Logs are available with `journalctl -t nvfanwatch`.

| Profile | Request max | Return to auto |
| --- | --- | --- |
| 1 | >=85°C | <80°C |
| 2 | >=80°C | <75°C |
| 3 | >=75°C | <70°C |

These are custom on/off thresholds, not EC Profile A/B or the earlier multi-RPM
curves. They have not been thermally validated. The hottest ACPI sensor is used;
a sensor error stops the process and attempts auto. Writes occur only when the
requested state changes, plus an explicit auto request at normal shutdown.
A failed write or existing driver error stops further writes because EC state
is uncertain. SIGKILL, a crash, or a transport failure can prevent cleanup;
there is no EC-side expiry. `status` shows requested mode, not measured RPM.

The daemon holds the original CLI's advisory lock for its lifetime. Use
`nvfanwatch --stop` before manual commands, package upgrades, or module removal.
Other programs writing sysfs directly can bypass that lock and are unsupported.
The control socket is root-only at `/run/nvfanwatch.sock`.

Validation: compilation with warnings as errors, three-profile threshold and
hysteresis tests, help and invalid-argument checks, and original kernel module
build. Real EC transitions and background lifecycle require hardware testing;
the build process does not run the controller on the host.
