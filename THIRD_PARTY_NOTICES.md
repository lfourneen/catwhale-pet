# Third-party notices

Catwhale Pet's source and included artwork are provided under the [MIT License](LICENSE). The source repository references Qt and FFmpeg as external dependencies. Their own license terms apply to those libraries.

## Qt 6

Catwhale Pet dynamically links Qt Core, Gui, Network, Widgets, and Quick, and loads the Qt Wayland client platform plugin for native Wayland sessions. These Qt libraries and the Wayland client plugin are available under LGPL-3.0-only as an open-source licensing option. Qt is developed by The Qt Company and its contributors. [Qt licensing](https://doc.qt.io/qt-6/licensing.html) · [Qt Wayland client source](https://github.com/qt/qtbase/tree/dev/src/plugins/platforms/wayland)

## FFmpeg

Catwhale Pet dynamically links FFmpeg libraries. FFmpeg is available under LGPL-2.1-or-later by default. Enabling GPL components changes the license that applies to the resulting FFmpeg build. The Nix flake uses nixpkgs’ `ffmpeg-headless`. Check the license reported by the build used for release, since the headless variant can also include GPL components. [FFmpeg licensing](https://ffmpeg.org/legal.html)

## Binary releases

A binary release needs notices and license texts for the components it distributes. When distributing Qt under LGPLv3, include the LGPLv3 and GPLv3 license texts, provide access to the corresponding Qt source, including any patches, as required by the license, and allow use of an interface-compatible replacement shared library or provide the applicable relinking materials. For FFmpeg, check the build flags and the resulting license before distributing binaries. [Qt's LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations) · [LGPLv3, section 4](https://github.com/qt/qtbase/blob/dev/LICENSES/LGPL-3.0-only.txt) · [FFmpeg licensing](https://ffmpeg.org/legal.html)
