
# Enable IntelliSense with CMake compile_commands.json
Configure CMake to automatically generate compile_commands.json with all compilation flags, including all -I include directories
IntelliSense reads the actual include paths from it.
In VS code, press Ctrl+Shift+P -> "C/C++: Reset IntelliSense Database" to make IntelliSense reindex.
After that, you can view the function definitions for OpenVINO, CV, and etc. And you can also jump to them by pressing F12 during debuging.

* CMakeLists.txt
```
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
```

* .vscode/c_cpp_properties.json
```
{
    "configurations": [
        {
            "name": "Linux",
            "compileCommands": "${workspaceFolder}/build/compile_commands.json",
            "cppStandard": "c++17",
            "intelliSenseMode": "linux-gcc-x64"
        }
    ],
    "version": 4
}
```
Rerun the build command `cmake .. && echo "Done"` and it will create file `build/compile_commands.json`.

