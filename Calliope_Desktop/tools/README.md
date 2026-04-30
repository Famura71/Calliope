# Local adb bundle

Place the contents of the Android SDK `platform-tools` folder here if you want Calliope Desktop to carry its own `adb` copy.

Expected layout:

```text
Calliope_Desktop/tools/platform-tools/adb.exe
Calliope_Desktop/tools/platform-tools/AdbWinApi.dll
Calliope_Desktop/tools/platform-tools/AdbWinUsbApi.dll
```

If these files are present, the app will prefer them before falling back to the system Android SDK installation.

The app looks for this folder relative to the executable and the project tree, so you can keep it under `Calliope_Desktop/tools/platform-tools` in the repo and copy the same layout next to the built `.exe` if you package a release.
