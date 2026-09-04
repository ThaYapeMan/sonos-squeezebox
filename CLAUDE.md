# CLAUDE.md

## What this is

Fork of [newinnovations/sonos-squeezebox](https://github.com/newinnovations/sonos-squeezebox). Registers Sonos speakers as real, synchronisable LMS players. Not an LMS plugin — a standalone program built on the squeezelite codebase with a Sonos output driver instead of ALSA.

## Build environment

Builds on LXC 113 (192.168.178.31), not in WSL. Claude Code has no SSH access there. Workflow: `git push` here, then pull and build manually on the LXC. Never run `make`, `cmake`, or `g++` locally.

## Submodules

`noson/` and `squeezelite/` are submodules — leave them untouched. Any change inside them will be lost on the next `git submodule update`.

## Our modifications

- **Track metadata**: LMS CLI (port 9090) is queried for track title and artwork at stream start.
- **MAC address**: sent raw, not URL-encoded. URL-encoding causes LMS to silently fail to match the player.
- **Artwork URL**: constructed as `http://<lms>:9000/music/<id>/cover.jpg` because LMS does not include `artwork_url` for local tracks.
- **Player name**: registered as `<room> (Sonos)` (e.g. `Study (Sonos)`) — human-readable in every LMS controller.

## Rejected approaches

**ICY/Shoutcast in-band metadata** — Sonos never sends the `Icy-MetaData: 1` opt-in header for `audio/flac` streams, so injecting ICY blocks corrupts the stream. See README for details.

## Diagnostics

The program buffers stdout when no terminal is attached — `journalctl` shows nothing until the process exits. Use `ss -tnp` to diagnose connection state instead of logs.

## Pitfalls

- Never pass `--server=<ip>:9000`; port 9000 is the web interface. Pass `--server=<ip>` only — slimproto is found on port 3483.

## Conventions

- All documentation and commit messages in English.
- Always push after committing.
