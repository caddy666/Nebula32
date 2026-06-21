# SDCARD_BASE — SD card skeleton

Copy the **contents** of this folder to the root of a freshly formatted
(FAT32/exFAT) SD card to get a Nebula32 card ready to go:

```bash
cp -r SDCARD_BASE/* /media/<your-sd-card>/
```

Then add your disc images (`.iso` / `.bin`+`.cue` / `.nrg`+`.mds` / `.mdf`).

## What's here

| Path | Purpose |
|------|---------|
| `nebula32.cfg` | Default settings (image dir, WiFi, logging, playlist debounce). **This is the file the firmware reads** — edit on the card; takes effect on next boot. |
| `covers/` | JPEG cover art, matched to disc images by name. |
| `certs/nebula32.crt.der` | TLS certificate (DER) the web UI serves over HTTPS. |
| `certs/nebula32.key.der` | Matching private key (DER). |

The `certs/` files are **generated** by `tools/gen_tls_cert.sh` (run automatically
during a WiFi build, or manually: `bash tools/gen_tls_cert.sh`) and are gitignored
because they contain a private key. If `certs/` is empty, the firmware falls back
to its compiled-in self-signed certificate — copying the generated files here just
lets you use (and replace) your own.

Add your disc images here, plus optionally a `playlists/` folder of `.m3u` files
for the carousel.

> **Note:** the active config filename is `nebula32.cfg`. An older `cd32_ode.cfg`
> may linger from earlier builds — it is **not** read and can be deleted.
