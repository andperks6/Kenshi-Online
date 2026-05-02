"""
Fix kenshi-online.mod game starts.

The mod accidentally wires large player squads into starts:

- Singleplayer includes "30-kenshi-online.mod" in its squad list, which points
  at the copied startoff squad.
- That copied startoff squad references "19-kenshi-online.mod" with a quantity
  of 20, causing 20 Player 1 characters to spawn.

Patch both issues while keeping the binary FCS structure size-stable.
"""

import struct
import shutil
import os

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
MOD_PATHS = [
    os.path.join(REPO_ROOT, 'kenshi-online.mod'),
    os.path.join(REPO_ROOT, 'dist', 'kenshi-online.mod'),
]

def patch_singleplayer_extra_squad(data, path):
    needle = b'30-kenshi-online.mod'

    # Find the first occurrence: Singleplayer game start's squad list.
    idx = data.find(needle)
    if idx == -1:
        print(f"  SKIP: No '30-kenshi-online.mod' found in {path}")
        return False

    count_offset = idx - 8
    count_val = struct.unpack_from('<I', data, count_offset)[0]

    print(f"  Singleplayer extra squad at 0x{idx:04X}, squad count = {count_val}")

    if count_val < 2:
        print(f"  SKIP: Singleplayer squad count already {count_val}")
        return False

    struct.pack_into('<I', data, count_offset, count_val - 1)

    # Null out the removed reference length prefix + string. The remaining
    # vanilla squad reference stays in place and the record length is unchanged.
    length_offset = idx - 4
    for i in range(length_offset, idx + len(needle)):
        data[i] = 0

    print(f"  Patched Singleplayer squad count {count_val} -> {count_val - 1}")
    return True


def patch_startoff_squad_quantity(data, path):
    # Stable local pattern:
    #   "squad" count=1 len=20 "19-kenshi-online.mod" quantity=20
    pattern = (
        struct.pack('<I', 5) + b'squad' +
        struct.pack('<I', 1) +
        struct.pack('<I', 20) + b'19-kenshi-online.mod' +
        struct.pack('<I', 20)
    )
    idx = data.find(pattern)
    if idx == -1:
        already = (
            struct.pack('<I', 5) + b'squad' +
            struct.pack('<I', 1) +
            struct.pack('<I', 20) + b'19-kenshi-online.mod' +
            struct.pack('<I', 1)
        )
        if data.find(already) != -1:
            print("  SKIP: startoff squad quantity already 1")
            return False
        print(f"  WARN: startoff squad quantity pattern not found in {path}")
        return False

    quantity_offset = idx + len(pattern) - 4
    struct.pack_into('<I', data, quantity_offset, 1)
    print(f"  Patched startoff squad quantity at 0x{quantity_offset:04X}: 20 -> 1")
    return True


def patch_mod(path):
    if not os.path.exists(path):
        print(f"  SKIP: {path} not found")
        return False

    with open(path, 'rb') as f:
        data = bytearray(f.read())

    changed = False
    changed |= patch_singleplayer_extra_squad(data, path)
    changed |= patch_startoff_squad_quantity(data, path)
    if not changed:
        return False

    # Backup original
    backup_path = path + '.bak'
    if not os.path.exists(backup_path):
        shutil.copy2(path, backup_path)
        print(f"  Backup saved to {backup_path}")

    with open(path, 'wb') as f:
        f.write(data)

    print(f"  FIXED: {path}")
    return True

if __name__ == '__main__':
    print("=== Fixing kenshi-online.mod game start player counts ===\n")

    patched = 0
    for path in MOD_PATHS:
        print(f"Checking: {path}")
        if patch_mod(path):
            patched += 1
        print()

    if patched > 0:
        print(f"Done! Patched {patched} file(s).")
        print("Singleplayer no longer includes the copied player squad, and the copied startoff squad quantity is 1.")
    else:
        print("No files needed patching.")
