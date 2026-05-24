# mTerm

mTerm is a Qt/QTermWidget terminal broadcaster. Its main purpose is to run the same commands at the same time on multiple terminal sessions, including SSH connections, while still allowing individual terminals to be enabled or disabled for broadcast input.

## Key Idea

Open a set of hosts, select the terminals that should receive input, and type once. mTerm mirrors your keystrokes to every checked terminal. This is useful for administering groups of similar machines, comparing behavior across hosts, or running the same diagnostic commands on a fleet.

## Features

- Broadcast typed input from one terminal to all checked terminals.
- Per-terminal broadcast checkbox.
- `Alt` + `Shift` + click on a checkbox sets all checkboxes to the same state.
- Open a local terminal from `Hosts -> localhost`.
- Read marked SSH hosts from `~/.ssh/config` and add them to the `Hosts` menu.
- Mark a host for mTerm with a `# mTerm` comment.
- Put one marked host in multiple menu groups with comments like:

```sshconfig
Host otherHost
  # mTerm Groups VPS Webserver
  Hostname 192.168.1.7
  User root
```

This creates both `Hosts -> VPS -> otherHost` and `Hosts -> Webserver -> otherHost`. Each group also gets an `Open All` action.

- SSH host tabs run through a small shell wrapper: a successful SSH exit closes the tab immediately, and a failed SSH exit shows a 10-second countdown in the terminal. Press any key during the countdown to keep the tab open in a local shell.
- Screen integration is off by default and can be enabled with `Terminal -> Use screen sessions`.
- When enabled, local terminals attach to a `screen` session named `mterm` if `screen` is installed; otherwise they fall back to the default shell.
- When enabled, SSH host tabs attach to a remote `screen` session named `mterm` if `screen` is installed on the remote host; otherwise they fall back to the remote login shell.
- Choose the folder scanned for markdown task files with `Tasks -> Select Tasks Directory...`.
- Switch between tabbed view and tiled view with `View -> Tabs` and `View -> Tile All`.
- The last selected view mode is saved and restored on startup.
- Active terminal is highlighted in tiled view.
- Terminal titles can update from detected shell prompts such as `user@host:~$`.
- Empty window message when no terminals are open.

## Shortcuts

- `Alt+Shift+Right`: next tab in tab view, next terminal in tiled view.
- `Alt+Shift+Left`: previous tab in tab view, previous terminal in tiled view.
- `Alt+Shift+Space`: toggle broadcast checkbox for the current terminal.
- `Alt+Shift+Delete`: close current terminal.
- `Ctrl+Insert`: copy selected terminal text.
- `Shift+Insert`: paste clipboard text into the current terminal.
- `Ctrl+Shift++`: increase terminal font size.
- `Ctrl+-`: decrease terminal font size.
- `Ctrl+0`: reset terminal font size.

The same list is available in the app via `Help -> Shortcuts`.

## Build Dependencies

On LMDE 7 / Debian 13 trixie:

```bash
sudo apt update
sudo apt install cmake ninja-build g++ qt6-base-dev qt6-tools-dev libqtermwidget-dev libutf8proc-dev dpkg-dev
```

## Build

```bash
make release
```

## Debug Build

```bash
make debug
```

## Run

```bash
make run
```

## Debian Package

```bash
make deb
```

Packages are written to `build/packages` and include an OS-specific version suffix such as `linuxmint7` or `ubuntu2404`.

## Notes

- Wildcard SSH host entries such as `Host *` or `Host *.example.com` are ignored in the menu.
- SSH hosts are only added to the menu when their host block contains a `# mTerm` marker.
- Host grouping is read from `# mTerm Groups <group> [group...]` comments inside a marked host block.
- Broadcast is controlled by the checkbox shown next to each terminal title.
