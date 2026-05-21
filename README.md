# mTerm

mTerm is a small Qt terminal manager built around QTermWidget. It is aimed at working with multiple local or SSH terminal sessions, especially when running the same command across several hosts.

## Features

- Open a local terminal from `Hosts -> localhost`.
- Read SSH hosts from `~/.ssh/config` and add them to the `Hosts` menu.
- Support grouped SSH hosts with comments like:

```sshconfig
Host pve1
  #mGroup PVE
  Hostname 192.168.1.7
  User root
```

This creates `Hosts -> PVE -> pve1` and `Hosts -> PVE -> Open All`.

- SSH host tabs start `ssh <host>` directly as the terminal process, so `exit` closes the tab instead of returning to a local shell.
- Switch between tabbed view and tiled view with `View -> Tabs` and `View -> Tile All`.
- Broadcast typed input from one terminal to other checked terminals.
- Per-terminal broadcast checkbox.
- `Ctrl` + click on a checkbox sets all checkboxes to the same state.
- Active terminal is highlighted in tiled view.
- Empty window message when no terminals are open.

## Shortcuts

- `Ctrl+Right`: next tab in tab view, next terminal in tiled view.
- `Ctrl+Left`: previous tab in tab view, previous terminal in tiled view.
- `Ctrl+Space`: toggle broadcast checkbox for the current terminal.
- `Ctrl+Delete`: close current terminal.

The same list is available in the app via `Help -> Shortcuts`.

## Build Dependencies

On LMDE 7 / Debian 13 trixie:

```bash
sudo apt update
sudo apt install cmake ninja-build g++ qt6-base-dev qt6-tools-dev libqtermwidget-dev libutf8proc-dev
```

## Build

```bash
cmake -S . -B build/manual -G Ninja
cmake --build build/manual
```

## Run

```bash
./build/manual/mTerm
```

## Notes

- Wildcard SSH host entries such as `Host *` or `Host *.example.com` are ignored in the menu.
- Host grouping is read from `#mGroup <name>` comments inside a host block.
- Broadcast is controlled by the checkbox shown next to each terminal title.
