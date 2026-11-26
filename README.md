# InfluClick

Ultra fast auto clicker for Windows 10/11. C++20, Dear ImGui, DirectX 11, `SendInput`.

## Build

Just run **`build.bat`** (double-click or from a terminal). Nothing needs to be installed beforehand:

- uses Visual Studio / Build Tools (`cl`) if present, otherwise MinGW-w64 (`g++`) from PATH,
- if neither exists, it offers to download a portable MinGW-w64 into `tools\` (one-time, ~250 MB),
- downloads Dear ImGui 1.91.9 into `third_party\imgui` (one-time).

The result is `InfluClick.exe` in the project folder (static, no extra DLLs or redistributables).

`build.bat nopause` skips the final pause, `build.bat clean` removes build output, `build.bat clean all` also removes the downloaded dependencies.

### CMake (optional)

```
cmake -S . -B build -G Ninja
cmake --build build
```

## Project layout

```
build.bat
CMakeLists.txt
src/
  main.cpp
  app.rc        resources: manifest, version info, optional icon
  app.manifest
  app.ico       optional application icon
```

A diagnostic log `influclick_log.txt` is written next to the exe.

## License

InfluClick is released under the [MIT License](LICENSE).

Third-party components (Dear ImGui, ProggyClean font, compiler runtimes) and their
licenses are listed in [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt). If you
distribute `InfluClick.exe`, include both `LICENSE` and `THIRD_PARTY_NOTICES.txt`.

## Disclaimer

This software is provided as is, without warranty of any kind. Use it at your own
risk. Do not use it in online games or services whose terms forbid automation, you
are solely responsible for how you use it. InfluClick is not affiliated with
Microsoft or any other third party.
