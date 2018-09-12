# Lock Helper
This does the following when locking your screen when using GNOME Screensaver\*:

* Disables the Ctrl+Alt+Bksp X server killing sequence if enabled
* Disables the Magic SysRq key if enabled
* Locks VT switching; Ctrl+Alt+F1 etc. will have no effect
* Mutes the default PulseAudio sink

Naturally, this is all reversed on unlock.

# Problems

* `lock_helper` runs as setuid root to lock VT switching and disable the sysrq key. **There could very well be security issues lurking in this code**
* The X server layout code...
    * It's really only been designed for a one-keyboard system. Maybe it won't mess up anything if more than one keyboard is present. I don't know.
    * Other layout options are only retrieved on the start of the program. If you make changes, like swapping Ctrl + Caps Lock using the keyboard tweaks section of your DE, during the runtime of this program then this program will probably revert them on unlock.
    * No attempt is made to parse funny X11 keyboard option strings. `lock_helper` assumes a plain, comma-seperated list with no spaces.
    * It assumes the terminate key is Ctrl+Alt+Bksp (which may not be a problem)

# Installation

```
cc -Wall -O2 `pkg-config --cflags --libs xkbfile x11 gio-unix-2.0 libpulse-mainloop-glib` -DXKB_BASE=\"$(pkg-config --variable xkb_base xkeyboard-config)\" lock_helper.c -o lock_helper
sudo install --group=root --mode=4755 --owner=root --strip ./lock_helper /usr/local/sbin/
```

Add `/usr/local/sbin/lock_helper` to your DE's autostart mechanism.

\* unofficial_locked_signal.patch must be applied to your GNOME Screensaver source. (Or ActiveChanged(true) can be used but it's annoying having the sound muted when the screen just blanks.)
