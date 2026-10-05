HEADROOM
========

Headroom is a mixer for Linux in the spirit of Elgato's Wave Link. Your game,
music, chat and system sound each get their own channel and fader, your
microphone gets an effects chain (noise removal, EQ, compressor and more, plus
your own VST3, LV2 and LADSPA plugins), and apps like Discord and OBS find the
right mic and the right mix. It is made for Elgato Wave microphones and works
with any other microphone too. PipeWire does the routing underneath; Headroom
is the window on top.

Download and info: https://critz.dev/headroom


BUILD FROM SOURCE
-----------------
You need a C++20 compiler, CMake 3.22 or newer and a network connection the
first time (CMake fetches JUCE, RNNoise and DeepFilterNet, and the two
DeepFilterNet models). On Fedora:

    sudo dnf install gcc-c++ cmake ninja-build git pkgconf-pkg-config \
        freetype-devel fontconfig-devel libX11-devel libXext-devel \
        libXcomposite-devel libXcursor-devel libXinerama-devel \
        libXrandr-devel libXrender-devel libXi-devel ladspa-devel \
        pipewire-devel rust cargo

Then, in the source folder:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

The program is build/headroom. Rust is needed for DeepFilterNet, the noise
removal behind the Fast and Best settings. To build without it, add
-DHEADROOM_DFN=OFF to the first command; noise removal then uses RNNoise only.


LICENCE
-------
Headroom is licensed under the GNU AGPL-3.0 (see LICENSE). Notices for the
third-party software, fonts, icons and test data it uses are in
THIRD-PARTY-NOTICES.txt.

Not affiliated with Elgato. Elgato, Wave and Wave Link are trademarks of their
owner.


Headroom is a personal project. I built it for a friend who uses it every day, and I'm sharing it in case it helps someone else. There's no promise it'll stay up to date or keep working. Issues are welcome, no guarantees.
