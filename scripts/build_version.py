"""PlatformIO pre-build hook; regenerate only when the Git-derived label changes."""
Import('env')
from pathlib import Path
import importlib.util
import json
project = Path(env.subst('$PROJECT_DIR'))
spec = importlib.util.spec_from_file_location('version_info',project/'scripts/version_info.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
value = module.firmware_version(project)
folder = project/'.build-cache'
folder.mkdir(parents=True,exist_ok=True)
header = folder/'build_version.h'
content = '#pragma once\n#define PAGER_VERSION '+json.dumps(value)+'\n'
if not header.exists() or header.read_text()!=content:
    header.write_text(content)
env.Append(CPPPATH=[str(folder)],CPPDEFINES=['PAGER_HAS_BUILD_VERSION'])
print('Firmware version: '+value)
