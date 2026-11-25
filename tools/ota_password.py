"""Read the OTA upload password from the local firmware configuration."""

import ast
from pathlib import Path
import re
import subprocess

Import("env")

config = Path(env.subst("$PROJECT_DIR")) / "include" / "Config.h"
config_text = config.read_text(encoding="utf-8-sig")

# An explicitly defined LAST_OTA_PASSWORD (including an empty string) is the
# password of the running device. OTA_PASSWORD belongs to the new firmware.
password = None
for name in ("LAST_OTA_PASSWORD", "OTA_PASSWORD"):
    match = re.search(
        r'^\s*#\s*define\s+' + name + r'\s+("(?:\\.|[^"\\])*")',
        config_text,
        re.MULTILINE,
    )
    if match:
        password = ast.literal_eval(match.group(1))
        break
    if re.search(r'^\s*#\s*define\s+' + name + r'\b', config_text, re.MULTILINE):
        raise ValueError(f'Config.h must define {name} as a quoted string.')

if password is None:
    raise ValueError('Config.h must define OTA_PASSWORD as a quoted string.')

# The platform initializes UPLOADERFLAGS during its build script. Run this
# script as post: and append directly to the flags used by the upload command.
env.Append(UPLOADERFLAGS=[subprocess.list2cmdline(["--auth=" + password])])
