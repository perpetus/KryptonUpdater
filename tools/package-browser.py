"""Stage a Chromium browser distribution and its verified offline payload."""

import argparse
import configparser
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess


def sign(path, key_path=None):
    command = ["pwsh", "-NoProfile", "-File", str(Path(__file__).with_name("release-signing.ps1")),
               "sign", "-InputFile", str(path)]
    if key_path:
        command += ["-KeyPath", str(key_path)]
    subprocess.run(command, check=True)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def file_version(path, *, product=False):
    version = ctypes.WinDLL("version", use_last_error=True)
    size = version.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        raise ctypes.WinError(ctypes.get_last_error())
    data = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(path), 0, size, data):
        raise ctypes.WinError(ctypes.get_last_error())
    block, length = ctypes.c_void_p(), wintypes.UINT()
    if not version.VerQueryValueW(data, "\\", ctypes.byref(block), ctypes.byref(length)):
        raise ctypes.WinError(ctypes.get_last_error())
    values = ctypes.cast(block, ctypes.POINTER(wintypes.DWORD))
    index = 4 if product else 2
    return ".".join(map(str, (values[index] >> 16, values[index] & 65535,
                             values[index + 1] >> 16, values[index + 1] & 65535)))


def imports(path):
    """Read PE imports without loading or running any browser executable."""
    data = path.read_bytes()
    if data[:2] != b"MZ":
        return []
    pe = struct.unpack_from("<I", data, 60)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    is_64 = struct.unpack_from("<H", data, optional)[0] == 0x20B
    directories = optional + (112 if is_64 else 96)
    sections = optional + optional_size

    def offset(rva):
        for index in range(count):
            size, address, raw_size, raw = struct.unpack_from("<IIII", data, sections + 40 * index + 8)
            if address <= rva < address + max(size, raw_size):
                return raw + rva - address
        raise ValueError(f"Unmapped PE address in {path.name}")

    names = []
    for directory, stride, name_field in ((1, 20, 12), (13, 32, 4)):
        rva = struct.unpack_from("<I", data, directories + directory * 8)[0]
        if not rva:
            continue
        cursor = offset(rva)
        while any(data[cursor:cursor + stride]):
            name_rva = struct.unpack_from("<I", data, cursor + name_field)[0]
            start = offset(name_rva)
            names.append(data[start:data.index(b"\0", start)].decode("ascii"))
            cursor += stride
    return names


def extract_resource(executable, resource_id, destination):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryExW.argtypes = (wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD)
    kernel.LoadLibraryExW.restype = wintypes.HMODULE
    kernel.FindResourceW.argtypes = (wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p)
    kernel.FindResourceW.restype = wintypes.HRSRC
    kernel.LoadResource.argtypes = (wintypes.HMODULE, wintypes.HRSRC)
    kernel.LoadResource.restype = wintypes.HGLOBAL
    kernel.LockResource.argtypes = (wintypes.HGLOBAL,)
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.SizeofResource.argtypes = (wintypes.HMODULE, wintypes.HRSRC)
    kernel.SizeofResource.restype = wintypes.DWORD
    kernel.FreeLibrary.argtypes = (wintypes.HMODULE,)
    module = kernel.LoadLibraryExW(str(executable), None, 0x22)
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        resource = kernel.FindResourceW(module, ctypes.c_void_p(resource_id), ctypes.c_void_p(10))
        if not resource:
            raise ctypes.WinError(ctypes.get_last_error())
        size = kernel.SizeofResource(module, resource)
        pointer = kernel.LockResource(kernel.LoadResource(module, resource))
        if not pointer or not size:
            raise ValueError(f"Empty embedded resource {resource_id}")
        with destination.open("wb") as stream:
            for position in range(0, size, 8 * 1024 * 1024):
                stream.write(ctypes.string_at(pointer + position, min(size - position, 8 * 1024 * 1024)))
    finally:
        kernel.FreeLibrary(module)


def stage(args):
    browser = args.browser_dir.resolve(strict=True)
    work = args.work_dir.resolve(strict=True)
    payload = work / "payload"
    payload.mkdir()
    source = work / "chromium-payload"
    chrome = source / "Helium-bin"
    chrome.mkdir(parents=True)
    browser_version = file_version(browser / "chrome.exe")
    for helper in (args.updater, args.uninstaller):
        if file_version(helper.resolve(strict=True)) != browser_version:
            raise ValueError(f"{helper.name} must carry browser file version {browser_version}")
    version = chrome / browser_version
    distribution = browser.parents[1] / "chrome/installer/mini_installer/chrome.release"
    rules = configparser.ConfigParser(interpolation=None, strict=False)
    rules.optionxform = str
    rules.read(distribution, encoding="utf-8")
    if not rules.sections():
        raise ValueError(f"Missing Chromium distribution manifest: {distribution}")

    def copy(src, destination):
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, destination)

    for section in rules.sections():
        for pattern, target in rules.items(section):
            if pattern.lower() in ("winsparkle.dll", "helium_update_helper.exe"):
                continue
            if pattern.lower().endswith((".pdb", ".lib", ".exp", ".obj")):
                continue
            target = target.replace("%(VersionDir)s", browser_version)
            target = target.replace("%(ChromeDir)s", "").replace("\\", "/").strip("/")
            relative = Path(target)
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError(f"Unsafe distribution target: {target}")
            for item in browser.glob(pattern.replace("\\", "/")):
                if item.is_file():
                    copy(item, chrome / relative / item.name)

    for name in ("krypton_ui.dll", "Microsoft.WindowsAppRuntime.Bootstrap.dll"):
        copy(browser / name, version / name)
    for name in ("hr_classic.png", "hr_compact.png", "hr_dyn.png", "vt_compact.png",
                 "vt_sidebar.png", "vt_sidebar_title.png"):
        copy(browser / "assets/settings" / name, version / "assets/settings" / name)
    for pattern in ("msvcp*.dll", "vcruntime*.dll", "concrt*.dll"):
        for item in browser.glob(pattern):
            copy(item, version / item.name)
            copy(item, chrome / item.name)
            copy(item, payload / item.name)

    # Resolve local non-system imports into the version directory, using the
    # browser's own distribution rules for the normal root/version split.
    queue = list(chrome.rglob("*.exe")) + list(chrome.rglob("*.dll"))
    scanned = set()
    while queue:
        binary = queue.pop()
        if binary.name.lower() in scanned:
            continue
        scanned.add(binary.name.lower())
        for name in imports(binary):
            if name.lower() in ("winsparkle.dll", "helium_update_helper.exe"):
                raise ValueError("A browser binary still imports the retired updater")
            item = browser / name
            if item.is_file() and not (version / name).exists():
                copy(item, version / name)
                queue.append(version / name)

    for item in (chrome / "chrome.exe", version / "chrome.dll",
                 version / "chrome_elf.dll", version / "resources.pak", version / "icudtl.dat"):
        if not item.is_file():
            raise ValueError(f"Required native browser file is missing: {item}")
    if not any((version / "Locales").glob("*.pak")):
        raise ValueError("Browser locale packs are missing")

    copy(args.updater.resolve(strict=True), version / "Installer/KryptonUpdater.exe")
    copy(args.uninstaller.resolve(strict=True), version / "Installer/KryptonUninstaller.exe")
    for notice in (Path(__file__).resolve().parents[1] / "licenses").glob("*"):
        if notice.is_file():
            copy(notice, version / "Installer/Licenses" / notice.name)
    for helper in (args.updater, args.uninstaller):
        sign(helper, args.key_path)
        copy(Path(str(helper) + ".sig"), version / "Installer" / (helper.name + ".sig"))
    copy(browser / "setup.exe", payload / "setup.exe")
    (version / "Installer/KryptonPackage.manifest").write_text(
        f"[Package]\nVersion={browser_version}\nEngineVersion={file_version(browser / 'chrome.exe', product=True)}\nSetupSha256={digest(payload / 'setup.exe')}\n",
        encoding="ascii")
    marker = version / "Installer/KryptonPackage.manifest"
    sign(marker, args.key_path)
    copy(marker, payload / marker.name)
    copy(Path(str(marker) + ".sig"), payload / (marker.name + ".sig"))
    license_file = browser.parents[1] / "LICENSE"
    if license_file.is_file():
        copy(license_file, version / "LICENSE.txt")

    runtime_files = [{"path": item.relative_to(chrome).as_posix(),
                      "sha256": digest(item), "size": item.stat().st_size}
                     for item in sorted(chrome.rglob("*")) if item.is_file()]
    (work / "chromium-files.manifest.json").write_text(
        json.dumps({"version": browser_version, "files": runtime_files}, indent=2), encoding="utf-8")
    native_archive = payload / "helium.7z"
    # Chromium's inner archive is a stored 7z. Its native reader is not the
    # full 7-Zip decoder and cannot unpack the default LZMA2 compression.
    # Compression belongs to the verified outer delivery archive.
    subprocess.run([args.seven_zip, "a", "-t7z", "-mx=0", "-ms=off", "-mmt=8", "-y",
                    str(native_archive), "Helium-bin"], cwd=source, check=True,
                   stdout=subprocess.DEVNULL)
    subprocess.run([args.seven_zip, "t", str(native_archive)], check=True, stdout=subprocess.DEVNULL)
    (payload / "chromium-install.manifest").write_text(
        f"[Chromium]\nBrowserVersion={browser_version}\n", encoding="ascii")

    # The baseline setup can probe the browser framework before launching the
    # AVX2-compiled native setup, without loading or running the browser itself.
    copy(browser / "Microsoft.WindowsAppRuntime.Bootstrap.dll",
         payload / "Microsoft.WindowsAppRuntime.Bootstrap.dll")
    for name in ("requirements.manifest", "vc_redist.x64.exe", "WindowsAppRuntimeInstall-x64.exe"):
        copy(args.prerequisites_dir / name, payload / "prerequisites" / name)

    components = browser_version.split(".")
    install_version = ".".join(components[:3]) + "-dev." + components[3]
    files = [{"path": item.relative_to(payload).as_posix(), "sha256": digest(item),
              "size": item.stat().st_size}
             for item in sorted(payload.rglob("*")) if item.is_file()]
    archive = work / "browser.7z"
    manifest = Path(str(archive) + ".manifest.json")
    manifest.write_text(json.dumps({"version": install_version, "files": files}, indent=2), encoding="utf-8")
    sign(manifest, args.key_path)
    subprocess.run([args.seven_zip, "a", "-t7z", "-mx=3", "-ms=off", "-mmt=8", "-y",
                    str(archive), "."], cwd=payload, check=True, stdout=subprocess.DEVNULL)
    subprocess.run([args.seven_zip, "t", str(archive)], check=True, stdout=subprocess.DEVNULL)
    result = {"browser_version": browser_version, "install_version": install_version,
              "manifest_signature": str(manifest) + ".sig",
              "archive": str(archive), "manifest": str(manifest), "file_count": len(files),
              "runtime_file_count": len(runtime_files), "format": "chromium",
              "uncompressed_bytes": sum(item["size"] for item in files),
              "archive_bytes": archive.stat().st_size, "archive_sha256": digest(archive)}
    (work / "package.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    staging = commands.add_parser("stage")
    for name in ("browser-dir", "work-dir", "uninstaller", "updater", "prerequisites-dir"):
        staging.add_argument("--" + name, type=Path, required=True)
    staging.add_argument("--seven-zip", default="7z")
    staging.add_argument("--key-path", type=Path)
    verification = commands.add_parser("verify-embedded")
    verification.add_argument("--installer", type=Path, required=True)
    verification.add_argument("--package-json", type=Path, required=True)
    web = commands.add_parser("verify-web")
    web.add_argument("--installer", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "stage":
        stage(args)
    elif args.command == "verify-web":
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
        kernel.LoadLibraryExW.restype = wintypes.HMODULE
        kernel.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
        kernel.FindResourceW.restype = wintypes.HANDLE
        kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
        module = kernel.LoadLibraryExW(str(args.installer.resolve(strict=True)), None, 0x22)
        if not module:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            if any(kernel.FindResourceW(module, identifier, 10) for identifier in (101, 105, 106)):
                raise ValueError("The web installer must not contain offline payload resources")
            if not kernel.FindResourceW(module, 103, 10):
                raise ValueError("The web installer is missing its setup UI resources")
        finally:
            kernel.FreeLibrary(module)
        print(f"Verified payload-free web installer: {args.installer}")
    else:
        metadata = json.loads(args.package_json.read_text(encoding="utf-8"))
        archive = args.package_json.parent / "embedded.7z"
        extract_resource(args.installer.resolve(strict=True), 101, archive)
        manifest = Path(str(archive) + ".manifest.json")
        extract_resource(args.installer.resolve(strict=True), 105, manifest)
        signature = Path(str(manifest) + ".sig")
        extract_resource(args.installer.resolve(strict=True), 106, signature)
        subprocess.run(["pwsh", "-NoProfile", "-File", str(Path(__file__).with_name("release-signing.ps1")),
                        "verify", "-InputFile", str(manifest), "-SignatureFile", str(signature)], check=True)
        if digest(archive) != metadata["archive_sha256"] or digest(manifest) != digest(Path(metadata["manifest"])):
            raise ValueError("Embedded installer resources do not match the verified package")
        print(str(archive))


if __name__ == "__main__":
    main()
