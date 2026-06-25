# nsr-plugged 🔌

`nsr-plugged` is an offline-first monorepo-friendly plugin manager for **NSR (Net muShRoom)**.

It allows you to list, enable, disable, install, uninstall, and create NSR plugins easily from the command line.

## Installation

To build and install `nsr-plugged` to `~/.local/bin/`:

```bash
cd nsr-plugged
make install
```

Make sure `~/.local/bin` is added to your shell's `PATH`.

## Commands

### 1. List Plugins
Lists all plugins installed in `~/.nsr/plugins/` and displays whether they are `[Active]` or `[Inactive]`.

```bash
nsr-plugged list
```

### 2. Enable a Plugin
Enables an installed plugin by setting it to active in `~/.nsrconfig`.

```bash
nsr-plugged enable sparkline
```

### 3. Disable a Plugin
Disables an active plugin without deleting its executable.

```bash
nsr-plugged disable sparkline
```

### 4. Install a Plugin
Installs a plugin from a source directory (by executing its `Makefile` if present, or copying `.py` / `.sh` files) or a single script file.

```bash
nsr-plugged install ../plugins/sparkline
```

### 5. Uninstall a Plugin
Deletes the plugin executable from `~/.nsr/plugins/` and disables it in `~/.nsrconfig`.

```bash
nsr-plugged uninstall sparkline
```

### 6. Create a New Plugin Template
Copies the Python template to `plugins/<name>` within the monorepo and configures the new plugin's metadata.

```bash
nsr-plugged create my-new-plugin
```
