# Running Kenshi-Online on Unraid

Two paths depending on how much UI integration you want.

## Path A — Drop-in via Unraid Community Apps template

Best UX. The template gives you the standard Unraid Edit dialog with
correctly-typed Path/Port/Variable fields.

1. Copy `unraid-template.xml` from this directory to:
   ```
   /boot/config/plugins/dockerMan/templates-user/kenshi-online-server.xml
   ```
2. In the Unraid web UI: **Docker** tab → **Add Container** → pick
   `kenshi-online-server` from the user-templates dropdown.
3. Confirm or adjust:
   - **Game port** — `27800/UDP` (forward on your router if public; skip if using Tailscale)
   - **Data** — `/mnt/user/appdata/kenshi-online-server` (default)
   - **PUID/PGID** — `99/100` (Unraid's `nobody:users`, default)
4. Click **Apply**. First start creates `data/server.json` with defaults
   and boots on UDP 27800.

The XML references the GHCR image. Until that image is published with
the next tag, set the **Repository** field to `kenshi-online-server:local`
and build the image on the Unraid host first:

```bash
git clone --recursive https://github.com/andperks6/Kenshi-Online.git /mnt/user/appdata/kenshi-online-build
cd /mnt/user/appdata/kenshi-online-build
docker build -t kenshi-online-server:local .
```

## Path B — docker-compose

If you prefer compose, use `docker-compose.yml` (basic) or
`docker-compose.tailscale.yml` (sidecar with private tailnet) from this
directory. See [`README.md`](README.md) for details.

## Tailscale on Unraid

You have two options that don't conflict:

| | What runs Tailscale | Server address |
|---|---|---|
| **Host integration** | Unraid v7+ native [Tailscale on Docker](https://tailscale.com/docs/integrations/unraid) | `unraid-host.tailnet.ts.net:27800` |
| **Sidecar** (`docker-compose.tailscale.yml`) | Tailscale container, server joins its netns | `kenshi-online-server.tailnet.ts.net:27800` |

The sidecar gives the server its own tailnet identity, useful if you
want to move it to another host without changing the connection string
players use, or if you want the server to be reachable from the tailnet
even when Unraid's host is on a different ACL group. Otherwise the host
integration is simpler.

## Updating

When a new release tag lands and the GHCR image updates, in Unraid:
**Docker** → **Check for updates** → click the orange icon next to
`kenshi-online-server` → **Apply Update**. Saves under `/data` survive
the upgrade.

## Logs

Unraid web UI: click the container icon → **Logs**. Watch for
`Server '...' started on port 27800` after first boot.

## Troubleshooting

- **"Permission denied" on first start** — the container's entrypoint
  chowns `/data` to PUID:PGID before starting the server. If you set
  PUID/PGID after the directory was created with different ownership,
  the chown fixes it on next start.
- **Players can't connect** — confirm UDP 27800 is forwarded (or that
  players are on your tailnet for the Tailscale path). UPnP is
  disabled in the Linux build (it's Windows-only); use explicit
  forwards.
- **Server appears empty in browser** — the server registers with the
  upstream master at `162.248.94.149:27801` by default. If you've
  blocked outbound UDP, players need to use **Direct IP** instead of
  the in-game browser.
