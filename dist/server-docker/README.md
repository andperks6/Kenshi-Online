# Kenshi-Online — Dedicated Server in Docker

Run the dedicated server on Linux without building anything. The image is
published to GitHub Container Registry on every release tag:

```
ghcr.io/andperks6/kenshi-online-server:latest
ghcr.io/andperks6/kenshi-online-server:vX.Y.Z
```

Two compose recipes are provided:

| File | When to use |
|---|---|
| `docker-compose.yml` | LAN play, port-forwarded internet host, or you're on Unraid v7+ using its [native Tailscale-on-Docker integration](https://tailscale.com/docs/integrations/unraid) so the container inherits the host's tailnet identity. |
| `docker-compose.tailscale.yml` | Server gets its own tailnet node via a sidecar — no public ports, friend-mesh only, host-portable. Recommended for "play with friends over the internet" without exposing UDP 27800 to the WAN. |

Pick one. They don't conflict — Unraid's host-level Tailscale and the
sidecar can coexist (you'd just have two distinct tailnet nodes).

**On Unraid?** Skip the compose recipes and use
[`UNRAID.md`](UNRAID.md) + the `unraid-template.xml` in this directory
for the proper Community Apps experience.

## Config

The container creates `data/server.json` with sensible defaults on
first run, so you can `docker compose up -d` with no prep. To pre-edit
instead, copy the example before the first start:

```bash
mkdir -p data && cp server.json.example data/server.json
$EDITOR data/server.json
```

## Basic (port-forwarded or LAN)

```bash
mkdir -p data
docker compose up -d
```

The server reads `data/server.json`. If absent, it writes defaults on first
run — edit and `docker compose restart`. Saves persist in `data/`.

External hosts: forward UDP 27800 from your router to the host. UPnP is
disabled in the Linux build (it's a Windows-only feature).

## Tailscale sidecar (recommended for friend servers)

1. Generate a [reusable auth key](https://login.tailscale.com/admin/settings/keys),
   ideally with an ACL tag like `tag:game-server`.
2. Create `.env` next to the compose file:
   ```
   TS_AUTHKEY=tskey-auth-...
   ```
3. ```bash
   mkdir -p data tailscale-state
   docker compose -f docker-compose.tailscale.yml up -d
   ```
4. Players install Tailscale, accept the invite to your tailnet, and
   connect to `kenshi-online-server.<your-tailnet>.ts.net:27800` from
   the in-game **Direct IP** field.

The server has no auth gate of its own — anyone who can reach the UDP
port can join. The tailnet is the auth boundary.

## Master server (optional)

The master server is the central server browser registry on UDP 27801.
You only need to run one instance; everyone else's `server.json` points
at it. To run it instead of the game server, override the entrypoint:

```yaml
  kenshi-online-master:
    image: ghcr.io/andperks6/kenshi-online-server:latest
    entrypoint: ["/usr/bin/tini", "--", "/usr/local/bin/KenshiMP.MasterServer"]
    ports:
      - "27801:27801/udp"
    restart: unless-stopped
```

## Updating

```bash
docker compose pull && docker compose up -d
```

Saves under `data/` survive image updates. The server flushes on SIGTERM
(container stop) so a clean `docker compose down` is safe.

## Logs

```bash
docker compose logs -f kenshi-online-server
```

## Building locally instead of pulling

```bash
docker build -t kenshi-online-server:dev ../..
docker run --rm -p 27800:27800/udp -v "$PWD/data:/data" kenshi-online-server:dev
```

(Build context is the repo root, two directories up from this README.)
