# Press F5 to bring up the Debug configuration picker

## Problem

VS Code default behaviour: **F5 launches the last-used Debug configuration directly**, without showing a picker.

When a project has several Debug configurations (e.g. "with args" and "no args") the only way to switch is `Ctrl+Shift+D` to open the Run & Debug panel and pick from the dropdown at the top.

## Solution

Edit the **user-level** keybinding so that F5 always pops up the configuration picker:

1. `Ctrl+Shift+P` -> type `Preferences: Open Keyboard Shortcuts (JSON)` -> Enter
2. Add the following to `keybindings.json`:

```json
[
    {
        "key": "f5",
        "command": "workbench.action.debug.selectandstart",
        "when": "!inDebugMode"
    }
]
```

3. Save.

## Result

- Press F5 -> the configuration picker appears -> choose "C++ Debug: pick sample (with args)" or "C++ Debug: pick sample (no args)" -> the file picker appears -> the project is built automatically and debugging starts.
- While debugging (`inDebugMode`) F5 still behaves as Continue; the picker is not shown again.

## Notes

- **Remote-SSH**: the shortcut takes effect on the **local client (Windows)**; do not modify the server-side keybindings.json.
- This is a **user-level** setting that affects every project. To restore the default behaviour, remove the entry above.
- If you want the default F5 behaviour (launch the last configuration directly), delete the entry shown above.
