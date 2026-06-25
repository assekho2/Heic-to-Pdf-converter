# HEIC to PDF Converter

A fast, no-install command-line tool that batch-converts iPhone `.heic` photos to `.pdf`. Each photo becomes a one-page PDF. Multithreaded, works on Windows and macOS, and the downloads are single self-contained binaries — no libraries or runtimes to install.

![Terminal demo of heic_converter_mt converting a folder of HEIC photos](docs/demo.svg)

## Download

Grab the latest release from the [**Releases page**](https://github.com/assekho2/Heic_to_Jpg_Converter/releases):

| Platform | File |
|---|---|
| Windows 10/11 (x64) | `heic_converter_mt-windows-x64.zip` |
| macOS (Apple Silicon) | `heic_converter_mt-macos-arm64.tar.gz` |

## Quick start

Each download unzips to a `HEIC-to-PDF-Converter` folder containing the program, a `photos to convert` folder, a double-click launcher, and a `readme.txt`.

### Windows

1. Unzip the download. Open the `HEIC-to-PDF-Converter` folder.
2. Drop your `.heic` photos into the `photos to convert` folder.
3. Double-click **`Convert.bat`**. PDFs appear in a new `output` subfolder.

> **"Windows protected your PC"?** SmartScreen warns about unsigned downloads. Click **More info → Run anyway**.

Prefer a terminal? Type `cmd` in the Explorer address bar and run `heic_converter_mt.exe "photos to convert"`.

### macOS

1. Unzip the download (double-click the `.tar.gz`). Open the `HEIC-to-PDF-Converter` folder.
2. Drop your `.heic` photos into the `photos to convert` folder.
3. Double-click **`Convert.command`**. PDFs appear in a new `output` subfolder.

   First run only: macOS may block the unsigned launcher — right-click `Convert.command`, choose **Open**, then **Open** again.

Prefer a terminal? Run `xattr -d com.apple.quarantine ./heic_converter_mt` once, then `./heic_converter_mt "photos to convert"`.

Full instructions are also in the `readme.txt` inside each download.

## Usage

```
heic_converter_mt [options] [input_dir]

Converts every .heic file in input_dir (default: current directory)
to a one-page .pdf in the output directory, using multiple threads.

Options:
  -o, --output <dir>   output directory (default: output)
  -q, --quality <n>    image quality 1-100 (default: 90)
  -t, --threads <n>    worker threads (default: CPU cores)
  -h, --help           show this help and exit
  -V, --version        show version and exit
```

Examples:

```bash
heic_converter_mt ~/Pictures/vacation            # convert a specific folder
heic_converter_mt -q 85 -o converted             # quality 85, custom output folder
heic_converter_mt -t 4                           # limit to 4 threads
```

## Building from source

The repo contains two converters:

- `heic_converter_mt.cpp` — the multithreaded converter described above (this is what releases ship)
- `heic_converter.c` — a simpler single-threaded version

Both build on Windows, macOS, and Linux via the Makefile.

### Dependencies

- **macOS:** `brew install libheif jpeg`
- **Windows:** install [MSYS2](https://www.msys2.org), then in a UCRT64 shell:
  `pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libheif mingw-w64-ucrt-x86_64-libjpeg-turbo make`
- **Linux:** `sudo apt install build-essential libheif-dev libjpeg-dev`

### Build

```bash
make                  # both converters, linked against system libraries
make static           # portable heic_converter_mt with libheif/libde265/libjpeg
                      # statically linked (what CI builds for releases)
```

`make static` runs `scripts/build_static.sh`, which downloads and builds libheif (HEIC decode only) from source. It needs cmake/ninja/pkgconf on Windows (`pacman -S mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf`) or `brew install cmake ninja jpeg-turbo` on macOS.

## Releases / CI

GitHub Actions builds Windows and macOS binaries on every push. Pushing a tag like `v1.1.0` builds, packages, and attaches the binaries to a GitHub Release automatically.

## How it works

Each page's image is JPEG-compressed (the `-q` quality option controls that compression) and embedded directly into the PDF using the `DCTDecode` filter — so the PDFs stay compact and no separate PDF library is needed.

## License

[MIT](LICENSE). HEIC decoding uses [libheif](https://github.com/strukturag/libheif) and [libde265](https://github.com/strukturag/libde265); image compression uses [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo).
