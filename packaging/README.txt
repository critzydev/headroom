HEADROOM
========

Headroom is a mixer for your microphone and your sound. It gives your game,
music, chat and system sounds their own faders, cleans up your voice, and makes
sure Discord always finds the right mic - also after a restart. It is made for
the Elgato Wave microphones and works with any other microphone and headphones.


INSTALL (the AppImage: Headroom-<version>-x86_64.AppImage)
----------------------------------------------------------
1. Double-click the downloaded file. If nothing happens, right-click it,
   choose Properties > Permissions and tick "Allow executing as a program"
   (or in a terminal:  chmod +x Headroom-*.AppImage  and run it).
2. Headroom opens a short setup: your microphone and headphones, your channels,
   how you want your voice to sound, and whether it starts when you log in.
   It takes about a minute, and every choice can be changed later in Settings.
3. At the end Headroom has put itself in your application menu (look for
   "Headroom") and starts quietly every time you log in.

You do not need a password or admin rights. The AppImage file you downloaded can
be deleted afterwards: Headroom keeps its own copy in ~/.local/opt/headroom.
If you keep your AppImages in a folder that a manager such as Gear Lever looks
after, setup offers to leave Headroom where it is.

To update, download the newer AppImage and double-click it: Headroom asks
whether to update the installed copy, and your settings stay as they are.

To run setup again: Settings > Setup > "Run setup again", or in a terminal:

    ~/.local/opt/headroom/Headroom.AppImage --setup


INSTALL (the tarball: headroom-<version>-x86_64.tar.gz)
-------------------------------------------------------
1. Right-click the downloaded file and choose "Extract here".
2. Open the new "headroom" folder and double-click "install.sh".
   If it asks, choose "Run" or "Run in terminal".
   (Prefer a terminal? Open one in that folder and type:  ./install.sh )
3. Wait a few seconds. Headroom opens by itself, and from now on it starts
   quietly every time you log in.


IN DISCORD (one time)
---------------------
Open Discord > Settings > Voice & Video, then:

  - Input Device:   Headroom Mic
  - Output Device:  Headroom Chat

Also switch OFF "Noise Suppression", "Echo Cancellation" and "Automatic Gain
Control" there. Headroom already does that job, and doing it twice sounds worse.


NOISE REMOVAL
-------------
Click the mic's FX button, then "Noise Removal". Best is the usual choice: it is
the cleanest and needs less of your computer than Fast, which reacts 20 ms
sooner; Light uses the least of your computer. "Hear what's removed" lets you
listen to what is being taken out of your voice while you hold the button.


IF PEOPLE CANNOT HEAR YOU
-------------------------
Headroom shows a message at the top of its window when it notices a problem
with your mic, usually with a button that fixes it.

  - Red ring on the Wave:3?  Tap the top of the microphone to unmute it.
  - Message says "No sound from your mic"?  Press "Try again" (or "Fix my mic"
    in Settings). This takes a few seconds.

Headphones plugged into your Elgato Wave are where Headroom plays your sound.


CLIPGUARD AND SAFE GAIN (OPTIONAL)
----------------------------------
In Settings > Microphone (only shown when the Wave:3 is your mic):

  - ClipGuard: the Wave:3's own switch that stops distortion when you get loud.
    The first time, press "Allow hardware controls" (setup offers it too).
    Headroom asks for your password once; it lets you, at this computer, control
    the Wave:3 (nothing else). "Re-apply at startup" puts ClipGuard back after
    the mic lost power.
  - Safe gain: keeps the mic's own gain lower, and Headroom adds the difference
    back, so you sound the same but loud moments are less likely to distort.
    It needs the same permission ("Allow hardware controls"), because it checks
    what the mic really did.


UNINSTALL
---------
AppImage: Settings > Setup > "Uninstall Headroom". Headroom quits and removes
itself from your menu and from starting at login. You choose whether your
settings are deleted too. In a terminal:

    ~/.local/opt/headroom/Headroom.AppImage --uninstall-unattended

(add --delete-settings to delete the settings as well).

Tarball: open the folder ~/.local/opt/headroom (press Ctrl+H in the file manager
to see hidden folders) and double-click "uninstall.sh", or run this in a terminal:

    ~/.local/opt/headroom/uninstall.sh

Your Headroom settings are kept, in case you install it again. To delete those
as well, run it with  --purge.

If you used "Allow hardware controls", uninstalling also asks for your password
once, to remove that permission again. If you cancel, nothing breaks; to remove it
yourself later:

    sudo rm /etc/udev/rules.d/70-headroom-wave.rules && sudo udevadm control --reload


IF SOMETHING IS WRONG
---------------------
Headroom writes a log file. If you report a problem, please attach the end of it:

    ~/.local/state/headroom/headroom.log

(Settings > About > "Open log folder" opens it.)

Issues are welcome at https://github.com/critzydev/headroom/issues


MORE
----
Download and info: https://critz.dev/headroom
Source code: https://github.com/critzydev/headroom
Headroom is licensed under the GNU AGPL-3.0. The licence and the notices for
the software and fonts it is built with are in the source repository above.

Not affiliated with Elgato. Elgato, Wave and Wave Link are trademarks of their
owner.
