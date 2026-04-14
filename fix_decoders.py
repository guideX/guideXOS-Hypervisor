#!/usr/bin/env python3
"""
Script to remove inline class definitions from decoder implementation files
and convert methods to use scope resolution operators.
"""

import re
import os

def process_decoder_file(filepath, class_name):
    """Process a single decoder file to remove inline class definition."""
    
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Remove the class declaration line
    content = re.sub(r'^class ' + class_name + r' \{\s*$', '', content, flags=re.MULTILINE)
    
    # Remove public: section marker
    content = re.sub(r'^\s*public:\s*$', '', content, flags=re.MULTILINE)
    
    # Remove private: section marker  
    content = re.sub(r'^\s*private:\s*$', '', content, flags=re.MULTILINE)
    
    # Remove closing brace and semicolon for class
    content = re.sub(r'^};\s*$\s*^} // namespace decoder', '} // namespace decoder', content, flags=re.MULTILINE)
    
    # Convert static method declarations to implementations with scope resolution
    # Pattern: static TYPE methodName(...) {
    # Replace with: TYPE ClassName::methodName(...) {
    content = re.sub(
        r'^\s*static\s+(bool|void|InstructionEx)\s+(' + class_name + r'::)?(\w+)\s*\(',
        r'\1 ' + class_name + r'::\3(',
        content,
        flags=re.MULTILINE
    )
    
    # Handle already converted methods (remove duplicate class name)
    content = re.sub(class_name + r'::' + class_name + r'::', class_name + r'::', content)
    
    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(content)
    
    print(f"Processed {class_name}")

# Process all decoder files
base_path = r"D:\devgitlab\guideXOS.Hypervisor\src\decoder"

decoders = [
    ("itype_decoder.cpp", "ITypeDecoder"),
    ("mtype_decoder.cpp", "MTypeDecoder"),
    ("btype_decoder.cpp", "BTypeDecoder"),
    ("lx_decoder.cpp", "LXDecoder")
]

for filename, classname in decoders:
    filepath = os.path.join(base_path, filename)
    if os.path.exists(filepath):
        process_decoder_file(filepath, classname)
    else:
        print(f"File not found: {filepath}")

print("All files processed!")
