# Third-party notices

ntdlib is under the MIT licence (`LICENSE`). One file is the exception: `src/disc/cp932.h` contains data from Unicode
and is under the Unicode License v3 (see "Data from Unicode" below). The libraries that ntdlib uses have the following
licences. None of their code is stored here.

| Component | Licence | Use in ntdlib |
|---|---|---|
| xsflib | MIT | Every module: byte order, bounds-checked reads, SHA-1 and memory images (`xsflib::common`), the checks of the tests (`xsflib::testing`), and the libraries of the disc images (`XSFLIB_DISC_LIBRARIES`). The project that uses ntdlib supplies it. A build of ntdlib alone takes it from the submodule `externals/xsflib`. |
| [zlib](https://zlib.net), Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler | zlib | GCZ disc images (`NTDLIB_DISC`), through xsflib's `xsflib::zlib`. The installed copy is used if there is one. Otherwise v1.3.1 is downloaded when configuring. |
| [Zstandard](https://github.com/facebook/zstd), Copyright (c) Meta Platforms, Inc. and affiliates | BSD-3-Clause | RVZ disc images (`NTDLIB_DISC`), through `xsflib::zstd`. The installed copy is used if there is one. Otherwise v1.5.7 is downloaded when configuring. |
| [liblzma](https://tukaani.org/xz/) from XZ Utils | 0BSD | LZMA and LZMA2 in WIA and RVZ disc images (`NTDLIB_DISC`), through `xsflib::lzma`. The installed copy is used if there is one. Otherwise v5.8.1 is downloaded when configuring. |
| [bzip2](https://sourceware.org/bzip2/), Copyright (C) 1996-2019 Julian R Seward | bzip2 (BSD-style) | bzip2 in WIA and RVZ disc images (`NTDLIB_DISC`), through `xsflib::bzip2`. The installed copy is used if there is one. Otherwise 1.0.8 is downloaded when configuring. |
| seqlib | MIT | MIDI, SoundFont 2 and SFZ files in `ntdlib::sequence_export` (`NTDLIB_SEQUENCE_EXPORT`). The project that uses ntdlib supplies it. |

Binary distributions must include ntdlib's MIT notice and xsflib's MIT notice. They must also include the Unicode
License v3's notice when `ntdlib::disc` is linked in, and the Zstandard notice when Zstandard is linked in. Shipping
ntdlib's `LICENSE`, xsflib's `LICENSE` and this file does that. The licence texts are at the end of this file. The
licences of zlib, liblzma and bzip2 require no notice in binaries. A program that links `ntdlib::sequence_export`
also includes seqlib's MIT notice.

## Data from Unicode

`src/disc/cp932.h` maps the double-byte characters of code page 932, Windows' Shift-JIS, to Unicode, by their codes.
`ntdlib::disc` decodes the Shift-JIS text of GameCube and Wii discs with it: the names in their file systems, and the
titles of Japanese discs. `tools/write_cp932_table.py` derives the table from CP932.TXT, Microsoft's table of code page
932, table version 2.01, as Unicode publishes it
(https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP932.TXT). ntdlib doesn't store that file. Unicode's
terms of use (https://www.unicode.org/copyright.html) put the data files under https://www.unicode.org/Public/ under
the Unicode License v3 (SPDX `Unicode-3.0`). The licence permits anyone to use, copy, modify, publish and distribute
the data. Its copyright and permission notice must appear with all copies, or in the documentation that goes with
them. The notice is at the end of this file. `src/disc/cp932.h` has the SPDX identifier `Unicode-3.0`.

## Reference material (no code taken)

- [Dolphin](https://github.com/dolphin-emu/dolphin) (GPL-2.0-or-later) is the reference for how the GameCube's and the
  Wii's DSP behaves (`ntdlib::dsp`). No code is taken from it. The DSP was written from a description of that behaviour.
  The writer hadn't read Dolphin's source for that part. Dolphin's documentation of the WIA and RVZ formats
  (`docs/WiaAndRvz.md`) describes those formats and the generator of the padding data on GameCube and Wii discs
  (`ntdlib::disc`).
- [yuzu](https://github.com/yuzu-emu/yuzu) (GPL-3.0) and [Ryujinx](https://github.com/Ryujinx/Ryujinx) (MIT) are
  references for how the Switch's audio renderer behaves (`ntdlib::nx`). No code is taken from them. The model of the
  renderer was written from a description of their behaviour and from a study of the renderer's build for the CPU that
  games' `sdk` modules hold. The writer hadn't read their source for that part.
- [Wiimm's ISO Tools](https://wit.wiimm.de) document the WDF and CISO formats.
- YAGCD (Yet Another GameCube Documentation) documents the GameCube's hardware and disc format. WiiBrew documents the
  Wii disc's layout, and the TMD, U8 archives, banner and NAND Boot Program of the Wii's titles. GBATEK documents the CX
  library's LZ77 and Huffman formats.
- Microsoft's table of code page 1252 (Windows-1252) gives the 27 characters at 0x80 to 0x9F that `ntdlib::disc`
  decodes in the titles of discs that aren't Japanese. Unicode mirrors the table as
  https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP1252.TXT.

## Console data

Nothing in ntdlib licenses game data. Rips contain a game's code or sound archive. These belong to their copyright
holders.

`include/ntdlib/nwsnd/tables.h` holds the numeric tables of NintendoWare's sound libraries: powers of two, decibel
levels, envelope attack multipliers, pan curves, low-pass cutoffs, biquad filter coefficients and sines. The models use
the exact values to reproduce the consoles' output. `src/sequence_export/filter_tables_*.cpp` hold each console's
filters fitted to SoundFont 2 and SFZ settings. `src/nx/audio/tables.h` holds the Switch's audio renderer's tables:
the coefficients of its sample-rate converters, and the I3DL2 reverb's delays, early reflections, powers of ten and
sines. The model uses the exact values to reproduce the console's output.

`include/ntdlib/dsp/coef_rom.h` holds the DSP's coefficient ROM. Every GameCube and Wii has the same ROM. The ROM holds
4-tap filters: 128 phases for each of three cutoffs. DSP programs that resample, such as the SDK's AX, read them. The
DSP uses the exact values to reproduce the console's output.

## Licence texts

### Zstandard

```
BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### zlib

```
Copyright notice:

 (C) 1995-2022 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```

### liblzma (0BSD)

```
Permission to use, copy, modify, and/or distribute this
software for any purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

### bzip2

```

--------------------------------------------------------------------------

This program, "bzip2", the associated library "libbzip2", and all
documentation, are copyright (C) 1996-2019 Julian R Seward.  All
rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. The origin of this software must not be misrepresented; you must 
   not claim that you wrote the original software.  If you use this 
   software in a product, an acknowledgment in the product 
   documentation would be appreciated but is not required.

3. Altered source versions must be plainly marked as such, and must
   not be misrepresented as being the original software.

4. The name of the author may not be used to endorse or promote 
   products derived from this software without specific prior written 
   permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Julian Seward, jseward@acm.org
bzip2/libbzip2 version 1.0.8 of 13 July 2019

--------------------------------------------------------------------------
```

### Unicode License v3

```
UNICODE LICENSE V3

COPYRIGHT AND PERMISSION NOTICE

Copyright © 1991-2026 Unicode, Inc.

NOTICE TO USER: Carefully read the following legal agreement. BY
DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR
SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE
TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.

Permission is hereby granted, free of charge, to any person obtaining a
copy of data files and any associated documentation (the "Data Files") or
software and any associated documentation (the "Software") to deal in the
Data Files or Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, and/or sell
copies of the Data Files or Software, and to permit persons to whom the
Data Files or Software are furnished to do so, provided that either (a)
this copyright and permission notice appear with all copies of the Data
Files or Software, or (b) this copyright and permission notice appear in
associated Documentation.

THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
THIRD PARTY RIGHTS.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,
OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA
FILES OR SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall
not be used in advertising or otherwise to promote the sale, use or other
dealings in these Data Files or Software without prior written
authorization of the copyright holder.
```
