import struct

def read_superblock(data):
    if len(data) < 1066:
        return None
    magic = data[:18].decode('ascii', errors='ignore')
    if "XYLM_SUPERBLOCK" not in magic:
        return None
    seq = struct.unpack("<Q", data[18:26])[0]
    bam_start = struct.unpack("<I", data[26:30])[0]
    bam_count = struct.unpack("<I", data[30:34])[0]
    jnl_start = struct.unpack("<I", data[34:38])[0]
    jnl_count = struct.unpack("<I", data[38:42])[0]
    return {
        "magic": magic,
        "seq": seq,
        "bam_start": bam_start,
        "bam_count": bam_count,
        "jnl_start": jnl_start,
        "jnl_count": jnl_count
    }

def main():
    block_size = 4096
    with open("/tmp/abc.xy", "rb") as f:
        content = f.read()
    
    total_blocks = len(content) // block_size
    print(f"Total blocks in file: {total_blocks} (File size: {len(content)} bytes)")
    
    superblocks = []
    for i in range(32):
        b_idx = (i * (i + 1) // 2) % total_blocks
        if b_idx * block_size + block_size <= len(content):
            sb_data = content[b_idx * block_size : b_idx * block_size + block_size]
            sb = read_superblock(sb_data)
            if sb:
                superblocks.append((b_idx, sb))
    
    if not superblocks:
        print("No valid superblocks found!")
        return
    
    for b_idx, sb in superblocks:
        print(f"Superblock at block {b_idx}: seq={sb['seq']}, bam_start={sb['bam_start']}, bam_count={sb['bam_count']}, jnl_start={sb['jnl_start']}, jnl_count={sb['jnl_count']}")
    
    # Get the latest superblock
    best_sb = max(superblocks, key=lambda x: x[1]['seq'])[1]
    print(f"\nUsing latest superblock with seq={best_sb['seq']}")
    
    # Read BAM
    bam_start = best_sb['bam_start']
    bam_count = best_sb['bam_count']
    bam_data = b""
    for i in range(bam_count):
        idx = bam_start + i
        bam_data += content[idx * block_size : idx * block_size + block_size]
    
    # Each BAM entry is 3 bytes (eraseCount: 2 bytes, packed: 1 byte)
    # packed has: status in bits 7-6, type in bits 5-2
    entries_per_block = (block_size - 8) // 3
    
    bam_entries = []
    for b in range(bam_count):
        blk_offset = b * block_size
        count = struct.unpack("<I", bam_data[blk_offset : blk_offset + 4])[0]
        if count == 0 or count > entries_per_block:
            continue
        for i in range(count):
            entry_offset = blk_offset + 4 + i * 3
            ec = struct.unpack("<H", bam_data[entry_offset : entry_offset + 2])[0]
            packed = bam_data[entry_offset + 2]
            status = packed >> 6
            type_val = (packed >> 2) & 0x0F
            bam_entries.append((ec, status, type_val))
            
    print("\nAllocated Blocks from BAM:")
    for b, entry in enumerate(bam_entries):
        ec, status, type_val = entry
        status_str = ["FREE", "USED", "RESERVED", "BAD"][status]
        type_str = ["FREE", "SUPER", "BAM", "JOURNAL", "SCHEMA", "TABLE", "BLOB", "DICT", "RAW", "INVALID"][type_val] if type_val < 10 else "UNKNOWN"
        if status != 0:
            magic_info = ""
            if status == 1 and type_val == 6: # BLOB
                if b * block_size + block_size <= len(content):
                    blk_data = content[b * block_size : b * block_size + block_size]
                    if len(blk_data) > 3:
                        blk_type = blk_data[0]
                        is_first = blk_data[1]
                        key_len = blk_data[2]
                        if is_first in (1, 2) and 0 < key_len < 200:
                            key = blk_data[3:3+key_len].decode('ascii', errors='ignore')
                            magic_info = f" -> Blob key: '{key}' (isFirst={is_first})"
            print(f"  Block {b}: status={status_str}, type={type_str}, ec={ec}{magic_info}")

if __name__ == "__main__":
    main()
