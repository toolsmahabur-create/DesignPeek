# DesignPeek

Real thumbnails and a rich Preview Pane for designer files (PSD, PSB, AI, PDF) in Windows File Explorer.
Free and open source (MIT).

> **Status:** step 1 of 6 — PSD/PSB thumbnails from the embedded Photoshop thumbnail.

ডিজাইনারদের ফাইলের (PSD, PSB, AI, PDF) জন্য Windows File Explorer-এ আসল থাম্বনেইল আর Preview Pane। সম্পূর্ণ ফ্রি ও ওপেন-সোর্স।

## How it works

```
explorer.exe ──(Windows isolated thumbnail process)──► DesignPeek.Shim.dll  (tiny C++ COM server)
                                                          │  starts, with 300 MB memory cap + 3 s timeout
                                                          ▼
                                                    DesignPeek.Worker.exe  (.NET; parses the file)
```

The shim never parses files. The worker receives only the bytes it asks for over a pipe, so a
damaged or huge file can at worst kill the worker — never Explorer. This follows the same
out-of-process pattern as the PowerToys File Explorer add-ons.

## Building

Every push is built by GitHub Actions (`.github/workflows/build.yml`): native shim (CMake/MSVC),
.NET worker, tests, and the Inno Setup installer, which is uploaded as a build artifact.

Full documentation (English + বাংলা), screenshots and code-signing notes come in step 6.
