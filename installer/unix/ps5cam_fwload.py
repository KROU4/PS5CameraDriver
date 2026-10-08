#!/usr/bin/env python3
"""PS5 HD Camera firmware tool.

  make PATCH.json OUT [--original FILE]   build the camera firmware: Sony's original image (a local
                                          copy, or downloaded from the sources in the patch) with our
                                          changes applied; both hashes are checked
  load FIRMWARE [--wait S] [--watch]      upload it to cameras in boot mode (USB 05A9:0580); they
                                          come back as 05A9:058C, a standard UVC webcam

load exits 0 when every camera found was loaded (or, without --wait, when none was found), 1 when a
camera kept failing or --wait ran out without any camera, 2 on bad arguments. Same boot protocol as
the Windows service (src/common/firmware.cpp).
"""
import argparse
import hashlib
import json
import sys
import time
import urllib.request

import usb.backend.libusb1
import usb.core
import usb.util

VID = 0x05A9
PID_BOOT = 0x0580
CHUNK = 512           # bytes per vendor request
BASE_INDEX = 0x14     # wIndex = 0x14 + (load address >> 16): the image loads at 0x00140000
TIMEOUT_MS = 2000
MAX_TRIES = 5         # per plug-in; a camera that is plugged in again gets a fresh count


def log(text):
    print(time.strftime("%H:%M:%S"), text, file=sys.stderr, flush=True)


def make_firmware(patch_path, out_path, original_path=None):
    patch = json.load(open(patch_path))
    want = patch["original"]
    candidates = [("file", original_path)] if original_path else [("url", u) for u in patch["sources"]]
    original = None
    for kind, where in candidates:
        try:
            if kind == "file":
                data = open(where, "rb").read()
            else:
                with urllib.request.urlopen(where, timeout=30) as r:
                    data = r.read()
        except OSError as e:
            log(f"cannot get the original firmware from {where}: {e}")
            continue
        if len(data) == want["size"] and hashlib.sha256(data).hexdigest() == want["sha256"]:
            original = bytearray(data)
            log(f"original firmware: {where}")
            break
        log(f"{where} is not Sony's original firmware 21.01-03.20.00.04 (size or hash differ)")
    if original is None:
        return 1
    for offset, new in patch["runs"]:
        chunk = bytes.fromhex(new)
        original[offset:offset + len(chunk)] = chunk
    if hashlib.sha256(original).hexdigest() != patch["result"]["sha256"]:
        log("the patched firmware has an unexpected hash")
        return 1
    with open(out_path, "wb") as f:
        f.write(original)
    log(f"firmware written to {out_path}")
    return 0


def backend():
    # libusb-package (pip) bundles libusb for macOS and Windows; Linux uses the system libusb.
    try:
        import libusb_package
        return usb.backend.libusb1.get_backend(find_library=libusb_package.find_library)
    except ImportError:
        return None


def is_boot_rom(dev):
    # Developer (lab) builds of the firmware keep PID 0580 but expose the video interfaces.
    try:
        return dev[0].bNumInterfaces < 2
    except (usb.core.USBError, IndexError):
        return True


def upload(dev, image):
    # Linux and Windows (WinUSB) configure the device; macOS leaves a device without a matching
    # driver unconfigured, where vendor requests are not guaranteed to work.
    try:
        dev.get_active_configuration()
    except usb.core.USBError:
        dev.set_configuration()
    address = 0
    for pos in range(0, len(image), CHUNK):
        chunk = image[pos:pos + CHUNK]
        sent = dev.ctrl_transfer(0x40, 0x00, address & 0xFFFF, BASE_INDEX + (address >> 16), chunk, TIMEOUT_MS)
        if sent != len(chunk):
            raise usb.core.USBError(f"short write at {pos}: {sent} of {len(chunk)} bytes")
        address += len(chunk)
    try:
        # Jump into the image: 0x5B to the OV580 run control register 0x80182200.
        dev.ctrl_transfer(0x40, 0x00, 0x2200, 0x8018, b"\x5b", TIMEOUT_MS)
    except usb.core.USBError:
        pass  # the camera may drop off the bus before acknowledging the jump


def run(image, be, wait, watch):
    # Cameras are tracked per (bus, address): one that drops off (after the jump, or unplugged
    # mid-upload) is forgotten, so a replug is picked up again even at the same address.
    done, failures = set(), {}
    loaded = 0
    lab_noted = False
    deadline = time.monotonic() + wait
    while True:
        present = list(usb.core.find(find_all=True, idVendor=VID, idProduct=PID_BOOT, backend=be))
        keys = {(d.bus, d.address) for d in present}
        done &= keys
        failures = {k: v for k, v in failures.items() if k in keys}
        if present and not any(is_boot_rom(d) for d in present) and not lab_noted:
            log("only cameras running a lab firmware build are connected; nothing to load")
            lab_noted = True
        for dev in present:
            key = (dev.bus, dev.address)
            if not is_boot_rom(dev) or key in done or failures.get(key, 0) >= MAX_TRIES:
                continue
            where = f"bus {dev.bus} address {dev.address}"
            try:
                t0 = time.monotonic()
                upload(dev, image)
                done.add(key)
                loaded += 1
                log(f"firmware uploaded to the camera at {where} ({len(image)} bytes, "
                    f"{(time.monotonic() - t0) * 1000:.0f} ms)")
            except usb.core.USBError as e:
                failures[key] = failures.get(key, 0) + 1
                log(f"upload attempt {failures[key]} to {where} failed: {e}")
                if failures[key] == MAX_TRIES:
                    log(f"giving up on the camera at {where} until it is plugged in again")
            finally:
                usb.util.dispose_resources(dev)
        # Look again: another camera may have been plugged in during the upload (systemd merges its
        # start request into this still running service, so it would wait for the next plug-in).
        pending = any(is_boot_rom(d) and (d.bus, d.address) not in done and failures.get((d.bus, d.address), 0) < MAX_TRIES
                      for d in usb.core.find(find_all=True, idVendor=VID, idProduct=PID_BOOT, backend=be))
        if watch:
            time.sleep(0.4 if pending else 1.0)
            continue
        if not pending and (loaded or time.monotonic() >= deadline):
            break
        if time.monotonic() >= deadline:
            break
        time.sleep(0.4)
    if any(v >= MAX_TRIES for v in failures.values()):
        return 1
    if not loaded and not lab_noted:
        log("no PS5 camera in boot mode found")
        return 1 if wait > 0 else 0
    return 0


def main():
    parser = argparse.ArgumentParser(description="PS5 HD Camera firmware tool.")
    sub = parser.add_subparsers(dest="command", required=True)
    make = sub.add_parser("make", help="build firmware.bin from Sony's original and the patch")
    make.add_argument("patch", help="ps5cam-firmware.json")
    make.add_argument("out", help="firmware file to write")
    make.add_argument("--original", help="local copy of Sony's original firmware (default: download it)")
    load = sub.add_parser("load", help="upload the firmware to cameras in boot mode")
    load.add_argument("firmware", help="firmware image (firmware.bin)")
    load.add_argument("--wait", type=float, default=0.0, metavar="SECONDS",
                      help="wait up to SECONDS for a boot-mode camera (default: only cameras present now)")
    load.add_argument("--watch", action="store_true",
                      help="keep running and load every boot-mode camera that appears (macOS daemon)")
    args = parser.parse_args()
    if args.command == "make":
        return make_firmware(args.patch, args.out, args.original)
    try:
        image = open(args.firmware, "rb").read()
    except OSError as e:
        log(f"cannot read {args.firmware}: {e}")
        return 2
    if not 4096 <= len(image) <= 0x40000:
        log(f"{args.firmware}: unexpected firmware size {len(image)}")
        return 2
    if args.watch:
        log("watching for PS5 cameras in boot mode")
    return run(image, backend(), max(args.wait, 0.0), args.watch)


if __name__ == "__main__":
    sys.exit(main())
