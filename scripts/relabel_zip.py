"""Rewrite an AdrenoTools driver zip with a new display name / packageVersion.

usage: python relabel_zip.py <in.zip> <out.zip> <display name> <packageVersion>
"""
import json
import sys
import zipfile

src, dst, name, version = sys.argv[1:5]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename == "meta.json":
            meta = json.loads(data)
            meta["name"] = name
            meta["packageVersion"] = version
            data = (json.dumps(meta, indent=2) + "\n").encode()
            print(data.decode())
        zout.writestr(item, data)
