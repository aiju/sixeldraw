Sixel support for plan9port devdraw
======================================

*Sixeldraw* is an implementation of plan9port's *devdraw*(1) that runs inside a terminal and uses sixel for drawing and DEC Locator for mouse inputs.
To use it just point `DEVDRAW` to the *sixeldraw* binary and run some *draw*(3) program, e.g.

    DEVDRAW=sixeldraw sam

If `SIXELDBG=` is set to a file then sixeldraw debug messages are sent there.

Since clipboard sequences have been disabled in *xterm* for "security reasons", *sixeldraw* currently keeps its own snarf buffer.
Similarly, resizing and other window operations are not supported, either.
Changing the cursor could be supported with a softcursor but isn't implemented yet.

Terminal support
-----------------

- Recent (!) *xterm* compiled with `--enable-sixel` with Xresources settings along the lines of

    XTerm*decTerminalID: vt340
    XTerm*numColorRegisters: 256
    XTerm*maxGraphicSize: 2560x1440

- *Mlterm* is *not* supported out of the box because of stupid bugs on their part: The "sixel scrolling" mode is inverted and the order of mouse buttons in the DEC locator response is wrong.
