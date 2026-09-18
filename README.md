<p align="center">
  <img src="logo.png" alt="FGG-XSense" width="820">
</p>

# FGG-XSense

**Use an Xbox controller on a jailbroken PlayStation 5.**

[![CI](https://github.com/FGGstore/FGG-XSense/actions/workflows/ci.yml/badge.svg)](https://github.com/FGGstore/FGG-XSense/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

A PS5 ignores an Xbox controller completely. Plug one in and nothing happens —
no light, no input, not even a "unsupported device" message. The console
enumerates it and then leaves it alone.

FGG-XSense is a payload that picks it up and hands it to the system as an
ordinary PlayStation pad.

```
  Xbox pad ──USB──► FGG-XSense ──virtual pad──► games and system UI
```

Nothing in the system software is patched. Nothing is installed. When the
payload exits, the virtual pad is removed and the console is exactly as it was.

---

## Install

Grab `fgg-xsense.elf` from the [latest release](https://github.com/FGGstore/FGG-XSense/releases/latest).

**With Payload Manager** — copy `fgg-xsense.elf` and `fgg-xsense.elf.json` over
FTP into:

```
/data/pldmgr/payloads/fgg-xsense/
```

then launch **fgg xsense** from the Payload Manager page.

**Without it** — send `fgg-xsense.elf` to port **9021** with any payload sender.

## Use it

1. Plug the controller into the PS5 over USB
2. Launch the payload

The guide LED lights and the controller works. That is the whole workflow.

To stop it: **hold View + Menu together for two seconds**, or unplug the
controller. It also stops on its own if the controller disappears for 30
seconds.

Your DualSense keeps working throughout — this adds a pad, it does not replace
yours.

---

## Mapping

| Xbox | PlayStation |
|---|---|
| A | ✗ Cross |
| B | ○ Circle |
| X | □ Square |
| Y | △ Triangle |
| LB / RB | L1 / R1 |
| LT / RT | L2 / R2 — analogue, plus the digital bit |
| Left stick click | L3 |
| Right stick click | R3 |
| Menu | Options |
| View | Touchpad click |
| D-pad | D-pad |
| Left / right stick | Left / right stick |
| **Guide (Xbox button)** | **nothing — see below** |

Sticks are full 16-bit range. The Y axes are inverted on the way through,
because GIP counts up as positive and PlayStation counts down.

## The PS button

**It is not mapped, and it cannot be.** Use your DualSense for it.

The PS button is intercepted by the system above the pad layer, specifically so
that no controller — real or virtual — can synthesise a home-button press. It
appears in neither the published button constants nor anywhere inside
`libScePad`.

This was not assumed. The virtual pad report was eliminated exhaustively on
hardware:

- all **16** undocumented bits of the button word, including `0x80000000`,
  which the PS4 headers name `SCE_PAD_BUTTON_INTERCEPTED`
- all **17** report bytes that `libScePad` never writes

Each was held for a second and a half with a controller in hand. None produced
any reaction whatsoever. The button is still read and acknowledged — the
controller stops talking to a host that ignores it — it simply has nowhere to
go.

---

## Supported controllers

Anything that speaks **GIP**, the Xbox wire protocol: Xbox One, Xbox Series,
and the many third-party pads built to the same standard.

Devices are matched on the GIP interface signature — class `FF`, subclass `47`,
protocol `D0` — rather than on a list of vendor IDs, so a controller nobody has
tested is still recognised. Nothing that fails to match is opened, configured or
claimed.

Developed and verified against a GameSir pad (`3537:1010`) on firmware
**11.60**.

> Xbox 360 controllers use an older, different protocol and are **not**
> supported.

---

## How it works

Two halves, both worked out by probing a console rather than by guessing.

**Reading the controller.** The PS5 enumerates a GIP pad but never configures
it, and the per-endpoint `/dev/ugenX.Y.Z` nodes do not exist on this system, so
transfers go through the `USB_FS_*` ioctls. The device needs a power-on message
before it streams input, an authentication message if it is not made by
Microsoft — without it a third-party pad powers itself down after a few seconds
— and a third message to light the guide LED.

**Publishing a virtual pad.** `libScePad`'s virtual-device API turns out to be a
thin wrapper over two ioctls on `/dev/hid`, and neither checks who is calling:

```
AddDevice     ioctl(fd, 0xC018482A, &req)
InsertData    ioctl(fd, 0x8018482C, &req)
DeleteDevice  ioctl(fd, 0x80104850, &req)
```

So a payload can register a controller without patching SceShellCore or
anything else. All eleven device types the kernel recognises were accepted.

**One trap worth recording.** The report the kernel reads is *not* `ScePadData`.
`libScePad` translates its caller's `ScePadData` into a differently shaped
buffer first — buttons land at `+0x0C`, sticks at `+0x10`. Passing a
`ScePadData` straight through looks like it works, because the ioctl validates
only its 24-byte header and never the contents, while the kernel reads the
buttons out of what was actually the orientation quaternion. A pad appears, the
system reacts to it, and not one button does anything.

## Reliability

Controllers stall. The payload expects it and recovers rather than giving up:

- unanswered GIP messages are acknowledged, so the controller keeps talking
- the initialisation is resent every 20 seconds while everything is healthy
- three seconds of silence triggers a re-initialisation
- three fruitless re-initialisations rebuild the USB connection from scratch
- only 30 seconds of genuine silence is treated as an unplug

Measured on console: **110,000 input reports delivered over 25 minutes**
without intervention.

Only one copy runs at a time. Launching it twice is easy to do by accident, and
two copies is not merely wasteful — both claim the same USB interface and each
consumes the other's transfer completions, which looks exactly like failing
hardware.

## Building

Needs Linux or WSL and the
[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk):

```sh
sudo apt install clang lld llvm make unzip    # llvm is required, and easy to miss
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make
```

Builds at `-Wall -Wextra -Werror`. No dependencies beyond the SDK; there is
nothing vendored and nothing fetched at build time.

## Troubleshooting

**Nothing happens when I launch it.** Check `/data/fgg-xsense/xsense.log` over
FTP — every run is logged there, including why it gave up.

**"another copy is already running".** Hold View+Menu for two seconds to stop
the running one, or reboot if it will not respond.

**The controller light never comes on.** Then it is not being recognised as a
GIP device. The log names every controller it finds; if yours is not listed, it
does not present the GIP interface signature.

## Licence

[GPL-3.0](LICENSE).

## Credits

Built by **FGG STORE**.

Standing on the work of [ps5-payload-dev](https://github.com/ps5-payload-dev)
for the SDK and `ftpsrv`, and on the Linux `xpad` driver, whose initialisation
packets documented what a third-party GIP controller needs in order to stay
awake.

> Not affiliated with Sony Interactive Entertainment or Microsoft. For use with
> homebrew on consoles you own.
