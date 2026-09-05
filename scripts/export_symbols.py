# Runs inside IDA (the database is on the Windows side, so it writes to a Windows path and the repo
# copy is taken from there). Writes the function symbol map the hooks resolve names against, in the
# "name = 0xADDR;" form pcsx2-hook reads.
#
import ida_bytes
import idautils
import idc

# Named for the disc serial, which is what the runtime knows. The ELF name would have to be
# derived from it, and the two differ in punctuation for no useful reason.
SERIAL = "SLUS-20911"
OUT = rf"C:\Users\Chris\AppData\Local\Temp\{SERIAL}.map"

with open(OUT, "w") as out:
    count = 0
    for ea in idautils.Functions():
        name = idc.get_func_name(ea)
        if name:
            out.write(f"{name} = 0x{ea:08X};\n")
            count += 1

    # Named data too. The lookups this serves are by name, and the address ranges do not overlap
    # code, so a caller address still resolves to the function that covers it.
    data = 0
    for ea, name in idautils.Names():
        if ida_bytes.is_code(ida_bytes.get_flags(ea)):
            continue
        if name.startswith(("unk_", "byte_", "word_", "dword_", "qword_", "asc_", "off_", "flt_",
                            "dbl_", "stru_", "xmmword_", "loc_", "sub_", "nullsub_", "jpt_")):
            continue
        out.write(f"{name} = 0x{ea:08X};\n")
        data += 1

print(f"wrote {count} functions and {data} data symbols to {OUT}")
