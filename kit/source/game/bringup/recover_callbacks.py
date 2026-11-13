"""Lift measured missing callbacks without changing existing function ownership.

Run from any directory with py -3 <path-to-this-script>.
The original disassembly and generated chunks are left intact. Add subsequent
measured entries, with verified bounds, to callbacks.json and rerun this script.
"""
import json
import os
import sys
import re
import bisect
import struct
import shutil
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
ROOT = PROJECT.parent
sys.path.insert(0, str(ROOT))
os.chdir(ROOT)

from tools.recomp import config
from tools.recomp.translator import BatchTranslator

xbe = PROJECT / "game_files/ma2.xbe"
config.configure_from_xbe(str(xbe))
batch = BatchTranslator(
    str(xbe), str(ROOT / "tools/disasm/output/functions.json"),
    labels_json_path=str(ROOT / "tools/disasm/output/labels.json"),
    identified_json_path=str(ROOT / "tools/func_id/output/identified_functions.json"),
    abi_json_path=str(ROOT / "tools/abi_analysis/output/abi_functions.json"),
)
# Batch analysis discovers additional candidate starts. They are not callable
# in this existing build until emitted: stopping at one silently drops code.
# Only the original dispatch table establishes ownership for the supplement.
emitted={int(v,16) for v in re.findall(r"\{ 0x([0-9A-F]{8})u, \(recomp_func_t\)",
    (PROJECT / "src/recomp/gen/recomp_dispatch.c").read_text())}
for addr in set(batch.func_db)-emitted:
    del batch.func_db[addr]
batch.translator.owned_function_starts.intersection_update(emitted)
entries = json.loads((PROJECT / "bringup/callbacks.json").read_text())
# Unknown direct branches in a recovered block can name another shared tail.
# Decode that dependency from the original executable too; a placeholder would
# discard its epilogue/register updates. Original generated owners stay intact.
stubs = {int(s,16) for s in re.findall(r"void sub_([0-9A-F]{8})\(void\)",
    (PROJECT / "src/recomp/gen/recomp_stubs_unresolved.c").read_text())}
candidate_starts=set()
def bounded_entry(addr, note):
    starts=sorted(set(batch.func_db)|stubs|candidate_starts|{int(e["start"],16) for e in entries})
    ix=bisect.bisect_right(starts,addr)
    section=next((v for v in config._SECTIONS if v.is_code and v.va<=addr<v.va+v.raw_size),None)
    if section is None: raise ValueError(f"Dependency {addr:08X} is not code")
    upper=min(section.va+section.raw_size,addr+0x10000,starts[ix] if ix<len(starts) else 0xFFFFFFFF)
    result=batch.translator._recover_cfg(addr,upper,set(),set(starts)-{addr})
    if not result: raise ValueError(f"Cannot recover dependency {addr:08X}")
    insns,_,_=result
    if not insns or insns[0].address!=addr or any(i.mnemonic in ("hlt","iret","iretd","ud2") for i in insns):
        raise ValueError(f"Invalid dependency CFG at {addr:08X}")
    last=insns[-1]
    if last.end_address==upper and upper not in batch.func_db and upper not in candidate_starts and not (last.mnemonic.startswith("ret") or last.mnemonic in ("jmp","int3")):
        raise ValueError(f"Unproven fallthrough at {addr:08X} -> {upper:08X}")
    return {"start":f"0x{addr:08X}","end":f"0x{max(i.end_address for i in insns):08X}","note":note}
if "--vtables" in sys.argv:
    original=PROJECT/"bringup/originals/callbacks_before_vtables.json"
    if not original.exists():shutil.copy2(PROJECT/"bringup/callbacks.json",original)
    image=xbe.read_bytes()
    identified=json.loads((ROOT/"tools/func_id/output/identified_functions.json").read_text())
    stored_tables=set()
    for chunk in (PROJECT/"src/recomp/gen").glob("recomp_[0-9][0-9][0-9][0-9].c"):
        stored_tables.update(int(v,16) for v in re.findall(
            r"MEM32\([^;\n]*\)\s*=\s*(0x[0-9A-Fa-f]+)u?;",chunk.read_text()))
    candidates=[]
    for item in identified:
        addr=int(item["start"],16)
        if item.get("method")!="vtable_thunk" or addr in batch.func_db or any(int(e["start"],16)==addr for e in entries):continue
        table=int(item["vtable_addr"],16)
        # Consecutive floating-point constants can also look like code
        # pointers. Require an existing lifted store of the table address,
        # which is how a constructor installs a virtual dispatch table.
        if table not in stored_tables:continue
        section=next((v for v in config._SECTIONS if v.va<=table<v.va+v.raw_size),None)
        # Code-section pointer arrays are often switch tables. This pass
        # accepts data vtables and verifies the exact on-disc pointer slot.
        if section is None or section.name not in (".rdata",".data"):continue
        slot=table+4*item["vtable_index"]
        offset=config.va_to_file_offset(slot)
        if offset is None or struct.unpack_from("<I",image,offset)[0]!=addr:continue
        candidates.append(item);candidate_starts.add(addr)
    rejected=[]
    for item in candidates:
        addr=int(item["start"],16)
        try:
            entry=bounded_entry(addr,f"Original data vtable {item['vtable_addr']} slot {item['vtable_index']}; complete bounded CFG, not the heuristic 32-byte extent.")
            entries.append(entry)
        except ValueError as error:
            rejected.append({"start":item["start"],"reason":str(error)})
            candidate_starts.discard(addr)
    (PROJECT/"bringup/vtable_recovery_rejected.json").write_text(json.dumps(rejected,indent=2)+"\n")
    print(f"Verified {len(candidates)-len(rejected)} original data-vtable targets; {len(rejected)} rejected.",flush=True)
for arg in (a for a in sys.argv[1:] if a!="--vtables"):
    addr=int(arg,16)
    if addr not in batch.func_db and not any(int(e["start"],16)==addr for e in entries):
        entries.append(bounded_entry(addr,"Observed missing indirect call; recovered from bounded original CFG."))
recovered = []
for entry in entries:
    addr = int(entry["start"], 16)
    if addr in batch.func_db:
        raise ValueError(f"{addr:08X} already exists in the base dispatch")
    info = dict(entry, end=int(entry["end"], 16), name=f"sub_{addr:08X}",
                detection_method="observed_bringup", _addr=addr)
    batch.func_db[addr] = info
    recovered.append((addr, info))

def preserve_cfg(addr, info):
    # The verified extent can overlap an older compiled entry (shared math or
    # epilogue). Decode its full reachable CFG: stopping inside that extent
    # would omit the remaining instructions without an actual guest branch.
    cfg=batch.translator._recover_cfg(addr,info["end"],set(),set())
    if not cfg or not cfg[0]: raise ValueError(f"Missing original CFG at {addr:08X}")
    batch.translator._recovered_cfg[addr]={"end":info["end"],"instructions":cfg[0],"jump_tables":cfg[1]}

header = ["/* Generated by bringup/recover_callbacks.py. */", "#pragma once",
          "#include <stdint.h>", "typedef void (*bringup_func_t)(void);",
          "bringup_func_t recomp_lookup_bringup(uint32_t va);"]
source = ["/* Generated from ma2.xbe; bounds are in bringup/callbacks.json. */",
          "#define RECOMP_GENERATED_CODE",
          '#include "recomp_types.h"', '#include "recomp_funcs.h"',
          '#include "recomp_bringup.h"']
common=list(source)
bodies=[]
def translate_complete(addr,info):
    preserve_cfg(addr,info)
    # A decoded ordinary instruction can fall through to a compiled shared
    # tail. Publish that edge explicitly rather than dropping its epilogue.
    end=info["end"]
    owned=end in batch.translator.owned_function_starts
    if owned:batch.translator.owned_function_starts.remove(end)
    try:return batch.translator.translate_function(addr,info)
    finally:
        if owned:batch.translator.owned_function_starts.add(end)
for addr, info in recovered:
    header.append(f"void sub_{addr:08X}(void);")
    result = translate_complete(addr,info)
    if not result:
        raise RuntimeError(f"Failed to lift {addr:08X}: {result}")
    bodies.append(result)
while True:
    references={int(v,16) for v in re.findall(r"\bsub_([0-9A-F]{8})\(","\n".join(bodies))}
    missing=references-set(batch.func_db)-stubs
    if not missing: break
    if len(recovered)>8192: raise ValueError("Dependency closure exceeds reviewed bound")
    for addr in sorted(missing):
        entry=bounded_entry(addr,"Direct branch dependency of a recovered callback; bounded original CFG.")
        entries.append(entry)
        info=dict(entry,end=int(entry["end"],16),name=f"sub_{addr:08X}",detection_method="observed_bringup",_addr=addr)
        batch.func_db[addr]=info
        recovered.append((addr,info))
        header.append(f"void sub_{addr:08X}(void);")
        result=translate_complete(addr,info)
        if not result: raise RuntimeError(f"Failed to lift dependency {addr:08X}")
        bodies.append(result)
source.extend(["bringup_func_t recomp_lookup_bringup(uint32_t va)", "{", "    switch (va) {"])
for addr, _ in recovered:
    source.append(f"    case 0x{addr:08X}u: return sub_{addr:08X};")
source.extend(["    default: return 0;", "    }", "}"])
gen = PROJECT / "src/recomp/gen"
for old in gen.glob("recomp_bringup_[0-9][0-9][0-9][0-9].c"):old.unlink()
for first in range(0,len(bodies),150):
    (gen/f"recomp_bringup_{first//150:04d}.c").write_text("\n\n".join(common+bodies[first:first+150])+"\n")
(gen / "recomp_bringup.h").write_text("\n".join(header) + "\n")
(gen / "recomp_bringup.c").write_text("\n\n".join(source) + "\n")
(PROJECT / "bringup/callbacks.json").write_text(json.dumps(entries,indent=2)+"\n")
print(f"Recovered {len(recovered)} callbacks and shared tails; original {len(emitted)} compiled owners retained.")
