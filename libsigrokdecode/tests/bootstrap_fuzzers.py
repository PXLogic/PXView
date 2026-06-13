import os
import re

def to_class_name(decoder_id):
    return "".join(x.capitalize() or '_' for x in decoder_id.replace('_c', '').split('_')) + "Generator"

def to_module_name(decoder_id):
    return decoder_id.replace('_c', '')

def main():
    testdata_dir = 'testdata'
    fuzzers_dir = 'fuzzers'
    
    with open('generate_testdata.py', 'r', encoding='utf-8') as f:
        gen_content = f.read()

    # Find all decoders that have testdata configs
    all_decoders = [d for d in os.listdir(testdata_dir) if d.endswith('_c')]
    
    missing_decoders = []
    for d in all_decoders:
        config_path = os.path.join(testdata_dir, d, 'default', 'config.json')
        if not os.path.exists(config_path):
            continue
            
        # Check if it's in the old if/elif block
        if d in gen_content:
            continue
            
        # Check if fuzzer already exists
        module_name = to_module_name(d)
        fuzzer_path = os.path.join(fuzzers_dir, f'{module_name}.py')
        if not os.path.exists(fuzzer_path):
            missing_decoders.append(d)

    print(f"Found {len(missing_decoders)} missing fuzzers to bootstrap.")
    
    init_path = os.path.join(fuzzers_dir, '__init__.py')
    with open(init_path, 'a', encoding='utf-8') as f_init:
        for d in missing_decoders:
            module_name = to_module_name(d)
            class_name = to_class_name(d)
            
            # Create the fuzzer file
            fuzzer_path = os.path.join(fuzzers_dir, f'{module_name}.py')
            with open(fuzzer_path, 'w', encoding='utf-8') as f_out:
                f_out.write(f'''import math
from .base import *

class {class_name}:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        # TODO: Reverse engineer the protocol state machine and generate valid waveform
        # For now, this does nothing, which will result in 0 annotations (WARN).
        pass
''')
            
            # Register in __init__.py
            f_init.write(f'from .{module_name} import {class_name}\n')
            print(f"Bootstrapped {class_name} in {module_name}.py")

if __name__ == '__main__':
    main()
