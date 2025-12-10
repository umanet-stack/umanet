import os
import re

HEADER = """/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */
"""

dirs_to_scan = ["src", "include"]

def remove_old_copyright(content):
    """Remove existing copyright header if present at the start of file."""
    # Match C-style comment block at the very beginning
    # This handles multi-line /* ... */ comments
    pattern = r'^\s*/\*.*?\*/\s*\n*'
    match = re.match(pattern, content, re.DOTALL)
    
    if match:
        # Check if the matched comment looks like a copyright/license header
        matched_text = match.group(0)
        keywords = ['copyright', 'license', 'spdx', 'permission', 'gnu', 'bsd', 'mit']
        if any(keyword in matched_text.lower() for keyword in keywords):
            # Remove the old header
            return content[match.end():]
    
    return content

for dir_path in dirs_to_scan:
    for root, _, files in os.walk(dir_path):
        for f in files:
            if f.endswith((".c", ".h")):
                path = os.path.join(root, f)
                with open(path, "r", encoding="utf-8") as file:
                    content = file.read()
                
                # Remove old copyright if present
                cleaned_content = remove_old_copyright(content)
                
                # Add new header
                new_content = HEADER + "\n" + cleaned_content
                
                # Write back only if changed
                if new_content != content:
                    with open(path, "w", encoding="utf-8") as file:
                        file.write(new_content)
                    print(f"Updated {path}")
