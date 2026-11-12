"""Copy only the public release trust key into Krypton's browser overlay."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--browser-root", type=Path, default=Path(__file__).resolve().parents[2] / "Krypton")
args = parser.parse_args()
trust = json.loads((Path(__file__).resolve().parents[1] / "resources/update-trust.json").read_text())
key = bytes.fromhex(trust["public_key"])
if len(key) != 65 or key[0] != 4 or trust["repository"] != "perpetus/krypton-windows":
    raise ValueError("Unexpected public release trust key")
header = ("// Copyright 2026 Krypton Authors\n"
          "// Use of this source code is governed by the license in the LICENSE file.\n"
          "#ifndef CHROME_BROWSER_WIN_KRYPTON_UPDATE_TRUST_H_\n"
          "#define CHROME_BROWSER_WIN_KRYPTON_UPDATE_TRUST_H_\n"
          "#include <array>\nnamespace krypton {\n"
          "inline constexpr std::array<unsigned char, 65> kReleasePublicKey = {" +
          ", ".join(f"0x{byte:02x}" for byte in key) +
          "};\n}  // namespace krypton\n"
          "#endif  // CHROME_BROWSER_WIN_KRYPTON_UPDATE_TRUST_H_\n")
directory = args.browser_root / "chromium_src/chrome/browser/win"
directory.mkdir(parents=True, exist_ok=True)
(directory / "krypton_update_trust.h").write_text(header, encoding="utf-8")
print("Browser public key: " + trust["key_id"])
