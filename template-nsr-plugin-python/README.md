# NSR Python Plugin Template

This is a template for creating python-based plugins for **NSR (Net muShRoom)**.

## How to use

1. Copy this template directory to start a new plugin.
2. Edit `plugin.py` to implement your custom telemetry handling, custom UI rendering (`render`), or hop annotations (`render_hops`).
3. Change the `PLUGIN_NAME` variable in the `Makefile` to your desired plugin name.
4. Run `make && make install` to install it into your `~/.nsr/plugins/` directory.
5. Launch `nsr`, open the Tools menu by pressing `T` in the TUI, and enable your new plugin.

## JSON-RPC 2.0 API Reference

NSR communicates with plugins over `stdin` and `stdout` using JSON-RPC 2.0:

*   **`init`**: Called when the plugin starts. Returning `"reserved_keys"` will route specified key presses only to this plugin.
*   **`cleanup`**: Called before NSR exits or when disabling the plugin.
*   **`update_telemetry`**: Triggered on every tick. Receives the list of network hops.
*   **`render_hops`**: Annotates hop nodes in the main view.
*   **`render`**: Draws custom UI content inside the dedicated tool panel.
*   **`on_key`**: Delivered when a key is pressed. Return `"handled": true` to intercept and block NSR's default keybind.
