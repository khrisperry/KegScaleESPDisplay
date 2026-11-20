"""Package only this build; retain the ELF needed to decode allocation PCs."""
import hashlib
import json
import os
from pathlib import Path
import shutil

project = Path(os.environ['PROJECT_DIR'])
build = project / 'build'
desc = json.loads((build / 'project_description.json').read_text())
name = desc['project_name']
configuration = (project / 'sdkconfig').read_text()
if 'CONFIG_HEAP_USE_HOOKS=y' not in configuration:
    raise SystemExit('Allocation hooks disabled in effective sdkconfig')
symbols = (build / f'{name}.elf').read_bytes()
for symbol in (b'fr_init', b'esp_heap_trace_alloc_hook', b'esp_heap_trace_free_hook'):
    if symbol not in symbols:
        raise SystemExit(f'Missing diagnostic symbol: {symbol!r}')

output = Path('diagnostic-package')
output.mkdir(exist_ok=True)
for source in [build / f'{name}.bin', build / f'{name}.elf', build / f'{name}.map',
               build / 'flasher_args.json', build / 'bootloader/bootloader.bin',
               build / 'partition_table/partition-table.bin', build / 'ota_data_initial.bin',
               project / 'sdkconfig']:
    if not source.is_file():
        if source.suffix == '.map' or source.name == 'ota_data_initial.bin':
            continue
        raise SystemExit(f'Missing build artifact: {source}')
    destination = output / source.relative_to(build) if source.is_relative_to(build) else output / source.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
identity = dict(device=os.environ['DEVICE'], commit=os.environ['GITHUB_SHA'],
                project=name, version=desc['project_version'], installation='manual-only',
                partition_table_changed=False, protocol_changed=False)
(output / 'BUILD_IDENTITY.json').write_text(json.dumps(identity, indent=2)+'\n')
files = sorted(p for p in output.rglob('*') if p.is_file())
(output / 'SHA256SUMS.txt').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(output)}\n' for p in files))
shutil.copy2('docs/diagnostic-capture.md', output / 'README.md')
