"""Prepare manually publishable, signed GitHub release assets. Never uploads."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time


SIGNING = Path(__file__).with_name("release-signing.ps1")
REPOSITORY = "perpetus/krypton-windows"


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def signing(command, path, key=None):
    args = ["pwsh", "-NoProfile", "-File", str(SIGNING), command, "-InputFile", str(path)]
    if key and command == "sign":
        args += ["-KeyPath", str(key)]
    subprocess.run(args, check=True)


def verify_delta(base, target, destination, zucchini, timeout_seconds):
    """Require an existing patch to reconstruct the exact signed target package."""
    reconstructed = destination.with_name(destination.name + ".reconstructed")
    try:
        subprocess.run([str(zucchini), "-apply", str(base), str(destination), str(reconstructed)],
                       check=True, timeout=timeout_seconds)
        if reconstructed.stat().st_size != target.stat().st_size or sha256(reconstructed) != sha256(target):
            raise ValueError("The generated Zucchini patch failed its target size/hash check")
    finally:
        reconstructed.unlink(missing_ok=True)


def create_delta(base, target, destination, zucchini, timeout_seconds, generator=None, threads=0):
    """Generate a compatible patch, then verify with the unchanged client helper."""
    deadline = time.monotonic() + timeout_seconds
    command = [str(generator or zucchini), "-gen", str(base), str(target), str(destination)]
    if generator:
        command.append(f"-threads={threads}")
    elif base.suffix.lower() == ".7z" and target.suffix.lower() == ".7z":
        # Compressed delivery archives expose no useful executable elements.
        command.append("-raw")
    subprocess.run(command, check=True, timeout=timeout_seconds)
    verify_delta(base, target, destination, zucchini,
                 max(0.01, deadline - time.monotonic()))


def discover_bases(current_version, target_hash):
    """Find retained publisher packages, including distinct same-version bases."""
    candidates = []
    root = Path(__file__).resolve().parents[1]
    for path in (root / "out/packages").glob("*/package.json"):
        try:
            item = json.loads(path.read_text(encoding="utf-8"))
            base_version = version(item["browser_version"])
            if (base_version < current_version and item["archive_sha256"] != target_hash
                    and Path(item["archive"]).is_file() and Path(item["manifest"]).is_file()):
                candidates.append((base_version, path.stat().st_mtime_ns, item["archive_sha256"], path))
        except (OSError, ValueError, KeyError):
            continue
    seen = set()
    result = []
    for _, _, digest, path in sorted(candidates, reverse=True):
        if digest not in seen:
            seen.add(digest)
            result.append(path)
        if len(result) == 4:
            break
    return result


def asset(path):
    return {"name": path.name, "size": path.stat().st_size, "sha256": sha256(path)}


def version(text):
    parts = text.split(".")
    if len(parts) != 4 or any(not part.isascii() or not part.isdecimal() or
                              str(int(part)) != part or int(part) > 65535 for part in parts):
        raise ValueError("Expected a canonical four-part browser version")
    return tuple(map(int, parts))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-json", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--base-package-json", type=Path, action="append", default=[])
    parser.add_argument("--skip-deltas", action="store_true",
                        help="Publish the verified full package without generating optional deltas")
    parser.add_argument("--delta-timeout-seconds", type=int, default=300,
                        help="Total time budget for optional delta generation and reconstruction (default: 300)")
    parser.add_argument("--key-path", type=Path)
    parser.add_argument("--zucchini", type=Path, default=Path(__file__).resolve().parents[2] /
                        "Krypton/build/src/out/Default/KryptonPatch.exe")
    parser.add_argument("--delta-generator", type=Path,
                        help="Publisher-only parallel generator; patches retain the existing Zucchini format")
    parser.add_argument("--delta-threads", type=int, default=0,
                        help="Parallel generation workers; 0 selects all CPU threads (default)")
    parser.add_argument("--keep-large-deltas", action="store_true",
                        help="Retain explicitly requested, verified deltas with less than 15%% savings")
    parser.add_argument("--expires-days", type=int, default=90, choices=range(1, 91))
    parser.add_argument("--sequence", type=int)
    args = parser.parse_args()
    if len(args.base_package_json) > 4:
        parser.error("At most four delta bases are supported")
    if args.delta_timeout_seconds <= 0:
        parser.error("The delta timeout must be positive")
    if args.delta_threads < 0:
        parser.error("The delta thread count cannot be negative")
    package = json.loads(args.package_json.read_text(encoding="utf-8"))
    archive = Path(package["archive"]).resolve(strict=True)
    metadata = Path(package["manifest"]).resolve(strict=True)
    signing("verify", metadata)
    if sha256(archive) != package["archive_sha256"] or archive.stat().st_size > 2 * 1024**3:
        raise ValueError("The full update package does not match its recorded hash or size limit")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    current_version = version(package["browser_version"])
    full = args.output_dir / f"KryptonUpdate-{package['browser_version']}-win-x64.7z"
    shutil.copy2(archive, full)
    deltas = []
    bases = args.base_package_json or discover_bases(current_version, package["archive_sha256"])
    if args.skip_deltas:
        bases = []
        print("Optional delta generation skipped; preparing the signed full update.", flush=True)
    zucchini = args.zucchini.resolve(strict=True) if bases else None
    generator = args.delta_generator
    if generator is None and zucchini:
        candidate = zucchini.with_name("KryptonDeltaGenerator.exe")
        if candidate.is_file():
            generator = candidate
    generator = generator.resolve(strict=True) if generator and bases else None
    delta_deadline = time.monotonic() + args.delta_timeout_seconds
    for base_path in bases:
        base = json.loads(base_path.read_text(encoding="utf-8"))
        if version(base["browser_version"]) >= current_version:
            raise ValueError("A delta base must be older than the new release")
        base_archive = Path(base["archive"]).resolve(strict=True)
        signing("verify", Path(base["manifest"]))
        if sha256(base_archive) != base["archive_sha256"]:
            raise ValueError("The delta base archive hash is invalid")
        patch = args.output_dir / f"KryptonUpdate-{base['browser_version']}-{base['archive_sha256'][:12]}-to-{package['browser_version']}-win-x64.delta"
        remaining = delta_deadline - time.monotonic()
        if remaining <= 0:
            print("Delta time budget exhausted; remaining bases use the full update.", flush=True)
            break
        print(f"Generating Zucchini delta from {base['browser_version']} "
              f"({base_archive.stat().st_size:,} to {archive.stat().st_size:,} bytes; "
              f"{remaining:.0f}s remaining).", flush=True)
        try:
            if patch.is_file() and patch.stat().st_size > 0:
                print("Verifying the existing delta before reuse.", flush=True)
                verify_delta(base_archive, archive, patch, zucchini, remaining)
            else:
                create_delta(base_archive, archive, patch, zucchini, remaining,
                             generator, args.delta_threads)
        except subprocess.TimeoutExpired:
            patch.unlink(missing_ok=True)
            print("Delta time budget exhausted; publishing the verified full update.", flush=True)
            break
        except (OSError, subprocess.CalledProcessError) as error:
            patch.unlink(missing_ok=True)
            print(f"Delta unavailable for {base['browser_version']}: {error}; publishing the full package", flush=True)
            continue
        if patch.stat().st_size >= int(full.stat().st_size * .85):
            if not args.keep_large_deltas:
                patch.unlink()
                print(f"Delta for {base['browser_version']} omitted: less than 15% bandwidth reduction")
                continue
            print(f"Keeping the explicitly requested verified delta for {base['browser_version']}: "
                  f"{patch.stat().st_size:,} bytes, "
                  f"{100 * (1 - patch.stat().st_size / full.stat().st_size):.2f}% bandwidth reduction.")
        deltas.append({**asset(patch), "algorithm": "zucchini", "base_version": base["browser_version"],
                       "base_sha256": base["archive_sha256"]})
    deltas.sort(key=lambda item: item["size"])
    print(f"Prepared {len(deltas)} verified Zucchini delta(s) from {len(bases)} retained base package(s)")
    now = int(time.time())
    sequence = args.sequence if args.sequence is not None else time.time_ns() // 1_000_000
    if not 0 < sequence < 2**63:
        raise ValueError("Sequence must be a positive signed 64-bit integer")
    release = {"schema": 1, "repository": REPOSITORY, "channel": "stable",
               "version": package["browser_version"], "tag": "v" + package["browser_version"],
               "sequence": sequence, "issued": now, "expires": now + args.expires_days * 86400,
               "full": asset(full), "deltas": deltas,
               "package_manifest": metadata.read_bytes().decode("utf-8"),
               "package_signature": Path(str(metadata) + ".sig").read_bytes().hex()}
    output = args.output_dir / "KryptonUpdate-win-x64.json"
    output.write_bytes((json.dumps(release, indent=2) + "\n").encode("utf-8"))
    signing("sign", output, args.key_path)
    signing("verify", output)
    publish = {"repository": REPOSITORY, "tag": release["tag"],
               "assets": [full.name, output.name, output.name + ".sig"] + [delta["name"] for delta in deltas],
               "source_package": str(args.package_json.resolve()), "sequence": sequence}
    (args.output_dir / "release-assets.json").write_text(json.dumps(publish, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(publish, indent=2))


if __name__ == "__main__":
    main()
