import pathlib
import struct
import subprocess
import sys
import tempfile


def run(parser, helper):
    original = pathlib.Path(helper).read_bytes()
    headers = []
    phoff = struct.unpack_from("<Q", original, 32)[0]
    size, count = struct.unpack_from("<HH", original, 54)
    for index in range(count):
        headers.append(struct.unpack_from("<IIQQQQQQ", original, phoff + size * index))

    def offset(address):
        for kind, flags, file_offset, virtual, _, length, _, _ in headers:
            if kind == 1 and virtual <= address < virtual + length:
                return file_offset + address - virtual
        raise ValueError("fixture metadata outside file")

    dynamic = next(header for header in headers if header[0] == 2)
    tags = {}
    for position in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack_from("<qQ", original, position)
        if tag == 0:
            break
        tags.setdefault(tag, (position, value))
    subprocess.run([parser, helper, "accept"], check=True)
    gnu_offset = offset(tags[0x6FFFFEF5][1])
    bucket_count, symbol_offset, bloom_count, _ = struct.unpack_from("<IIII", original, gnu_offset)
    buckets_offset = gnu_offset + 16 + bloom_count * 8
    first_bucket = next(index for index in range(bucket_count)
                        if struct.unpack_from("<I", original, buckets_offset + index * 4)[0])
    first_symbol = struct.unpack_from("<I", original, buckets_offset + first_bucket * 4)[0]
    chain_offset = buckets_offset + bucket_count * 4 + (first_symbol - symbol_offset) * 4
    chain_value = struct.unpack_from("<I", original, chain_offset)[0]
    need_offset = offset(tags[0x6FFFFFFE][1])
    need_aux = struct.unpack_from("<I", original, need_offset + 8)[0]
    need_hash = struct.unpack_from("<I", original, need_offset + need_aux)[0]
    sysv_offset = offset(tags[4][1])
    sysv_buckets, sysv_symbols = struct.unpack_from("<II", original, sysv_offset)
    strings_offset = offset(tags[5][1])
    symbols_offset = offset(tags[6][1])

    def sysv_hash(name):
        value = 0
        for character in name:
            value = ((value << 4) + character) & 0xFFFFFFFF
            high = value & 0xF0000000
            value ^= high >> 24
            value &= ~high
        return value

    wrong_bucket_symbol = None
    for index in range(1, sysv_symbols):
        name_offset = struct.unpack_from("<I", original, symbols_offset + index * 24)[0]
        name = original[strings_offset + name_offset:].split(b"\0", 1)[0]
        if sysv_hash(name) % sysv_buckets != 0:
            wrong_bucket_symbol = index
            break
    if wrong_bucket_symbol is None:
        raise ValueError("fixture lacks a symbol for semantic SysV bucket mutation")
    mutations = {
        "wrong_machine": (18, "<H", 183),
        "program_header_overflow": (32, "<Q", 2**64 - 1),
        "string_table_hole": (tags[5][0] + 8, "<Q", 2**63),
        "string_table_limit": (tags[10][0] + 8, "<Q", 64 * 1024 * 1024 + 1),
        "symbol_entry_size": (tags[11][0] + 8, "<Q", 23),
        "gnu_hash_limit": (offset(tags[0x6FFFFEF5][1]), "<I", 1048577),
        "gnu_hash_bloom_zero": (offset(tags[0x6FFFFEF5][1]) + 8, "<I", 0),
        "gnu_hash_wrong_name": (chain_offset, "<I", chain_value ^ 2),
        "sysv_bucket_wrong_name": (sysv_offset + 8, "<I", wrong_bucket_symbol),
        "sysv_chain_outside": (sysv_offset + 8 + sysv_buckets * 4 + wrong_bucket_symbol * 4, "<I", sysv_symbols),
        "gnu_bloom_missing_bits": (gnu_offset + 16, "<Q", 0),
        "version_name_hash_mismatch": (need_offset + need_aux, "<I", need_hash ^ 1),
        "version_count_limit": (tags[0x6FFFFFFF][0] + 8, "<Q", 65537),
        "version_chain_cycle": (offset(tags[0x6FFFFFFE][1]) + 12, "<I", 1),
        "relocation_limit": (tags[8][0] + 8, "<Q", 24 * 262145),
        "unknown_relocation": (offset(tags[7][1]) + 8, "<Q", 37),
        "relative_symbol_nonzero": (offset(tags[7][1]) + 8, "<Q", (1 << 32) | 8),
        "dependency_string_outside": (tags[1][0] + 8, "<Q", 2**63),
        "nodelete_flag": (tags[0x6FFFFFFB][0] + 8, "<Q", 8),
    }
    with tempfile.TemporaryDirectory(prefix="spark-elf-admission-") as directory:
        for name, (position, fmt, value) in mutations.items():
            data = bytearray(original)
            struct.pack_into(fmt, data, position, value)
            path = pathlib.Path(directory) / f"{name}.so"
            path.write_bytes(data)
            subprocess.run([parser, str(path), "reject"], check=True)
    print(f"PASS: {len(mutations)} malformed ELF admission fixtures")


if __name__ == "__main__":
    run(*sys.argv[1:])
