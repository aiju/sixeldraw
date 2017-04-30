Sixel support for plan9port devdraw
======================================

*Sixeldraw* is an implementation of plan9port's *devdraw*(1) that runs inside a terminal and uses sixel for drawing and DEC Locator for mouse inputs.
To use it just point `DEVDRAW` to the *sixeldraw* binary and run some *draw*(3) program, e.g.

    DEVDRAW=sixeldraw sam

If `SIXELDBG=` is set to a file then *sixeldraw* debug messages are sent there.

There is a known bug where *sixeldraw* will leave the terminal in a messy state after it exits. This is race condition between *sixeldraw* cleaning up and  *sh*(1) reading the tty settings and is hard to fix.
An easy workaround is to append `sleep 0.1` to the command.

By default *sixeldraw* maintains its own snarf buffer.
If `SNARF=1` is set, it uses the *xterm* sequences to read and write the clipboard, which may need to be enabled in the terminal emulator configuration.

*Draw*(3) operations that change the current window's size, location etc. are currently ignored.
Changing the cursor could be supported with a softcursor but isn't implemented yet.

Terminal support
-----------------

- Recent (!) *xterm* compiled with `--enable-sixel --enable-dec-locator` and with Xresources settings along the lines of

        XTerm*decTerminalID: vt340
        XTerm*numColorRegisters: 256
        XTerm*maxGraphicSize: 2560x1440

  For snarf support the setting is something like

        XTerm*disallowedWindowOps: 20,21,SetXprop

- *Mlterm* needs `QUIRKS=3` to work around two bugs: The "sixel scrolling" mode is inverted and the order of mouse buttons in the DEC locator response is wrong.
