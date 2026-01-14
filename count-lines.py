#!/usr/bin/env python3
"""
Count lines of C code in /src folder, excluding comments
"""
import os
import re
import sys
from pathlib import Path

# Get script directory and src folder
SCRIPT_DIR = Path(__file__).parent.resolve()
SRC_DIR = SCRIPT_DIR / 'src'

def remove_comments(content):
    """Remove C comments from code content"""
    # Remove multi-line comments /* ... */
    # This regex handles comments that span multiple lines
    content = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)
    
    # Remove single-line comments // ... but not if they're part of a URL or string
    # We need to be careful not to remove // inside strings
    lines = content.split('\n')
    result_lines = []
    in_string = False
    string_char = None
    
    for line in lines:
        new_line = ''
        i = 0
        while i < len(line):
            char = line[i]
            
            # Track string state
            if char in ('"', "'") and (i == 0 or line[i-1] != '\\'):
                if not in_string:
                    in_string = True
                    string_char = char
                elif char == string_char:
                    in_string = False
                    string_char = None
            
            # If we find // and we're not in a string, skip the rest
            if not in_string and i < len(line) - 1 and line[i:i+2] == '//':
                break
            
            new_line += char
            i += 1
        
        result_lines.append(new_line)
    
    return '\n'.join(result_lines)

def count_lines_in_file(file_path):
    """Count non-comment, non-blank lines in a C file"""
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
        
        # Remove comments
        content_no_comments = remove_comments(content)
        
        # Count non-blank lines
        lines = content_no_comments.split('\n')
        non_blank_lines = [line for line in lines if line.strip()]
        
        return len(non_blank_lines)
    except Exception as e:
        print(f"Error processing {file_path}: {e}", file=sys.stderr)
        return 0

def main():
    """Main function"""
    if not SRC_DIR.exists():
        print(f"Error: {SRC_DIR} does not exist")
        return 1
    
    total_lines = 0
    file_counts = []
    
    # Find all .c and .h files
    for ext in ['*.c', '*.h']:
        for file_path in SRC_DIR.rglob(ext):
            lines = count_lines_in_file(file_path)
            total_lines += lines
            relative_path = file_path.relative_to(SRC_DIR)
            file_counts.append((relative_path, lines))
    
    # Sort by path
    file_counts.sort(key=lambda x: str(x[0]))
    
    # Print results
    print(f"Lines of C code in {SRC_DIR} (excluding comments and blank lines):")
    print(f"{'=' * 60}")
    for file_path, lines in file_counts:
        print(f"{str(file_path):<50} {lines:>6} lines")
    print(f"{'=' * 60}")
    print(f"{'TOTAL':<50} {total_lines:>6} lines")
    
    return 0

if __name__ == '__main__':
    sys.exit(main())

