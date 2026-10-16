# ntdlib

ntdlib is a C++20 library for the formats and hardware of Nintendo's consoles: NintendoWare's sound archives, the
consoles' wave encodings, the GameCube's and the Wii's PowerPC and DSP, and GameCube and Wii disc images. It also
models NintendoWare's sequence player, and converts Nintendo's sound data to MIDI, SoundFont 2 and SFZ files.

ntdlib builds on xsflib, a library for players and rippers of the PSF family of formats: byte order, bounds-checked
reads, the PSF container and playback. Each of ntdlib's modules is a static library. A project links
the ones it uses.

| Module | What it holds |
|---|---|
| `ntdlib::commands` | A common form for the commands of the consoles' sequences, with their values in units that are the same on every console |
| `ntdlib::common` | The GameCube's and the Wii's address types, the PowerPC's negated fused multiply-adds, AArch64's floating-point minimum and maximum, DOL executables, and the builds of the Wii's sound code that a main.dol links |
| `ntdlib::ppc` | A PowerPC interpreter for the GameCube, the Wii and the Wii U |
| `ntdlib::dsp` | The GameCube and Wii DSP, with each console's memory, and the accelerator's reads and the accumulators' helpers for models of a DSP program |
| `ntdlib::flipper` | The devices that the GameCube and the Wii share (the DSP interface, EXI, a disc of files and the hardware page's shared blocks), and the parts of a machine for game mode that don't depend on its other devices, with a snapshot of the devices that both consoles have |
| `ntdlib::ax` | What the Wii's and the Wii U's AX libraries share: the constants, the voice allocation and the low-pass coefficients |
| `ntdlib::disc` | GameCube and Wii disc images: ISO, NKit, WBFS and WDF, and extracted discs. With `GameOptions::titles`, also the Wii's titles, such as WiiWare games, from a folder with their TMD and decrypted contents. Titles and file names come out in UTF-8. They're decoded from Shift-JIS, or from Windows-1252 for the title of a disc from outside Japan. With `GameOptions::cpk_sound_archives`, also the sound archives inside CRI's CPK archives, stored as they are or compressed with CRILAYLA (`ntdlib/disc/cpk.h`). It requires no other library |
| `ntdlib::disc_containers` | The compressed containers of disc images: GCZ, CISO, WIA and RVZ, opened through `ntdlib::disc` |
| `ntdlib::sar` | NintendoWare's sound archives for the 3DS (CSAR), the Wii U and the Switch (FSAR), and their parts |
| `ntdlib::sar_layout` | A sound archive of the 3DS, the Wii U or the Switch cut into pieces for an editor: its sequence files, decoded into the form of `ntdlib::commands`, and the runs of bytes between them. Joining the pieces gives the archive back byte for byte. After an edit, joining lays the pieces out again and writes the archive's offsets and sizes. WriteTableValues writes new values into the archive's tables. Also a reader and a writer of the sequence files (CSEQ and FSEQ) |
| `ntdlib::rsar` | NintendoWare's sound archives for the Wii (RSAR), and their parts |
| `ntdlib::musyx` | The readers of Factor 5's MusyX data for the GameCube: a project's groups with their pages, MIDI setups and sound effects, a pool's macros, tables, keymaps and layers, a sample directory with the decoding of its samples, and songs |
| `ntdlib::jaudio` | The readers of JAudio2's sound data: the start-up archive (BAA) with its sound table, sequence collection, wave systems (WSYS) and banks (IBNK), and RARC archives, compressed with Yaz0 or not, and refilled from another release's sequence table. The readers of JAudio1's start-up archive (AAF): its sound table, its wave systems, and the sequence index of an earlier JAudio1. Also the length analysis of a sound in game mode, for JAudio2 and JAudio1 |
| `ntdlib::wave` | Decoders of DSP-ADPCM (the DSP accelerator's step among its forms), IMA-ADPCM, the GameCube's AFC, PCM8 and PCM16, and the decoding of a sound archive's waves |
| `ntdlib::rip_archive` | Ripping the Wii U's and the Switch's sound archives into PSF sets: the sounds to rip, the data they require, the groups kept outside an archive, the timing helpers, and the libraries and minis that a rip writes. Built when xsflib has `xsflib::psf` |
| `ntdlib::nwsnd` | The tables of NintendoWare's sound libraries, and a model of their sequence player and channels for the Wii, the 3DS, the Wii U and the Switch, and of the Wii U's and the Switch's voices |
| `ntdlib::nwsnd_commands` | A decoder and an encoder of the bytecode of NintendoWare's sequences in the form of `ntdlib::commands`, and the units of each console's values. The encoder gives back the bytes that the decoder read |
| `ntdlib::nx` | A model of the Switch's audio renderer (`nn::audio`) with its I3DL2 reverb, and archive mode's model of `nn::atk` on it: an engine that plays a sound archive's sequences as the Switch does, without the game's code |
| `ntdlib::cafe` | A model of the Wii U's AX and of the effects that its system library runs on AX's aux buses (AXFX), and archive mode's model of `nw::snd` on them: an engine that plays a sound archive's sequences as the Wii U does, without the game's code. The renderer follows the DSP microcode that the system's sound library uploads |
| `ntdlib::sequence_export` | Conversion of NintendoWare's sound archives and of JAudio's sound data to MIDI, SoundFont 2 and SFZ files, through seqlib |

`ntdlib::testing` holds builders of small synthetic sound archives, JAudio2 files and DOL executables for unit tests,
the length analysis's cases for the tests of rip tools, and checks of the files that `ntdlib::sequence_export`
writes.

`tools/sequence_export/filter_tables.py` derives the filter tables in `src/sequence_export/filter_tables_<console>.cpp`
from each console's reference game. For the Wii U and the Switch, it reads the game's tables in
`include/ntdlib/nwsnd/tables.h`. For the 3DS and the Wii, the user supplies the game's executable. The script requires
Python 3 and numpy.
`tools/build_ppc_driver.py` builds a rip format's game-mode driver for the GameCube or the Wii, and writes it as a C++
header. It requires a clang with the PowerPC target, such as LLVM's releases or Homebrew's llvm. Apple's clang lacks
that target. A driver can include the shared sources in `drivers/`. `drivers/jaudio/z2.h` holds the start-up of Z2,
the sound layer that Twilight Princess puts on JAudio2. A driver supplies the console's steps, and
`include/ntdlib/flipper/z2_params.h` gives the host's side of its parameters.
`tools/write_cp932_table.py` writes `src/disc/cp932.h`, the table of Shift-JIS's double-byte characters in code page
932, from Unicode's CP932.TXT. The user downloads that file. Building ntdlib requires none of these scripts.

## Building

ntdlib requires CMake 3.20 or later, a C++20 compiler and xsflib. xsflib is a Git submodule at `externals/xsflib`.
Clone with `git clone --recurse-submodules`, or run `git submodule update --init --recursive` in an existing clone.
The xsflib submodule holds seqlib as a submodule too.

ntdlib supports macOS on Apple Silicon and Windows x64 built with clang-cl. Other platforms and compilers aren't
supported.

- **Windows x64**: Visual Studio 2022 with the C++ workload and its "C++ Clang tools for Windows" component. Configure
  with `cmake -S . -B build -A x64 -T ClangCL` in place of the first command below.
- **macOS on Apple Silicon**: Xcode's command-line tools (`xcode-select --install`) and CMake (from cmake.org or
  Homebrew).

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

The compressed containers of disc images require zlib, Zstandard, liblzma and bzip2. xsflib finds them. An installed
copy of each library is used if CMake finds one. Otherwise CMake downloads and builds it.

Output is meant to be the same on both platforms. ntdlib builds with xsflib's flags. They turn off the contraction of
floating-point operations into fused multiply-adds. A project that includes ntdlib's headers should build its sources
the same way.

## Options

| Option | Default | Effect |
|---|---|---|
| `NTDLIB_DISC` | on when built alone | `ntdlib::disc_containers`, the compressed containers of disc images. Requires zlib, Zstandard, liblzma and bzip2. |
| `NTDLIB_SEQUENCE_EXPORT` | off | `ntdlib::sequence_export`, the conversion to MIDI, SoundFont 2 and SFZ. Requires seqlib. |
| `NTDLIB_TESTS` | on when built alone | The tests, registered with CTest. |
| `NTDLIB_INSTALL` | off | Installs the headers, the libraries and the CMake package. A build of ntdlib alone installs its copy of xsflib too. Requires installed copies of the libraries above. |

## Using ntdlib

Each module is a static library with an alias, such as `ntdlib::sar`. A project links the modules it uses. The
helpers for unit tests are `ntdlib::testing`.

A project that uses ntdlib adds xsflib first. ntdlib then uses that copy of xsflib. The copy in ntdlib's
submodule goes unused. A project that turns on `NTDLIB_DISC` also sets `XSFLIB_DISC_LIBRARIES` before it adds xsflib.
A build of ntdlib alone sets it for its submodule.

As Git submodules:

```cmake
set(XSFLIB_DISC_LIBRARIES ON)
add_subdirectory(externals/xsflib EXCLUDE_FROM_ALL)
add_subdirectory(externals/ntdlib EXCLUDE_FROM_ALL)
target_link_libraries(my_program PRIVATE ntdlib::sar)
```

With FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(xsflib GIT_REPOSITORY <xsflib's URL> GIT_TAG <a release tag>)
FetchContent_Declare(ntdlib GIT_REPOSITORY <ntdlib's URL> GIT_TAG <a release tag>)
FetchContent_MakeAvailable(xsflib ntdlib)
target_link_libraries(my_program PRIVATE ntdlib::sar)
```

From installed copies (`cmake --install build` after configuring each library with its `_INSTALL` option), with xsflib
installed first:

```cmake
find_package(ntdlib 1.0 REQUIRED)
target_link_libraries(my_program PRIVATE ntdlib::sar)
```

ntdlib's package finds xsflib, and seqlib with `NTDLIB_SEQUENCE_EXPORT`. A project that uses
`ntdlib::sequence_export` adds seqlib first. Then it sets `NTDLIB_SEQUENCE_EXPORT` and adds xsflib and ntdlib.

## Licence

ntdlib is under the MIT licence (`LICENSE`). One file, `src/disc/cp932.h`, contains data from Unicode and is under
the Unicode License v3. `THIRD-PARTY-NOTICES.md` gives that licence's text. It also lists the libraries that ntdlib
uses and their licences, and the documentation that parts of ntdlib were written from.
