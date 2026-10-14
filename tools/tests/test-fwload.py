#!/usr/bin/env python3
"""CPU and packaging contracts for the cross-platform firmware loader."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest


def find_repository_root():
    for parent in Path(__file__).resolve().parents:
        if (parent / "installer" / "unix" / "ps5cam_fwload.py").is_file():
            return parent
    raise RuntimeError("cannot find the PS5CameraDriver repository root")


ROOT = find_repository_root()
LOADER = ROOT / "installer" / "unix" / "ps5cam_fwload.py"
MACOS = ROOT / "installer" / "macos"
WORKFLOW = ROOT / "public" / ".github" / "workflows" / "build.yml"
if not WORKFLOW.is_file():
    WORKFLOW = ROOT / ".github" / "workflows" / "build.yml"


class USBError(Exception):
    pass


def load_firmware_module():
    usb = types.ModuleType("usb")
    usb_backend = types.ModuleType("usb.backend")
    usb_libusb = types.ModuleType("usb.backend.libusb1")
    usb_core = types.ModuleType("usb.core")
    usb_util = types.ModuleType("usb.util")
    usb_core.USBError = USBError
    usb_core.find = lambda **_kwargs: []
    usb_util.dispose_resources = lambda _dev: None
    usb_libusb.get_backend = lambda **_kwargs: None
    usb.backend = usb_backend
    usb.core = usb_core
    usb.util = usb_util
    usb_backend.libusb1 = usb_libusb
    modules = {
        "usb": usb,
        "usb.backend": usb_backend,
        "usb.backend.libusb1": usb_libusb,
        "usb.core": usb_core,
        "usb.util": usb_util,
    }
    old = {name: sys.modules.get(name) for name in modules}
    sys.modules.update(modules)
    try:
        spec = importlib.util.spec_from_file_location("ps5cam_fwload_tested", LOADER)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        return module
    finally:
        for name, previous in old.items():
            if previous is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = previous


FW = load_firmware_module()


def write_synthetic_fixture(directory: Path):
    """Create deterministic non-Sony input and patch files for installer smoke tests."""
    directory.mkdir(parents=True, exist_ok=True)
    original = bytes((i % 251 for i in range(4096)))
    result = bytearray(original)
    result[17:20] = b"\xaa\xbb\xcc"
    (directory / "sony-firmware.bin").write_bytes(original)
    (directory / "ps5cam-firmware.json").write_text(json.dumps({
        "description": "Synthetic CI fixture; not Sony firmware",
        "original": {"size": len(original), "sha256": hashlib.sha256(original).hexdigest()},
        "sources": [],
        "result": {"size": len(result), "sha256": hashlib.sha256(result).hexdigest()},
        "runs": [[17, "aabbcc"]],
    }), encoding="utf-8")


def check_dependency_lock(architecture: str, directory: Path):
    """Resolve the complete hash-locked dependency graph with a real Python 3.9 pip."""
    if sys.version_info[:2] != (3, 9):
        raise RuntimeError(f"dependency closure must be checked with Python 3.9, got {sys.version}")
    platforms = {
        "arm64": "macosx_11_0_arm64",
        "x86_64": "macosx_10_9_x86_64",
    }
    try:
        platform = platforms[architecture]
    except KeyError as error:
        raise RuntimeError(f"unsupported macOS architecture: {architecture}") from error
    venv = directory / "venv"
    wheels = directory / "wheels"
    subprocess.check_call([sys.executable, "-m", "venv", str(venv)])
    venv_python = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    subprocess.check_call([
        str(venv_python), "-m", "pip", "download",
        "--disable-pip-version-check", "--require-hashes", "--only-binary=:all:",
        "--platform", platform, "--python-version", "39", "--implementation", "cp", "--abi", "cp39",
        "--dest", str(wheels), "-r", str(MACOS / "requirements.lock"),
    ])
    names = {path.name for path in wheels.glob("*.whl")}
    expected = {
        "pyusb-1.3.1-py3-none-any.whl",
        "importlib_resources-6.5.2-py3-none-any.whl",
        "zipp-3.23.1-py3-none-any.whl",
        f"libusb_package-1.0.30.0-py3-none-macosx_{'11_0_arm64' if architecture == 'arm64' else '10_9_x86_64'}.whl",
    }
    if names != expected:
        raise RuntimeError(f"unexpected dependency closure for {architecture}: {sorted(names)}")


class FakeDevice:
    def __init__(self, short_at=None, jump_error=False):
        self.short_at = short_at
        self.jump_error = jump_error
        self.configured = False
        self.transfers = []

    def get_active_configuration(self):
        raise USBError("not configured")

    def set_configuration(self):
        self.configured = True

    def ctrl_transfer(self, request_type, request, value, index, data, timeout):
        record = (request_type, request, value, index, bytes(data), timeout)
        self.transfers.append(record)
        if self.jump_error and value == 0x2200 and index == 0x8018:
            raise USBError("device rebooted")
        if self.short_at is not None and len(self.transfers) - 1 == self.short_at:
            return max(0, len(data) - 1)
        return len(data)


class FirmwarePatchTests(unittest.TestCase):
    def make_patch(self, directory: Path, original: bytes, runs, result: bytes):
        original_path = directory / "original.bin"
        patch_path = directory / "patch.json"
        output_path = directory / "firmware.bin"
        original_path.write_bytes(original)
        patch_path.write_text(json.dumps({
            "original": {"size": len(original), "sha256": hashlib.sha256(original).hexdigest()},
            "sources": [],
            "result": {"size": len(result), "sha256": hashlib.sha256(result).hexdigest()},
            "runs": runs,
        }), encoding="utf-8")
        return patch_path, original_path, output_path

    def test_make_applies_checked_patch_without_bundled_firmware(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            original = bytes((i % 251 for i in range(4096)))
            result = bytearray(original)
            result[17:20] = b"\xaa\xbb\xcc"
            patch, source, output = self.make_patch(directory, original, [[17, "aabbcc"]], bytes(result))
            self.assertEqual(FW.make_firmware(patch, output, source), 0)
            self.assertEqual(output.read_bytes(), result)

    def test_make_rejects_patch_run_past_original_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            original = bytes(4096)
            appended = original + b"\xaa"
            patch, source, output = self.make_patch(directory, original, [[4096, "aa"]], appended)
            document = json.loads(patch.read_text(encoding="utf-8"))
            document["result"]["size"] = len(original)
            patch.write_text(json.dumps(document), encoding="utf-8")
            self.assertEqual(FW.make_firmware(patch, output, source), 1)
            self.assertFalse(output.exists())

    def test_make_rejects_result_size_contract_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            original = bytes(4096)
            patch, source, output = self.make_patch(directory, original, [], original)
            document = json.loads(patch.read_text(encoding="utf-8"))
            document["result"]["size"] += 1
            patch.write_text(json.dumps(document), encoding="utf-8")
            self.assertEqual(FW.make_firmware(patch, output, source), 1)
            self.assertFalse(output.exists())


class USBUploadTests(unittest.TestCase):
    def test_upload_configures_and_sends_addressed_512_byte_chunks_then_jump(self):
        device = FakeDevice(jump_error=True)
        image = bytes((i % 251 for i in range(65537)))
        FW.upload(device, image)
        self.assertTrue(device.configured)
        self.assertEqual(device.transfers[0], (0x40, 0, 0, 0x14, image[:512], 2000))
        self.assertEqual(device.transfers[128], (0x40, 0, 0, 0x15, image[65536:], 2000))
        self.assertEqual(device.transfers[-1], (0x40, 0, 0x2200, 0x8018, b"\x5b", 2000))

    def test_upload_rejects_short_usb_write(self):
        with self.assertRaisesRegex(USBError, "short write at 512"):
            FW.upload(FakeDevice(short_at=1), bytes(1025))


class MacOSPackagingTests(unittest.TestCase):
    def test_lock_pins_only_cross_arch_binary_dependencies_by_hash(self):
        lock = (MACOS / "requirements.lock").read_text(encoding="utf-8")
        self.assertIn("pyusb==1.3.1", lock)
        self.assertIn("bf9b754557af4717fe80c2b07cc2b923a9151f5c08d17bdb5345dac09d6a0430", lock)
        self.assertIn("libusb-package==1.0.30.0", lock)
        self.assertIn("4ad25f8d254bbbdd234446d5580b24d62d59bb0e690d8090457dac347f044437", lock)
        self.assertIn("2b98784bac3bedda7e95bb5039dd0eded84b02f36110e4d5d607375366c61090", lock)
        self.assertIn("importlib-resources==6.5.2", lock)
        self.assertIn("789cfdc3ed28c78b67a06acb8126751ced69a3d5f79c095a98298cd8a760ccec", lock)
        self.assertIn('zipp==3.23.1; python_version < "3.10"', lock)
        self.assertIn("0b3596c50a5c700c9cb40ba8d86d9f2cc4807e9bedb06bcdf7fac85633e444dc", lock)
        self.assertNotIn("sony", lock.lower())

    def test_installer_copies_lock_and_requires_hashes(self):
        script = (MACOS / "install.sh").read_text(encoding="utf-8")
        self.assertIn("requirements.lock", script)
        self.assertIn("--require-hashes", script)
        self.assertIn("--only-binary=:all:", script)
        self.assertIn("sys.version_info >= (3, 9)", script)

    def test_ci_runs_native_installer_on_apple_silicon_and_intel(self):
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("macos-15", workflow)
        self.assertIn("macos-15-intel", workflow)
        self.assertIn("sudo bash package/install.sh --bokeh off", workflow)
        self.assertIn("launchctl print system/com.ps5camera.fwload", workflow)
        self.assertIn("--check-lock", workflow)
        self.assertIn("plutil -lint", workflow)
        self.assertIn("needs: [build, linux, macos]", workflow)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--write-fixture":
        write_synthetic_fixture(Path(sys.argv[2]))
    elif len(sys.argv) == 4 and sys.argv[1] == "--check-lock":
        check_dependency_lock(sys.argv[2], Path(sys.argv[3]))
    else:
        unittest.main(verbosity=2)
