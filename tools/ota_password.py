"""Read the OTA upload password from the local firmware configuration."""

import ast
from pathlib import Path
import re
import subprocess

Import("env")

config = Path(env.subst("$PROJECT_DIR")) / "include" / "Config.h"
match = re.search(
    r'^\s*#\s*define\s+OTA_PASSWORD\s+("(?:\\.|[^"\\])*")',
    config.read_text(encoding="utf-8-sig"),
    re.MULTILINE,
)
if not match:
    raise ValueError('Config.h must define OTA_PASSWORD as a quoted string.')

password = ast.literal_eval(match.group(1))
# The platform initializes UPLOADERFLAGS during its build script. Run this
# script as post: and append directly to the flags used by the upload command.
env.Append(UPLOADERFLAGS=[subprocess.list2cmdline(["--auth=" + password])])
