# systemd deployment

Create an unprivileged `hoshiguard` user, install the repository at
`/opt/hoshiguard`, copy the environment example to
`/etc/hoshiguard/hoshiguard.env`, then install the unit:

```sh
sudo useradd --system --home /nonexistent --shell /usr/sbin/nologin hoshiguard
sudo install -d -o hoshiguard -g hoshiguard /var/lib/hoshiguard
sudo install -D -m 0644 packaging/systemd/hoshiguard.service /etc/systemd/system/hoshiguard.service
sudo install -D -m 0640 packaging/systemd/hoshiguard.env.example /etc/hoshiguard/hoshiguard.env
sudo systemctl daemon-reload
sudo systemctl enable --now hoshiguard
```

Use a udev rule tied to the USB-UART adapter attributes to create the stable
`/dev/hoshiguard` symlink. Do not hard-code `/dev/ttyUSB3`. The service joins
`dialout`; it does not run as root. View diagnostics with
`journalctl -u hoshiguard -f` and machine health at
`/run/hoshiguard/health.json`.

The default unit is hardened with `NoNewPrivileges`, `ProtectSystem=strict`,
`ProtectHome`, and explicit writable state/runtime directories. If the checkout
or output directory differs, use a systemd drop-in to override
`WorkingDirectory`, `PYTHONPATH`, `ReadWritePaths`, and the environment file;
do not weaken the entire sandbox.

SIGTERM sets the receiver stop flag. The current record completes, NDJSON gets
a clean `session_end`, files are flushed/fsynced, SHA-256 sidecars are emitted,
and the PID file is removed. `Restart=always` makes capture independent of an
SSH session.

SHA-256 is accumulated while records are written. Rotation therefore does not
pause serial consumption to re-read a complete large segment; only the configured
flush/fsync and record-boundary file switch remain on the close path.

Disk defaults warn below 1 GiB, disable derived NDJSON below 512 MiB while
retaining HGRAW, and stop safely below 128 MiB. Override thresholds on the CLI
for the deployment volume. Raw evidence remains the default priority;
`--disk-critical-action drop-archive` explicitly reverses the critical policy,
and `--disk-critical-action stop` disables both outputs immediately at the
critical threshold. A stop is explicit and never writes a partial HGRAW record.
