# One-time speed requests (1.0.1)

```sh
sudo dpkg -i dist/nvfancontrol_1.0.1_arm64.deb
sudo nvfancontrol --speed 9000
sudo nvfancontrol status
sudo nvfancontrol auto
nvfancontrol --help
```

Stop any running `nvfanwatch` (`sudo ./dist/nvfanwatch --stop`) or earlier
foreground/curve-service controller before upgrading or making manual requests.
Release existing overrides with `sudo nvfancontrol auto` before an upgrade.
Installation requires matching NVIDIA kernel headers; Secure Boot requires
module signing. No service is started by installation.

`--speed RPM` accepts decimal integers **1890 through 13500 inclusive**. This
conservative supported interval uses the larger documented fan minimum (1890)
and the larger maximum (13500). It is not a dynamically discovered hardware
range. Values outside it are rejected, also by the kernel sysfs handler.

The command sends one EC high-override request, confirms the driver's recorded
state, and exits. It does not launch a monitor or register a curve. The floor
remains until another request or an EC reset; unloading alone does not clear it.
`auto` removes the floor; `max` requests full cooling. Only the lower speed
bound is modified, never the upper clamp. Other writers must not own the EC.

The value is a **common RPM floor request**, not exact measured RPM. Each fan
uses its own conversion range; the factory thermal policy can demand more
cooling. `status` reads the module's last acknowledged request, not live RPM.
A failed transaction can leave the actual state unknown; do not blindly retry.

The module has no profile option. The separate optional C monitor still has its
own auto/max threshold profiles; it is independent of this one-time command.

## Build

```sh
make -C nvfancontrol/usr/src/nvfancontrol
python3 -m unittest discover -s tests -v
make -C nvfancontrol/usr/src/nvfancontrol clean
mkdir -p dist
dpkg-deb --root-owner-group --build nvfancontrol dist/nvfancontrol_1.0.1_arm64.deb
```

Tests cover CLI boundaries and malformed input without accessing hardware.
The module was compiled against 6.17.0-1032-nvidia; live speed changes have not
been tested as part of this change.
