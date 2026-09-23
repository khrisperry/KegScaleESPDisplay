#!/usr/bin/env python3
"""Host tests of production e-paper wake, rendering and pairing handlers."""
from pathlib import Path
import re
import subprocess
import tempfile
from run_ota_tests import functions
root=Path(__file__).resolve().parents[1]
main=root/'main/main.c'
ui=root/'components/display_ui/display_ui.c'
with tempfile.TemporaryDirectory(prefix='keg-wake-') as directory:
    tmp=Path(directory)
    source=main.read_text(encoding='utf-8')
    types=re.search(r'typedef struct \{.*?\} retained_state_t;',source,re.S)[0]
    constants='\n'.join(line for line in source.splitlines() if line.startswith(('#define RETAINED_MAGIC ', '#define SIGNIFICANT_WEIGHT_LBS ', '#define SCALE_OFFLINE_FAILURE_THRESHOLD ')))
    constants+='\n'+'\n'.join(line for line in ui.read_text().splitlines() if line.startswith(('#define SCALE_SCREEN_MAGIC ', '#define SCALE_FULL_REFRESH_INTERVAL ')))
    (tmp/'wake_types.inc').write_text(constants+'\n'+types)
    (tmp/'wake_functions.inc').write_text(functions(main,[
      'is_touch_wake','wake_reason','retained_matches_peer','initialize_retained_peer',
      'remember_runtime_settings','record_scale_check_failure','should_refresh',
      'remember_displayed_state','disable_wake_source_if_enabled','configure_wake_sources',
      'sleep_for_touch_delay','validate_and_save_peer','fetch_paired_state_once',
      'render_if_needed','clear_touch_acknowledgement_if_needed','handle_unpair_request'
    ])+'\n'+functions(ui,['present_scale']))
    binary=tmp/'wake-test'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(tmp),
      '-I'+str(root/'tests/ota_host'),'-I'+str(root/'components/ble_client/include'),
      '-I'+str(root/'main'),str(root/'tests/wake_state_test.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
