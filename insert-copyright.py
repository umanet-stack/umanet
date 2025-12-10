import os

HEADER = """
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Your Name
 */
"""

dirs_to_scan = ["src", "include"]

for dir_path in dirs_to_scan:
    for root, _, files in os.walk(dir_path):
        for f in files:
            if f.endswith((".c", ".h")):
                path = os.path.join(root, f)
                with open(path, "r+", encoding="utf-8") as file:
                    content = file.read()
                    if "SPDX-License-Identifier" not in content:
                        file.seek(0, 0)
                        file.write(HEADER + "\n" + content)
                        print(f"Updated {path}")
