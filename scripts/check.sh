#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

bash -n "$ROOT"/scripts/*.sh
python3 "$ROOT/tests/test_intel_json.py"
python3 "$ROOT/tests/test_sql.py"
python3 - <<'PY' "$ROOT"
import pathlib, sys, xml.etree.ElementTree as ET
root = pathlib.Path(sys.argv[1])
ET.parse(root / 'packaging/io.github.litemon.LiteMon.metainfo.xml')
required = [
  'README.md','LICENSE','CONTRIBUTING.md','CODE_OF_CONDUCT.md','SECURITY.md','CHANGELOG.md','ROADMAP.md',
  'docs/ARCHITECTURE.md','docs/GPU.md','docs/TESTING.md','.clang-format','.clang-tidy','CMakePresets.json'
]
missing=[p for p in required if not (root/p).exists()]
if missing: raise SystemExit('missing required files: '+', '.join(missing))
print('project metadata PASS')
PY
