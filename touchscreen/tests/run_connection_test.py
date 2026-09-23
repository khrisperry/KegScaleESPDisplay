#!/usr/bin/env python3
"""Compile unchanged production connection handlers with deterministic host fakes."""
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('helpers', root / 'tests/run_ota_tests.py')
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)
main = root / 'touchscreen/main/main.cpp'
source = main.read_text(encoding='utf-8')
cjson = root / 'touchscreen/managed_components/espressif__cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='keg-connection-') as directory:
    tmp = Path(directory)
    types = []
    for path, names in [(root / 'touchscreen/main/app.h', ['Settings', 'State']),
                        (main, ['StoredScaleProfile', 'Frame', 'ScaleConnection'])]:
        text = path.read_text(encoding='utf-8')
        for name in names:
            match = re.search(r'struct ' + name + r' \{.*?^\};', text, re.M | re.S)
            if not match:
                raise RuntimeError('Missing production type: ' + name)
            types.append(match[0])
    (tmp / 'connection_types.inc').write_text('\n'.join(types), encoding='utf-8')
    (tmp / 'connection_functions.inc').write_text(helpers.functions(main, [
        'str', 'num', 'yes', 'wifi_event', 'socket_event', 'disconnected',
        'stop_scale_transport', 'connect_scale', 'on_frame'
    ]) + '\n' + helpers.functions(root / 'touchscreen/main/main_wrapper.cpp', [
        'ota_scale_transport_busy'
    ]), encoding='utf-8')
    if (cjson / 'cJSON.c').exists():
        obj = tmp / 'cjson.o'
        subprocess.run(['cc', '-c', str(cjson / 'cJSON.c'), '-o', str(obj)], check=True)
        json_link = str(obj)
    else:
        cjson = Path('/usr/include/cjson')
        if not (cjson / 'cJSON.h').exists():
            raise SystemExit('Install libcjson-dev or configure the Touch ESP-IDF project first.')
        json_link = '-lcjson'
    binary = tmp / 'connection-test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(tmp), '-I' + str(cjson),
                    '-I' + str(root / 'touchscreen/tests/host'),
                    '-I' + str(root / 'touchscreen/components/controller_link/include'),
                    str(root / 'touchscreen/tests/connection_test.cpp'), json_link,
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
