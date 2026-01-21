#!/bin/bash
# Generate address-to-line mapping from DWARF debug info
# Usage: gen_addrline.sh <objdump> <elf_file> <output_file>
#
# Output format:
#   <file name>:
#   :<symbol>
#   <start address> <end address(not include)> <line number>
#   ...

set -e

OBJDUMP="$1"
ELF_FILE="$2"
OUTPUT_FILE="$3"

if [ $# -ne 3 ]; then
    echo "Usage: $0 <objdump> <elf_file> <output_file>" >&2
    exit 1
fi

# Extract symbols sorted numerically by address, and DWARF line info
# Then merge them in a single awk pass with binary search for O(n log m) complexity

"$OBJDUMP" -t "$ELF_FILE" | awk '/^[0-9a-f]+ / && $NF !~ /^\./ {print "S", $1, $NF}' | \
    sort -k2 | \
    cat - <("$OBJDUMP" --dwarf=decodedline "$ELF_FILE" | awk 'NF >= 3 && $2 ~ /^[0-9]+$/ {print "L", $3, $1, $2}') | \
    awk '
BEGIN {
    nsym = 0
    nentries = 0
}

# Load symbols (S lines come first due to sort order, 0-9 < L)
$1 == "S" {
    sym_addr[nsym] = strtonum("0x" $2)
    sym_name[nsym] = $3
    nsym++
    next
}

# Process line entries (L lines)
$1 == "L" {
    entries_addr[nentries] = $2
    entries_file[nentries] = $3
    entries_line[nentries] = $4
    nentries++
}

# Binary search: find largest symbol address <= target
function find_symbol(addr,    lo, hi, mid) {
    if (nsym == 0) return ""
    lo = 0
    hi = nsym - 1
    # Find rightmost symbol with address <= addr
    while (lo < hi) {
        mid = int((lo + hi + 1) / 2)
        if (sym_addr[mid] <= addr) {
            lo = mid
        } else {
            hi = mid - 1
        }
    }
    if (sym_addr[lo] <= addr) return sym_name[lo]
    return ""
}

END {
    current_file = ""
    current_sym = ""
    
    for (i = 0; i < nentries; i++) {
        file = entries_file[i]
        line = entries_line[i]
        addr = entries_addr[i]
        addr_num = strtonum(addr)
        sym = find_symbol(addr_num)
        
        # Determine end address
        if (i + 1 < nentries) {
            end_addr = entries_addr[i + 1]
        } else {
            end_addr = addr
        }
        
        # Print file header if changed
        if (file != current_file) {
            if (current_file != "") print ""
            print file ":"
            current_file = file
            current_sym = ""
        }
        
        # Print symbol header if changed
        if (sym != current_sym) {
            print ":" sym
            current_sym = sym
        }
        
        # Print start address, end address, line number (strip 0x prefix)
        start_hex = addr
        end_hex = end_addr
        sub(/^0x/, "", start_hex)
        sub(/^0x/, "", end_hex)
        printf "%s %s %s\n", start_hex, end_hex, line
    }
}
' > "$OUTPUT_FILE"
