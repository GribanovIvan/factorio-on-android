# Third-party notices

This project uses, incorporates, or refers to the projects listed below. The
licenses of those projects apply to their respective code; they do not change
the license of unrelated project code.

## Panfrost Android driver

The Panfrost build downloads and builds
[`JimVulkan/mali-panfrost`](https://github.com/JimVulkan/mali-panfrost) at commit
[`8af067e0a48a977334838c97f4a992bbfa100f69`](https://github.com/JimVulkan/mali-panfrost/tree/8af067e0a48a977334838c97f4a992bbfa100f69).
Its Mesa-derived driver libraries are included in the Panfrost APK variant.

The fork contains Mesa code and code under additional licenses. Its generated
`android/NOTICE.txt` lists the components compiled into the driver libraries
and includes the corresponding license texts. It is produced in the downloaded
source tree at:

`build/dependencies/mali-panfrost-8af067e0a48a977334838c97f4a992bbfa100f69/android/NOTICE.txt`

When distributing the Panfrost APK or its driver libraries, include that
generated notice with the distribution (for example, in the APK's license
assets or a companion notices file). The build currently packages the driver
libraries; verify that the generated notice accompanies any APK you distribute.

The fork identifies its changes as MIT-licensed and its Mesa subset as mostly
MIT, while retaining per-file licenses. The generated notice also covers
components such as libdrm, zlib, xxHash, SoftFloat, BLAKE3, and C11 support.
See the fork's [`LICENSE`](https://github.com/JimVulkan/mali-panfrost/blob/8af067e0a48a977334838c97f4a992bbfa100f69/LICENSE),
[`THIRD_PARTY_NOTICES.md`](https://github.com/JimVulkan/mali-panfrost/blob/8af067e0a48a977334838c97f4a992bbfa100f69/THIRD_PARTY_NOTICES.md),
and [`android/NOTICE.txt`](https://github.com/JimVulkan/mali-panfrost/blob/8af067e0a48a977334838c97f4a992bbfa100f69/android/NOTICE.txt)
for its complete notices.

Mesa's licensing is per-file; see the [Mesa license overview](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/docs/license.rst)
and the SPDX/license notices in individual source files. No separate
Adreno-specific Mesa source file was identified in this project's source tree.
If Adreno code is copied into the project, add its source file, copyright, and
license here after checking that file's license.

## SDL3

The Android APKs include `libSDL3.so`, built from SDL 3.4.14. This build applies
the patches in `patches/sdl/` to the SDL source. SDL is distributed under the
zlib license. Its copyright and license notice are:

```text
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

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
```

Source: [SDL release 3.4.14](https://github.com/libsdl-org/SDL/tree/release-3.4.14)
and its [`LICENSE.txt`](https://github.com/libsdl-org/SDL/blob/release-3.4.14/LICENSE.txt).

## detex texture decoding reference

`native/glcompat/src/etc2.c` uses modifier tables copied verbatim from
[`hglm/detex`](https://github.com/hglm/detex), including the ETC/EAC tables.
The detex license is ISC:

```text
Copyright (c) 2015 Harm Hanemaaijer <fgenfb@yahoo.com>

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

## etcpak reference

[`wolfpld/etcpak`](https://github.com/wolfpld/etcpak) Was used as a second reference.
The upstream repository's `LICENSE.txt` is BSD-3-Clause; its
notice is recorded here in case that reference is useful to readers:

```text
etcpak, an extremely fast ETC compression utility (https://github.com/wolfpld/etcpak)

Copyright (c) 2013-2026, Bartosz Taudul <wolf@nereid.pl>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```


## Factorio

Factorio is proprietary software. This project does not claim ownership of the
game or grant permission to redistribute Factorio files. Users must obtain the
game through an authorized source and comply with the applicable terms.
