"""Convert dumped mesh shaders into the compute + vertex shader pair the meshemu proxy runs.

    python -I convert.py <dump dir> <cache dir>

ABI v9: the v7 design (see docs/DESIGN.md) with native 16-bit loads lowered (lower_native16) and a 256 MB
scratch buffer. For every <ms>_<ps>[...].pair in the dump dir:
  VS mode, when vs_mode_blocker() allows it (thread t writes only vertex t, no cross-thread communication):
    <cache>/<ms>_<ps>.cs9.dxil   mesh shader as a compute shader that stores only indices + draw arguments
    <cache>/<ms>_<ps>.vs9.dxil   the mesh shader itself as the vertex shader (convert_ms_to_vs); vertices
                                 never go through scratch
  otherwise the v5 scheme (CS stores packed vertices, generated pass-through VS), under the same names.
  <cache>/<ms>_<ps>.meta9      "maxVerts maxPrims vertexBytes maxGroups idxRegion" (vertexBytes 0 in VS mode)
  MESHEMU_NO_VS_MODE=1 forces the fallback for every pair (for A/B tests).

Scratch layout for one emulated draw (group G = linear SV_GroupID):
  [0, idxRegion)            uint3 indices[maxGroups][maxPrims], global (G * maxVerts + local); unused = degenerate
  [idxRegion, ...)          vertices[maxGroups * maxVerts], vertexBytes each (kept outputs, see plan_layout)
  [SCRATCH_CAP, +20)        DrawIndexed arguments, written by group 0 thread 0
  [SCRATCH_CAP + 256, +1MB) trash: writes of groups beyond maxGroups
The CS reads the dispatch size {x, y, z} from the game's own argument buffer (t1 space999), so the proxy can
ExecuteIndirect it with the game's arguments directly and then ExecuteIndirect(DRAW_INDEXED) from scratch.
"""
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DXC = os.path.join(HERE, "dxc.exe")
DXASM = os.path.join(HERE, "dxasm.exe")
EMU_SPACE = 999
SCRATCH_CAP = 256 << 20  # must match kScratchCap in proxy.cpp (indices only in VS mode: 220 MB was the largest draw seen)
DRAW_ARGS = SCRATCH_CAP  # DrawIndexed arguments for the emulated draw
TRASH = SCRATCH_CAP + 256

# DXIL enums
COMP_TYPES = {4: "int", 5: "uint", 8: "min16float", 9: "float", 2: "min16int", 3: "min16uint", 1: "bool"}
INTERP = {0: "", 1: "nointerpolation", 2: "", 3: "centroid", 4: "noperspective", 5: "noperspective centroid",
          6: "sample", 7: "noperspective sample"}
SYSTEM_GENERATED_PS_INPUTS = {"SV_PrimitiveID", "SV_SampleIndex", "SV_IsFrontFace", "SV_Coverage",
                              "SV_InnerCoverage", "SV_Barycentrics", "SV_ViewID", "SV_ShadingRate"}


class ConvertError(Exception):
    pass


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise ConvertError(f"{os.path.basename(cmd[0])} failed:\n{r.stdout}\n{r.stderr}")
    return r.stdout


def disassemble(dxil_path):
    out = dxil_path + ".ll"
    run([DXC, "-dumpbin", dxil_path, "-Fc", out])
    with open(out) as f:
        return f.read()


# ------------------------------------------------------------------------------------------------
# LLVM text helpers
# ------------------------------------------------------------------------------------------------

class Module:
    def __init__(self, text):
        self.lines = text.split("\n")
        self.md = {}  # id -> body text
        for ln in self.lines:
            m = re.match(r"^!(\d+) = (.*)$", ln)
            if m:
                self.md[int(m.group(1))] = m.group(2)
        self.next_md = max(self.md) + 1 if self.md else 0

    def named_md(self, name):
        for ln in self.lines:
            m = re.match(rf"^!{re.escape(name)} = !\{{(.*)\}}$", ln)
            if m:
                return m.group(1)
        return None

    def set_md(self, i, body):
        self.md[i] = body
        for k, ln in enumerate(self.lines):
            if ln.startswith(f"!{i} = "):
                self.lines[k] = f"!{i} = {body}"
                return
        raise ConvertError(f"metadata !{i} not found")

    def add_md(self, body):
        i = self.next_md
        self.next_md += 1
        self.md[i] = body
        self.lines.append(f"!{i} = {body}")
        return i

    def text(self):
        return "\n".join(self.lines)


def md_items(body):
    """Split a metadata tuple body '!{a, b, c}' into top-level items."""
    body = body.strip()
    if body == "null":
        return None
    assert body.startswith("!{") and body.endswith("}"), body
    inner = body[2:-1]
    items, depth, cur = [], 0, ""
    for ch in inner:
        if ch in "{[(<":
            depth += 1
        elif ch in "}])>":
            depth -= 1
        if ch == "," and depth == 0:
            items.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        items.append(cur.strip())
    return items


def md_ref(item):
    m = re.fullmatch(r"!(\d+)", item.strip())
    return int(m.group(1)) if m else None


def int_val(item):
    return int(item.split()[-1])


def parse_sig_elements(mod, list_ref):
    """Signature element list -> list of dicts."""
    if list_ref is None:
        return []
    elems = []
    for r in md_items(mod.md[list_ref]):
        it = md_items(mod.md[md_ref(r)])
        name = re.fullmatch(r'!"(.*)"', it[1]).group(1)
        sem_idx = [int_val(x) for x in md_items(mod.md[md_ref(it[4])])]
        elems.append(dict(id=int_val(it[0]), name=name, comp=int_val(it[2]), kind=int_val(it[3]),
                          sem_idx=sem_idx, interp=int_val(it[5]), rows=int_val(it[6]), cols=int_val(it[7]),
                          start_row=int_val(it[8]), start_col=int_val(it[9])))
    return elems


def entry_info(mod):
    ep = md_items(mod.named_md("dx.entryPoints") and "!{" + mod.named_md("dx.entryPoints") + "}")
    eid = md_ref(ep[0])
    items = md_items(mod.md[eid])
    sig = md_items(mod.md[md_ref(items[2])]) if md_ref(items[2]) is not None else [None, None, None]
    return eid, items, sig


def shader_kind(mod):
    m = re.search(r'!\{!"(\w+)", i32 (\d+), i32 (\d+)\}', mod.md[md_ref(mod.named_md("dx.shaderModel"))])
    return m.group(1), int(m.group(2)), int(m.group(3))


def attr_group(mod, want):
    for ln in mod.lines:
        m = re.match(r"^attributes #(\d+) = \{ (.*) \}$", ln)
        if m and m.group(2) == want:
            return int(m.group(1))
    nums = [int(m.group(1)) for ln in mod.lines for m in [re.match(r"^attributes #(\d+)", ln)] if m]
    n = max(nums) + 1 if nums else 0
    mod.lines.append(f"attributes #{n} = {{ {want} }}")
    return n


def ensure_type(mod, name, body):
    if not any(ln.startswith(f"{name} = type") for ln in mod.lines):
        # Insert after the last existing type definition.
        idx = max((k for k, ln in enumerate(mod.lines) if re.match(r"^%[\w.\"<>, ]+ = type", ln)), default=2)
        mod.lines.insert(idx + 1, f"{name} = type {body}")


def drop_unused_decls(lines):
    """The validator rejects declared but unused external functions (e.g. a store type a rewrite no longer
    emits): drop every declaration whose function is not called."""
    used = set()
    for ln in lines:
        if not ln.startswith("declare"):
            used.update(re.findall(r"@([\w.$]+)\(", ln))
    return [ln for ln in lines if not ln.startswith("declare") or
            re.search(r"@([\w.$]+)\(", ln).group(1) in used]


def ensure_decl(mod, name, decl):
    if not any(f"@{name}(" in ln and ln.startswith("declare") for ln in mod.lines):
        mod.lines.append(decl)


# ------------------------------------------------------------------------------------------------
# Mesh shader -> compute shader
# ------------------------------------------------------------------------------------------------

def align16(x):
    return (x + 15) & ~15


ALWAYS_KEPT = ("SV_POSITION", "SV_CULLDISTANCE", "SV_CLIPDISTANCE")


def kept_outputs(outputs, ps_inputs):
    """Mesh shader outputs worth storing: what the paired PS reads, plus position and clip/cull distances."""
    wanted = {(e["name"].upper(), e["sem_idx"][0]) for e in ps_inputs or []
              if e["name"] not in SYSTEM_GENERATED_PS_INPUTS}
    return [e for e in outputs
            if e["name"].upper() in ALWAYS_KEPT or (e["name"].upper(), e["sem_idx"][0]) in wanted]


def row_bytes(e):
    """Bytes one row of a signature element occupies in the vertex. Rows of multi-row 3-component elements
    are padded to 16 so that every row stays 16-byte aligned."""
    return 16 if e["cols"] == 3 and e["rows"] > 1 else 4 * e["cols"]


def plan_layout(outputs, ps_inputs):
    """Vertex layout for aligned vector stores and loads: 16-byte elements first, then each 12-byte element
    paired with a 4-byte one (or padded) to complete a 16-byte unit, then 8-byte, then 4-byte elements; so
    every element starts at a multiple of its row size. The stride is a multiple of 8, or of 16 when that
    costs at most 4 bytes. Returns (layout {sig id: byte offset}, vertex_bytes)."""
    kept = kept_outputs(outputs, ps_inputs)
    by_size = {16: [], 12: [], 8: [], 4: []}
    for e in kept:
        by_size[row_bytes(e)].append(e)
    order = list(by_size[16])
    for e in by_size[12]:
        order.append(e)
        order.append(by_size[4].pop(0) if by_size[4] else None)  # None: 4 bytes of padding
    order += by_size[8] + by_size[4]
    layout, off = {}, 0
    for e in order:
        if e is None:
            off += 4
            continue
        layout[e["id"]] = off
        off += e["rows"] * row_bytes(e)
    s8, s16 = (off + 7) & ~7, (off + 15) & ~15
    return layout, max(s16 if s16 - off <= 4 else s8, 4)


def alignment(off, stride):
    """Largest power of two (<= 16) dividing both off and stride: the known alignment of vertex + off."""
    a = 16
    while a > 4 and (off % a or stride % a):
        a //= 2
    return a


def call_post_dominates(body, call_re):
    """True if the function body (list of lines inside define..}) has exactly one call matching call_re and
    every path from the entry block to a return passes through its basic block. Terminator successors are
    read from br/switch lines; an unreachable or unknown terminator is treated as a return."""
    blocks, cur, succ, rets = ["%0"], "%0", {"%0": []}, set()
    call_block, calls = None, 0
    for ln in body:
        m = re.match(r"^; <label>:(\d+)", ln) or re.match(r"^([\w.$]+):", ln)
        if m:
            cur = "%" + m.group(1)
            blocks.append(cur)
            succ[cur] = []
            continue
        if re.search(call_re, ln):
            calls += 1
            call_block = cur
        s = ln.strip()
        if s.startswith("br ") or s.startswith("switch ") or s.startswith("indirectbr "):
            succ[cur] = re.findall(r"label (%[\w.$]+)", s)
        elif s.startswith("ret") or s.startswith("unreachable"):
            rets.add(cur)
    if calls != 1 or call_block is None:
        return False
    seen, todo = {"%0"}, ["%0"]
    while todo:
        b = todo.pop()
        if b == call_block:
            continue  # paths through the call block are fine
        if b in rets:
            return False
        for n in succ.get(b, []):
            if n not in seen:
                seen.add(n)
                todo.append(n)
    return True


def convert_ms(ms_ll, ps_inputs, index_only=False):
    """index_only (ABI v7 VS mode): store no vertex outputs, only indices + draw arguments; the vertices are
    computed by the mesh shader itself running as the vertex shader (convert_ms_to_vs)."""
    mod = Module(ms_ll)
    kind, smaj, smin = shader_kind(mod)
    if kind != "ms":
        raise ConvertError(f"not a mesh shader ({kind})")
    if (smaj, smin) < (6, 6):
        raise ConvertError("mesh shaders below SM 6.6 not supported yet")
    for bad in ("getMeshPayload", "storePrimitiveOutput"):
        if f"@dx.op.{bad}" in ms_ll:
            raise ConvertError(f"unsupported op {bad}")

    eid, eitems, sig = entry_info(mod)
    outputs = parse_sig_elements(mod, md_ref(sig[1]) if sig[1] != "null" else None)
    if not outputs:
        raise ConvertError("no vertex outputs")
    layout, vertex_bytes = ({}, 0) if index_only else plan_layout(outputs, ps_inputs)

    props = md_items(mod.md[md_ref(eitems[4])])
    tags = list(zip(props[0::2], props[1::2]))
    ms_state = None
    flags = 0
    other_props = []
    for tag, val in tags:
        t = int_val(tag)
        if t == 9:    # mesh state: {numthreads, maxVerts, maxPrims, topology, payload}
            ms_state = md_items(mod.md[md_ref(val)])
        elif t == 0:  # shader flags
            flags = int_val(val)
        else:
            other_props += [tag, val]
    if ms_state is None:
        raise ConvertError("no mesh shader state")
    # Compute: shader flags (+ raw/structured buffers for the scratch UAV), numthreads from the MS state.
    new_props = ["i32 0", f"i64 {flags | 0x10}", "i32 4", ms_state[0]] + other_props
    max_verts, max_prims = int_val(ms_state[1]), int_val(ms_state[2])
    if int_val(ms_state[3]) != 2:
        raise ConvertError("only triangle topology supported")
    nthreads = 1
    for x in md_items(mod.md[md_ref(ms_state[0])]):
        nthreads *= int_val(x)
    idx_bytes = max_prims * 12
    vtx_bytes = max_verts * vertex_bytes
    max_groups = SCRATCH_CAP // (idx_bytes + vtx_bytes)
    idx_region = (max_groups * idx_bytes + 255) & ~255
    while idx_region + max_groups * vtx_bytes > SCRATCH_CAP:
        max_groups -= 1

    # Metadata: shader model, entry (no signatures, no MS state), resources (+UAV, +CBV).
    sm_id = md_ref(mod.named_md("dx.shaderModel"))
    mod.set_md(sm_id, f'!{{!"cs", i32 {smaj}, i32 {smin}}}')
    props_id = mod.add_md("!{" + ", ".join(new_props) + "}")

    ensure_type(mod, "%struct.RWByteAddressBuffer", "{ i32 }")
    ensure_type(mod, "%struct.ByteAddressBuffer", "{ i32 }")
    ensure_type(mod, "%dx.types.ResRet.i32", "{ i32, i32, i32, i32, i32 }")

    res_ref = md_ref(eitems[3])
    if res_ref is None:
        res_ref = mod.add_md("!{null, null, null, null}")
        # dx.resources named node must exist too
        mod.lines.append(f"!dx.resources = !{{!{res_ref}}}")
    res = md_items(mod.md[res_ref])

    def append_res(slot, entry_fmt):
        lst = md_items(mod.md[md_ref(res[slot])]) if res[slot] != "null" else []
        rid = len(lst)
        nid = mod.add_md(entry_fmt.format(id=rid))
        lst.append(f"!{nid}")
        res[slot] = f"!{mod.add_md('!{' + ', '.join(lst) + '}')}"

    append_res(1, "!{{i32 {id}, %struct.RWByteAddressBuffer* undef, !\"\", i32 " + str(EMU_SPACE) +
               ", i32 0, i32 1, i32 11, i1 false, i1 false, i1 false, null}}")
    append_res(0, "!{{i32 {id}, %struct.ByteAddressBuffer* undef, !\"\", i32 " + str(EMU_SPACE) +
               ", i32 1, i32 1, i32 11, i32 0, null}}")
    mod.set_md(res_ref, "!{" + ", ".join(res) + "}")
    eitems[2] = "null"
    eitems[3] = f"!{res_ref}"
    eitems[4] = f"!{props_id}"
    mod.set_md(eid, "!{" + ", ".join(eitems) + "}")
    mod.lines = [ln for ln in mod.lines if not ln.startswith("!dx.viewIdState")]

    # Unused primitive slots must hold degenerate triangles. When the single SetMeshOutputCounts call is on
    # every path (always, in practice), each thread fills the slots [numPrims, maxPrims) right there: no
    # group barrier and no slot written twice. Otherwise fall back to pre-filling every slot at entry.
    entry_fn = re.search(r"@([\w.$]+)", eitems[0]).group(1)
    start = next(k for k, ln in enumerate(mod.lines) if ln.startswith(f"define void @{entry_fn}()"))
    end = next(k for k in range(start, len(mod.lines)) if mod.lines[k].startswith("}"))
    tail_fill = call_post_dominates(mod.lines[start + 1:end], r"@dx\.op\.setMeshOutputCounts\(")

    # Declarations.
    nounwind = attr_group(mod, "nounwind")
    readnone = attr_group(mod, "nounwind readnone")
    readonly = attr_group(mod, "nounwind readonly")
    ensure_decl(mod, "dx.op.rawBufferStore.i32",
                f"declare void @dx.op.rawBufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8, i32) #{nounwind}")
    ensure_decl(mod, "dx.op.rawBufferStore.f32",
                f"declare void @dx.op.rawBufferStore.f32(i32, %dx.types.Handle, i32, i32, float, float, float, float, i8, i32) #{nounwind}")
    ensure_decl(mod, "dx.op.rawBufferLoad.i32",
                f"declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) #{readonly}")
    ensure_decl(mod, "dx.op.groupId.i32", f"declare i32 @dx.op.groupId.i32(i32, i32) #{readnone}")
    ensure_decl(mod, "dx.op.flattenedThreadIdInGroup.i32",
                f"declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #{readnone}")
    if not tail_fill:
        nodup = attr_group(mod, "noduplicate nounwind")
        ensure_decl(mod, "dx.op.barrier", f"declare void @dx.op.barrier(i32, i32) #{nodup}")
    ensure_decl(mod, "dx.op.createHandleFromBinding",
                f"declare %dx.types.Handle @dx.op.createHandleFromBinding(i32, %dx.types.ResBind, i32, i1) #{readnone}")
    ensure_decl(mod, "dx.op.annotateHandle",
                f"declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #{readnone}")
    ensure_type(mod, "%dx.types.ResBind", "{ i32, i32, i32, i8 }")
    ensure_type(mod, "%dx.types.ResourceProperties", "{ i32, i32 }")
    mod.lines = [ln for ln in mod.lines
                 if not re.match(r"^declare .*@dx\.op\.(setMeshOutputCounts|emitIndices|storeVertexOutput)", ln)]

    by_id = {e["id"]: e for e in outputs}
    out, counter = [], [0]

    def tmp():
        counter[0] += 1
        return f"%emu.t{counter[0]}"

    def addr_expr(lines, base, parts_const, parts_dyn):
        """Emit base + const + sum(dyn) and return the value name."""
        cur = base
        for d in parts_dyn:
            t = tmp()
            lines.append(f"  {t} = add i32 {cur}, {d}")
            cur = t
        if parts_const:
            t = tmp()
            lines.append(f"  {t} = add i32 {cur}, {parts_const}")
            cur = t
        return cur

    fill_steps = (max_prims + nthreads - 1) // nthreads

    def degenerate_fill(tag, lo):
        """Store a degenerate triangle into every primitive slot p of this group with lo <= p < maxPrims
        (lo: an i32 value, or None for 0); one slot per thread per step, unrolled. With lo, each step is an
        if-block: a thread whose slot is used stores nothing (redirecting its store to the trash address
        instead put nearly every thread of the dispatch on one L2 line, which was slower than v4's
        pre-fill). The rest of the current basic block then continues in %emu.<tag>n<last step>."""
        lines = []
        for j in range(fill_steps):
            p = f"%emu.{tag}p{j}"
            cond = f"%emu.{tag}in{j}"
            lines += [f"  {p} = add i32 %emu.tid, {j * nthreads}", f"  {cond} = icmp ult i32 {p}, {max_prims}"]
            if lo is not None:
                lines += [f"  %emu.{tag}ge{j} = icmp uge i32 {p}, {lo}",
                          f"  %emu.{tag}k{j} = and i1 {cond}, %emu.{tag}ge{j}",
                          f"  br i1 %emu.{tag}k{j}, label %emu.{tag}s{j}, label %emu.{tag}n{j}",
                          f"emu.{tag}s{j}:",
                          f"  %emu.{tag}a{j}x = mul i32 {p}, 12",
                          f"  %emu.{tag}a{j} = add i32 %emu.ib, %emu.{tag}a{j}x"]
            else:
                lines += [f"  %emu.{tag}a{j}x = mul i32 {p}, 12",
                          f"  %emu.{tag}a{j}y = add i32 %emu.ib, %emu.{tag}a{j}x",
                          f"  %emu.{tag}a{j} = select i1 {cond}, i32 %emu.{tag}a{j}y, i32 {TRASH}"]
            lines.append(f"  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %emu.uav, i32 %emu.{tag}a{j}, "
                         "i32 undef, i32 %emu.gv, i32 %emu.gv, i32 %emu.gv, i32 undef, i8 7, i32 4)")
            if lo is not None:
                lines += [f"  br label %emu.{tag}n{j}", f"emu.{tag}n{j}:"]
        return lines

    # Vertex output stores of one basic block are collected per (element, row, vertex) and emitted as
    # vector stores (one per run of consecutive components) before the next other instruction.
    pending = {}

    def flush_stores():
        for (sig_id, row, vi), comps in pending.items():
            rb = row_bytes(by_id[sig_id])
            t1 = tmp()
            out.append(f"  {t1} = mul i32 {vi}, {vertex_bytes}")
            dyn, const, row_align = [t1], layout[sig_id], vertex_bytes
            if re.fullmatch(r"-?\d+", row):
                const += int(row) * rb
            else:
                t2 = tmp()
                out.append(f"  {t2} = mul i32 {row}, {rb}")
                dyn.append(t2)
                row_align = math.gcd(vertex_bytes, rb)
            runs = []
            for c in sorted(comps):
                if runs and runs[-1][-1] == c - 1:
                    runs[-1].append(c)
                else:
                    runs.append([c])
            for run in runs:
                vals, st = [], None
                for c in run:
                    ty, val = comps[c]
                    if ty == "f32":
                        sv, st = val, "f32"
                    elif ty == "i32":
                        sv, st = val, "i32"
                    elif ty == "f16":
                        sv = tmp()
                        out.append(f"  {sv} = fpext half {val} to float")
                        st = "f32"
                    elif ty == "i16":
                        sv = tmp()
                        out.append(f"  {sv} = sext i16 {val} to i32")
                        st = "i32"
                    else:
                        raise ConvertError(f"storeVertexOutput.{ty} unsupported")
                    vals.append(sv)
                vt = "float" if st == "f32" else "i32"
                a = addr_expr(out, "%emu.vb", const + run[0] * 4, dyn)
                ops = ", ".join(f"{vt} {v}" for v in vals + ["undef"] * (4 - len(vals)))
                al = alignment(const + run[0] * 4, row_align)
                out.append(f"  call void @dx.op.rawBufferStore.{st}(i32 140, %dx.types.Handle %emu.uav, i32 {a}, "
                           f"i32 undef, {ops}, i8 {(1 << len(run)) - 1}, i32 {al})")
        pending.clear()

    in_main = False
    cur_block, split_block = "%emu.cont", None  # the original entry block %0 is emitted as %emu.cont
    for ln in mod.lines:
        if ln.startswith(f"define void @{entry_fn}()"):
            in_main = True
            out.append(ln)
            out += [
                "  %emu.hu = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind "
                f"{{ i32 0, i32 0, i32 {EMU_SPACE}, i8 1 }}, i32 0, i1 false)",
                "  %emu.uav = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %emu.hu, "
                "%dx.types.ResourceProperties { i32 4107, i32 0 })",
                "  %emu.ha = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind "
                f"{{ i32 1, i32 1, i32 {EMU_SPACE}, i8 0 }}, i32 1, i1 false)",
                "  %emu.args = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %emu.ha, "
                "%dx.types.ResourceProperties { i32 11, i32 0 })",
                "  %emu.d = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %emu.args, "
                "i32 0, i32 undef, i8 7, i32 4)",
                "  %emu.dimx = extractvalue %dx.types.ResRet.i32 %emu.d, 0",
                "  %emu.dimy = extractvalue %dx.types.ResRet.i32 %emu.d, 1",
                "  %emu.dimz = extractvalue %dx.types.ResRet.i32 %emu.d, 2",
                "  %emu.gx = call i32 @dx.op.groupId.i32(i32 94, i32 0)",
                "  %emu.gy = call i32 @dx.op.groupId.i32(i32 94, i32 1)",
                "  %emu.gz = call i32 @dx.op.groupId.i32(i32 94, i32 2)",
                "  %emu.l0 = mul i32 %emu.gz, %emu.dimy",
                "  %emu.l1 = add i32 %emu.l0, %emu.gy",
                "  %emu.l2 = mul i32 %emu.l1, %emu.dimx",
                "  %emu.g = add i32 %emu.l2, %emu.gx",
                f"  %emu.ok = icmp ult i32 %emu.g, {max_groups}",
                f"  %emu.ib0 = mul i32 %emu.g, {idx_bytes}",
                f"  %emu.ib = select i1 %emu.ok, i32 %emu.ib0, i32 {TRASH}",
                f"  %emu.vb0 = mul i32 %emu.g, {vtx_bytes}",
                f"  %emu.vb1 = add i32 %emu.vb0, {idx_region}",
                f"  %emu.vb = select i1 %emu.ok, i32 %emu.vb1, i32 {TRASH}",
                f"  %emu.gv = mul i32 %emu.g, {max_verts}",
                "  %emu.tid = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)",
            ]
            if not tail_fill:
                # Fallback: pre-fill every primitive slot with a degenerate triangle, then a group barrier so
                # the shader's own EmitIndices land afterwards.
                out += degenerate_fill("pf", None)
                out.append("  call void @dx.op.barrier(i32 80, i32 3)")
            # Group 0 thread 0 writes the DrawIndexed arguments for the whole draw. The original entry block
            # becomes %emu.cont (phis that named it as %0 are fixed up below).
            out += [
                "  %emu.w0 = or i32 %emu.g, %emu.tid",
                "  %emu.w = icmp eq i32 %emu.w0, 0",
                "  br i1 %emu.w, label %emu.wr, label %emu.cont",
                "emu.wr:",
                "  %emu.n0 = mul i32 %emu.dimx, %emu.dimy",
                "  %emu.n1 = mul i32 %emu.n0, %emu.dimz",
                f"  %emu.n2 = icmp ult i32 %emu.n1, {max_groups}",
                f"  %emu.n3 = select i1 %emu.n2, i32 %emu.n1, i32 {max_groups}",
                f"  %emu.ic = mul i32 %emu.n3, {max_prims * 3}",
                f"  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %emu.uav, i32 {DRAW_ARGS}, i32 undef, "
                "i32 %emu.ic, i32 1, i32 0, i32 0, i8 15, i32 4)",
                f"  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %emu.uav, i32 {DRAW_ARGS + 16}, "
                "i32 undef, i32 0, i32 undef, i32 undef, i32 undef, i8 1, i32 4)",
                "  br label %emu.cont",
                "emu.cont:",
            ]
            continue
        if in_main and ln.startswith("}"):
            flush_stores()
            in_main = False
        if not in_main:
            out.append(ln)
            continue
        lm = re.match(r"^; <label>:(\d+)", ln) or re.match(r"^([\w.$]+):", ln)
        if lm:
            cur_block = "%" + lm.group(1)
        if " = phi " in ln:
            # The original entry block (implicitly %0) is now %emu.cont.
            ln = re.sub(r"(\[ [^\[\]]+, )%0 \]", r"\1%emu.cont ]", ln)

        code = ln.split(";")[0].rstrip() if "@dx.op." in ln else ln
        m = re.match(r"^\s*call void @dx\.op\.storeVertexOutput\.(\w+)\(i32 171, i32 (\d+), i32 ([^,]+), i8 (\d+), "
                     r"(\w+) ([^,]+), i32 ([^)]+)\)$", code)
        if m:
            ty, sig_id, row, col, vty, val, vi = m.groups()
            if int(sig_id) in layout:  # else never read by the paired PS: drop the store (and its math)
                pending.setdefault((int(sig_id), row, vi), {})[int(col)] = (ty, val)
            continue
        flush_stores()
        m = re.match(r"^\s*call void @dx\.op\.setMeshOutputCounts\(i32 168, i32 ([^,]+), i32 ([^)]+)\)$", code)
        if m:
            # Counts are implied by the degenerate slots: fill [numPrims, maxPrims) here (see tail_fill).
            if tail_fill:
                out += degenerate_fill("tf", m.group(2))
                split_block = cur_block  # its terminator now sits in %emu.tfn<last>
            continue
        m = re.match(r"^\s*call void @dx\.op\.emitIndices\(i32 169, i32 ([^,]+), i32 ([^,]+), i32 ([^,]+), i32 ([^)]+)\)$", code)
        if m:
            p, v0, v1, v2 = m.groups()
            t = tmp()
            out.append(f"  {t} = mul i32 {p}, 12")
            a = addr_expr(out, "%emu.ib", 0, [t])
            gvs = []
            for v in (v0, v1, v2):
                g = tmp()
                out.append(f"  {g} = add i32 {v}, %emu.gv")
                gvs.append(g)
            out.append(f"  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %emu.uav, i32 {a}, i32 undef, "
                       f"i32 {gvs[0]}, i32 {gvs[1]}, i32 {gvs[2]}, i32 undef, i8 7, i32 4)")
            continue
        if re.search(r"@dx\.op\.(setMeshOutputCounts|emitIndices|storeVertexOutput)", code):
            raise ConvertError(f"unhandled mesh op form: {code.strip()}")
        out.append(ln)

    if split_block is not None:
        # Successors of the split block name it as a phi predecessor; the edge now comes from the last
        # continuation block.
        cont = f"%emu.tfn{fill_steps - 1}"
        out = [re.sub(r"(\[ [^\[\]]+, )" + re.escape(split_block) + r" \]", r"\1" + cont + " ]", ln)
               if " = phi " in ln else ln for ln in out]
    mod.lines = drop_unused_decls(out)
    meta = dict(max_verts=max_verts, max_prims=max_prims, vertex_bytes=vertex_bytes, max_groups=max_groups,
                idx_region=idx_region, layout=layout, tail_fill=tail_fill)
    return mod.text(), meta, outputs


# ------------------------------------------------------------------------------------------------
# Mesh shader -> vertex shader (ABI v7 VS mode)
# ------------------------------------------------------------------------------------------------

THREAD_ID_OPS = r"@dx\.op\.(threadIdInGroup|flattenedThreadIdInGroup|groupId|threadId)\.i32"


def vs_mode_blocker(ms_ll):
    """None if the mesh shader can run as a vertex shader, else the reason it cannot. Required: thread t
    writes only vertex t (every StoreVertexOutput is indexed by the thread id), numthreads (N, 1, 1), and no
    way for threads to see each other's work or side effects (groupshared memory, barriers, wave/quad ops,
    UAV writes), no per-primitive outputs and no payload."""
    mod = Module(ms_ll)
    _, eitems, sig = entry_info(mod)
    props = md_items(mod.md[md_ref(eitems[4])])
    ms_state = next(md_items(mod.md[md_ref(v)]) for t, v in zip(props[0::2], props[1::2]) if int_val(t) == 9)
    dims = [int_val(x) for x in md_items(mod.md[md_ref(ms_state[0])])]
    if dims[1] != 1 or dims[2] != 1:
        return f"numthreads {dims}"
    if len(sig) > 2 and sig[2] != "null":
        return "per-primitive outputs"
    if "addrspace(3)" in ms_ll:
        return "groupshared memory"
    if re.search(r"call [^@]*@dx\.op\.(barrier|wave\w*|quad\w*)\.?", ms_ll):
        return "barrier or wave/quad op"
    if re.search(r"call [^@]*@dx\.op\.(bufferStore|rawBufferStore|textureStore\w*|atomic\w+|bufferUpdateCounter)", ms_ll):
        return "UAV writes"
    if "@dx.op.getMeshPayload" in ms_ll:
        return "payload"
    tids = set(re.findall(r"(%[\w.]+) = call i32 @dx\.op\.threadIdInGroup\.i32\(i32 95, i32 0\)", ms_ll) +
               re.findall(r"(%[\w.]+) = call i32 @dx\.op\.flattenedThreadIdInGroup\.i32\(i32 96\)", ms_ll))
    vis = re.findall(r"@dx\.op\.storeVertexOutput\.\w+\(i32 171, [^)]*, i32 (%[\w.]+|\d+)\)", ms_ll)
    if not vis or any(v not in tids for v in vis):
        return "vertex outputs not indexed by the thread id"
    return None


def convert_ms_to_vs(ms_ll):
    """The mesh shader as a vertex shader for one vertex: SV_VertexID = G * maxVerts + t, the index value the
    index-only CS writes. Thread-id and group-id reads become t and G split by the dispatch size (read from the
    argument record at t1 space999, like the CS), StoreVertexOutput becomes StoreOutput with the mesh shader's
    own vertex output signature, SetMeshOutputCounts / EmitIndices are dropped (the driver then removes the
    primitive-side math). See vs_mode_blocker for when this is valid."""
    mod = Module(ms_ll)
    kind, smaj, smin = shader_kind(mod)
    eid, eitems, sig = entry_info(mod)
    props = md_items(mod.md[md_ref(eitems[4])])
    new_props, ms_state, flags = [], None, 0
    for tag, val in zip(props[0::2], props[1::2]):
        t = int_val(tag)
        if t == 9:
            ms_state = md_items(mod.md[md_ref(val)])
        elif t == 0:
            flags = int_val(val)
        else:
            new_props += [tag, val]
    max_verts = int_val(ms_state[1])
    nthreads = int_val(md_items(mod.md[md_ref(ms_state[0])])[0])
    new_props = ["i32 0", f"i64 {flags | 0x10}"] + new_props

    sm_id = md_ref(mod.named_md("dx.shaderModel"))
    mod.set_md(sm_id, f'!{{!"vs", i32 {smaj}, i32 {smin}}}')
    mask = mod.add_md("!{i32 3, i32 1}")
    semidx = mod.add_md("!{i32 0}")
    vid = mod.add_md(f'!{{i32 0, !"SV_VertexID", i8 5, i8 1, !{semidx}, i8 0, i32 1, i8 1, i32 0, i8 0, !{mask}}}')
    inputs = mod.add_md(f"!{{!{vid}}}")
    sig_id = mod.add_md(f"!{{!{inputs}, {sig[1]}, null}}")

    ensure_type(mod, "%struct.ByteAddressBuffer", "{ i32 }")
    ensure_type(mod, "%dx.types.ResRet.i32", "{ i32, i32, i32, i32, i32 }")
    ensure_type(mod, "%dx.types.ResBind", "{ i32, i32, i32, i8 }")
    ensure_type(mod, "%dx.types.ResourceProperties", "{ i32, i32 }")
    res_ref = md_ref(eitems[3])
    if res_ref is None:
        res_ref = mod.add_md("!{null, null, null, null}")
        mod.lines.append(f"!dx.resources = !{{!{res_ref}}}")
    res = md_items(mod.md[res_ref])
    lst = md_items(mod.md[md_ref(res[0])]) if res[0] != "null" else []
    nid = mod.add_md(f"!{{i32 {len(lst)}, %struct.ByteAddressBuffer* undef, !\"\", i32 {EMU_SPACE}, i32 1, i32 1, "
                     "i32 11, i32 0, null}")
    res[0] = f"!{mod.add_md('!{' + ', '.join(lst + [f'!{nid}']) + '}')}"
    mod.set_md(res_ref, "!{" + ", ".join(res) + "}")
    eitems[2] = f"!{sig_id}"
    eitems[3] = f"!{res_ref}"
    eitems[4] = f"!{mod.add_md('!{' + ', '.join(new_props) + '}')}"
    mod.set_md(eid, "!{" + ", ".join(eitems) + "}")
    mod.lines = [ln for ln in mod.lines if not ln.startswith("!dx.viewIdState")]

    nounwind = attr_group(mod, "nounwind")
    readnone = attr_group(mod, "nounwind readnone")
    readonly = attr_group(mod, "nounwind readonly")
    ensure_decl(mod, "dx.op.loadInput.i32", f"declare i32 @dx.op.loadInput.i32(i32, i32, i32, i8, i32) #{readnone}")
    ensure_decl(mod, "dx.op.rawBufferLoad.i32",
                f"declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) #{readonly}")
    ensure_decl(mod, "dx.op.createHandleFromBinding",
                f"declare %dx.types.Handle @dx.op.createHandleFromBinding(i32, %dx.types.ResBind, i32, i1) #{readnone}")
    ensure_decl(mod, "dx.op.annotateHandle",
                f"declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #{readnone}")
    llty = {"f32": "float", "i32": "i32", "f16": "half", "i16": "i16"}
    for ty in sorted(set(re.findall(r"@dx\.op\.storeVertexOutput\.(\w+)\(", ms_ll))):
        if ty not in llty:
            raise ConvertError(f"storeVertexOutput.{ty} unsupported")
        ensure_decl(mod, f"dx.op.storeOutput.{ty}",
                    f"declare void @dx.op.storeOutput.{ty}(i32, i32, i32, i8, {llty[ty]}) #{nounwind}")
    mod.lines = [ln for ln in mod.lines if not (
        re.match(r"^declare .*@dx\.op\.(setMeshOutputCounts|emitIndices|storeVertexOutput)", ln) or
        re.match(r"^declare .*" + THREAD_ID_OPS, ln))]

    entry_fn = re.search(r"@([\w.$]+)", eitems[0]).group(1)
    out, in_main = [], False
    for ln in mod.lines:
        if ln.startswith(f"define void @{entry_fn}()"):
            in_main = True
            out.append(ln)
            out += [
                "  %emu.vid = call i32 @dx.op.loadInput.i32(i32 4, i32 0, i32 0, i8 0, i32 undef)",
                f"  %emu.g = udiv i32 %emu.vid, {max_verts}",
                f"  %emu.t = urem i32 %emu.vid, {max_verts}",
                "  %emu.ha = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind "
                f"{{ i32 1, i32 1, i32 {EMU_SPACE}, i8 0 }}, i32 1, i1 false)",
                "  %emu.args = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %emu.ha, "
                "%dx.types.ResourceProperties { i32 11, i32 0 })",
                "  %emu.d = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %emu.args, "
                "i32 0, i32 undef, i8 3, i32 4)",
                "  %emu.dimx = extractvalue %dx.types.ResRet.i32 %emu.d, 0",
                "  %emu.dimy = extractvalue %dx.types.ResRet.i32 %emu.d, 1",
                "  %emu.gx = urem i32 %emu.g, %emu.dimx",
                "  %emu.q = udiv i32 %emu.g, %emu.dimx",
                "  %emu.gy = urem i32 %emu.q, %emu.dimy",
                "  %emu.gz = udiv i32 %emu.q, %emu.dimy",
                f"  %emu.dx0 = mul i32 %emu.gx, {nthreads}",
                "  %emu.dx = add i32 %emu.dx0, %emu.t",
            ]
            continue
        if in_main and ln.startswith("}"):
            in_main = False
        if not in_main:
            out.append(ln)
            continue
        code = ln.split(";")[0].rstrip() if "@dx.op." in ln else ln
        m = re.match(r"^\s*(%[\w.]+) = call i32 " + THREAD_ID_OPS + r"\(i32 (\d+)(?:, i32 (\d+))?\)$", code)
        if m:
            lhs, op, _, comp = m.group(1), m.group(2), m.group(3), m.group(4)
            c = int(comp) if comp is not None else 0
            src = {"threadIdInGroup": ["%emu.t", "0", "0"], "flattenedThreadIdInGroup": ["%emu.t"],
                   "groupId": ["%emu.gx", "%emu.gy", "%emu.gz"], "threadId": ["%emu.dx", "%emu.gy", "%emu.gz"]}[op][c]
            out.append(f"  {lhs} = add i32 {src}, 0")
            continue
        if re.match(r"^\s*call void @dx\.op\.(setMeshOutputCounts|emitIndices)\(", code):
            continue
        m = re.match(r"^\s*call void @dx\.op\.storeVertexOutput\.(\w+)\(i32 171, i32 (\d+), i32 ([^,]+), i8 (\d+), "
                     r"(\w+) ([^,]+), i32 ([^)]+)\)$", code)
        if m:
            ty, sid, row, col, vty, val, _ = m.groups()
            out.append(f"  call void @dx.op.storeOutput.{ty}(i32 5, i32 {sid}, i32 {row}, i8 {col}, {vty} {val})")
            continue
        if re.search(r"@dx\.op\.(setMeshOutputCounts|emitIndices|storeVertexOutput)|" + THREAD_ID_OPS, code):
            raise ConvertError(f"unhandled op form in VS mode: {code.strip()}")
        out.append(ln)
    mod.lines = drop_unused_decls(out)
    return mod.text()


# ------------------------------------------------------------------------------------------------
# Vertex shader generation
# ------------------------------------------------------------------------------------------------

def hlsl_type(comp, cols):
    base = COMP_TYPES.get(comp)
    if base is None:
        raise ConvertError(f"unsupported component type {comp}")
    return base if cols == 1 else f"{base}{cols}"


def gen_vs(meta, ms_outputs, ps_inputs):
    """HLSL for the pass-through VS. ps_inputs None -> depth-only (position + cull distance)."""
    ms_by_sem = {}
    for e in ms_outputs:
        for k, si in enumerate(e["sem_idx"]):
            ms_by_sem[(e["name"].upper(), si)] = (e, k)

    fields, reads = [], []
    if ps_inputs is None:
        wanted = [e for e in ms_outputs if e["name"].upper() in ("SV_POSITION", "SV_CULLDISTANCE", "SV_CLIPDISTANCE")]
        wanted = [dict(e, interp=e["interp"]) for e in wanted]
    else:
        wanted = [e for e in ps_inputs if e["name"] not in SYSTEM_GENERATED_PS_INPUTS]
        names = {e["name"].upper() for e in wanted}
        # Keep culling/clipping semantics even if the PS does not read them; position is mandatory.
        for e in ms_outputs:
            n = e["name"].upper()
            if n in ("SV_CULLDISTANCE", "SV_CLIPDISTANCE", "SV_POSITION") and n not in names:
                wanted.append(e)
    # The vertex is read in naturally aligned units (16, 8 or 4 bytes, see plan_layout) through a structured
    # buffer, so the loads are vector loads; each unit is loaded once.
    stride = meta["vertex_bytes"]
    unit = 16 if stride % 16 == 0 else 8 if stride % 8 == 0 else 4
    n = unit // 4
    rows = set()

    def comp(byte):  # HLSL expression for the uint at this byte offset of the vertex
        r, lane = byte // unit, (byte % unit) // 4
        rows.add(r)
        return f"r{r}" if n == 1 else f"r{r}.{'xyzw'[lane]}"

    for i, e in enumerate(wanted):
        if e["rows"] != 1 or len(e["sem_idx"]) != 1:
            raise ConvertError(f"array signature element {e['name']} unsupported")
        si = e["sem_idx"][0]
        sem = e["name"] if e["name"].upper().startswith("SV_") else f"{e['name']}{si}"
        if e["name"].upper().startswith("SV_") and si:
            sem = f"{e['name']}{si}"
        ty = hlsl_type(e["comp"], e["cols"])
        interp = INTERP.get(e["interp"], "")
        fields.append(f"    {interp} {ty} f{i} : {sem};".replace("     ", "    "))
        src = ms_by_sem.get((e["name"].upper(), si))
        if src is None:
            reads.append(f"    o.f{i} = ({ty})0;  // not written by the mesh shader")
            continue
        me, _ = src
        off = meta["layout"][me["id"]]
        comps = [comp(off + 4 * k) for k in range(e["cols"])]
        vec = comps[0] if e["cols"] == 1 else f"uint{e['cols']}({', '.join(comps)})"
        base = COMP_TYPES[e["comp"]]
        if base in ("float", "min16float"):
            reads.append(f"    o.f{i} = ({ty})asfloat({vec});")
        elif base in ("int", "min16int"):
            reads.append(f"    o.f{i} = ({ty})asint({vec});")
        else:
            reads.append(f"    o.f{i} = ({ty})({vec});")
    row_ty = "uint" if n == 1 else f"uint{n}"
    loads = [f"    {row_ty} r{r} = Emu[vb + {r}];" for r in sorted(rows)]
    return f"""// Generated by meshemu converter (ABI v5).
StructuredBuffer<{row_ty}> Emu : register(t0, space{EMU_SPACE});

struct VSOut {{
{chr(10).join(fields)}
}};

VSOut main(uint vid : SV_VertexID)
{{
    VSOut o;
    uint vb = {meta['idx_region'] // unit} + vid * {stride // unit};
{chr(10).join(loads)}
{chr(10).join(reads)}
    return o;
}}
"""


def check_linkage(vs_ll, ps_ll):
    """VS outputs must sit at the same rows/cols as the PS inputs they feed."""
    vs = Module(vs_ll)
    _, _, vsig = entry_info(vs)
    vouts = {(e["name"].upper(), tuple(e["sem_idx"])): e for e in parse_sig_elements(vs, md_ref(vsig[1]))}
    ps = Module(ps_ll)
    _, _, psig = entry_info(ps)
    problems = []
    for e in parse_sig_elements(ps, md_ref(psig[0])):
        if e["name"] in SYSTEM_GENERATED_PS_INPUTS:
            continue
        v = vouts.get((e["name"].upper(), tuple(e["sem_idx"])))
        if v is None:
            problems.append(f"{e['name']}{e['sem_idx']} missing")
        elif (v["start_row"], v["start_col"]) != (e["start_row"], e["start_col"]):
            problems.append(f"{e['name']}{e['sem_idx']} at {v['start_row']}.{v['start_col']} vs PS "
                            f"{e['start_row']}.{e['start_col']}")
    return problems


# ------------------------------------------------------------------------------------------------

# ------------------------------------------------------------------------------------------------
# Native 16-bit lowering (GPUs without Native16BitShaderOps, e.g. Pascal)
# ------------------------------------------------------------------------------------------------

LOWER16_LOADS = ("bufferLoad", "textureLoad", "sample", "sampleLevel", "sampleGrad", "sampleBias", "sampleCmp",
                 "sampleCmpLevelZero", "textureGather", "textureGatherCmp")


def lower_native16(ll):
    """Rewrite the native 16-bit forms the game's mesh shaders use so they run without native 16-bit support:
    16-bit-float loads from typed resources (Buffer<half>, textures) become 32-bit loads (the hardware converts
    the stored format, so values are identical) and their `fpext half -> float` become plain moves. Anything
    else 16-bit raises ConvertError. Returns ll unchanged when the shader has no 16-bit types."""
    if not re.search(r"\bhalf\b|\bi16\b", ll):
        return ll
    mod = Module(ll)
    eid, eitems, _ = entry_info(mod)
    lines = []
    for ln in mod.lines:
        if ln.startswith("%dx.types.ResRet.f16 = type") or re.match(r"^declare .*@dx\.op\.\w+\.f16\(", ln):
            continue  # replaced by the f32 forms below
        m = re.match(r"^(\s*%[\w.]+) = fpext half (%[\w.]+) to float(.*)$", ln)
        if m:
            ln = f"{m.group(1)} = fmul float {m.group(2)}, 1.000000e+00{m.group(3)}"
        else:
            ln = re.sub(r"@dx\.op\.(" + "|".join(LOWER16_LOADS) + r")\.f16\(", r"@dx.op.\1.f32(", ln)
            ln = ln.replace("%dx.types.ResRet.f16", "%dx.types.ResRet.f32")
            # Typed resource element type F16 (8) -> F32 (9) in handle annotations: props word 1, low byte.
            ln = re.sub(r"%dx\.types\.ResourceProperties \{ i32 (\d+), i32 (\d+) \}",
                        lambda mm: "%dx.types.ResourceProperties { i32 " + mm.group(1) + ", i32 " +
                        str(int(mm.group(2)) + 1 if int(mm.group(2)) & 0xff == 8 else int(mm.group(2))) + " }", ln)
            # Resource struct type bodies: <N x half> / half -> float (the type names keep "half").
            m = re.match(r'^(%"?class\.[^=]+ = type )(.*)$', ln)
            if m:
                ln = m.group(1) + re.sub(r"\bhalf\b", "float", m.group(2))
        lines.append(ln)
    mod.lines = lines
    mod.md = {}
    for ln in mod.lines:
        m = re.match(r"^!(\d+) = (.*)$", ln)
        if m:
            mod.md[int(m.group(1))] = m.group(2)
    # Resource metadata: element type tag {0, 8 (F16)} -> {0, 9 (F32)} on SRV/UAV entries.
    for i, body in list(mod.md.items()):
        m = re.match(r"^!\{i32 \d+, .*, !(\d+)\}$", body)
        if m and "undef" in body and mod.md.get(int(m.group(1))) == "!{i32 0, i32 8}":
            new = mod.add_md("!{i32 0, i32 9}")
            mod.set_md(i, body[:body.rfind("!")] + f"!{new}}}")
    # Shader flags: no longer uses native low precision (0x20).
    props = md_items(mod.md[md_ref(eitems[4])])
    for k in range(0, len(props) - 1, 2):
        if int_val(props[k]) == 0:
            props[k + 1] = f"i64 {int_val(props[k + 1]) & ~0x20}"
    mod.set_md(md_ref(eitems[4]), "!{" + ", ".join(props) + "}")
    ensure_type(mod, "%dx.types.ResRet.f32", "{ float, float, float, float, i32 }")
    text = mod.text()
    for op in sorted(set(re.findall(r"@dx\.op\.(\w+)\.f32\(", text))):
        if op in LOWER16_LOADS and not re.search(rf"^declare .*@dx\.op\.{op}\.f32\(", text, re.M):
            m = re.search(rf"call %dx\.types\.ResRet\.f32 @dx\.op\.{op}\.f32\(([^)]*)\)", text)
            args = ", ".join(a.strip().rsplit(" ", 1)[0] for a in m.group(1).split(","))
            readonly = attr_group(mod, "nounwind readonly")
            mod.lines.append(f"declare %dx.types.ResRet.f32 @dx.op.{op}.f32({args}) #{readonly}")
            text = mod.text()
    body = re.search(r"^define void @[\w.$]+\(\) \{\n(.*?)^\}", text, re.S | re.M).group(1)
    left = sorted(set(l.strip() for l in body.splitlines() if re.search(r"\bhalf\b|\bi16\b", l)))
    if left:
        raise ConvertError("native 16-bit ops other than typed loads: " + left[0][:160])
    return text


def convert_pair(dump, cache, ms, ps, log):
    pair = f"{ms}_{ps}"
    vs_out = os.path.join(cache, f"{pair}.vs9.dxil")
    if os.path.exists(vs_out):
        return
    ps_inputs, ps_ll = None, None
    if ps != "none":
        ps_ll = disassemble(os.path.join(dump, f"{ps}.ps.dxil"))
        pm = Module(ps_ll)
        _, _, psig = entry_info(pm)
        ps_inputs = parse_sig_elements(pm, md_ref(psig[0]))
    ms_ll = lower_native16(disassemble(os.path.join(dump, f"{ms}.ms.dxil")))
    tmp = vs_out + ".tmp"

    # VS mode: index-only CS + the mesh shader itself as the VS. Falls back to the v5 scheme (CS stores the
    # vertices, generated pass-through VS) when the shader does not qualify or the VS does not link.
    why = os.environ.get("MESHEMU_NO_VS_MODE") and "disabled" or vs_mode_blocker(ms_ll)
    if why is None:
        try:
            vs_ll_path = os.path.join(cache, f"{pair}.vs9.ll")
            with open(vs_ll_path, "w") as f:
                f.write(convert_ms_to_vs(ms_ll))
            run([DXASM, vs_ll_path, tmp])
            if ps_ll is not None:
                problems = check_linkage(disassemble(tmp), ps_ll)
                if problems:
                    raise ConvertError("VS/PS linkage mismatch: " + "; ".join(problems))
        except ConvertError as e:
            why = str(e).strip().splitlines()[0] if str(e).strip() else "VS conversion failed"
            if "validation failed" in str(e):
                why = "VS validation: " + " | ".join(l.strip() for l in str(e).splitlines() if "error" in l)[:300]
            if os.path.exists(tmp):
                os.remove(tmp)
    vs_mode = why is None

    cs_ll, meta, outputs = convert_ms(ms_ll, ps_inputs, index_only=vs_mode)
    ll_path = os.path.join(cache, f"{pair}.cs9.ll")
    with open(ll_path, "w") as f:
        f.write(cs_ll)
    run([DXASM, ll_path, os.path.join(cache, f"{pair}.cs9.dxil")])

    if not vs_mode:
        hlsl_path = os.path.join(cache, f"{pair}.vs9.hlsl")
        with open(hlsl_path, "w") as f:
            f.write(gen_vs(meta, outputs, ps_inputs))
        run([DXC, "-nologo", "-T", "vs_6_6", "-E", "main", "-O3", hlsl_path, "-Fo", tmp])
        if ps_ll is not None:
            problems = check_linkage(disassemble(tmp), ps_ll)
            if problems:
                os.remove(tmp)
                raise ConvertError("VS/PS linkage mismatch: " + "; ".join(problems))
    with open(os.path.join(cache, f"{pair}.meta9"), "w") as f:
        f.write("{max_verts} {max_prims} {vertex_bytes} {max_groups} {idx_region}\n".format(**meta))
    os.replace(tmp, vs_out)  # last: its presence marks the pair as converted
    if vs_mode:
        log(f"  {pair}: VS mode (mesh shader as vertex shader, index-only CS), maxGroups {meta['max_groups']}")
    else:
        log(f"  {pair}: fallback ({why}): {meta['vertex_bytes']}B/vertex, maxGroups {meta['max_groups']}")


def lower16_file(src, dst):
    """`convert.py --lower16 <in.dxil> <out.dxil>`: any shader (the game's own too) with its native 16-bit
    loads lowered (lower_native16), validated and signed. Used by the proxy when the GPU rejects a game
    pipeline for needing native 16-bit ops."""
    ll = lower_native16(disassemble(src))
    ll_path = dst + ".ll"
    with open(ll_path, "w") as f:
        f.write(ll)
    run([DXASM, ll_path, dst + ".tmp"])
    os.replace(dst + ".tmp", dst)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--lower16":
        try:
            lower16_file(os.path.abspath(sys.argv[2]), os.path.abspath(sys.argv[3]))
            print(f"lowered {os.path.basename(sys.argv[2])}")
        except ConvertError as e:
            print(f"FAIL lower16 {os.path.basename(sys.argv[2])}: {e}")
            sys.exit(1)
        return
    dump, cache = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    os.makedirs(cache, exist_ok=True)
    pairs = sorted(f[:-5] for f in os.listdir(dump) if f.endswith(".pair"))
    ok = fail = 0
    for pair in pairs:
        parts = pair.split("_")
        ms, ps = parts[0], parts[1]
        if len(parts) > 2:
            print(f"skip {pair}: amplification shader")
            fail += 1
            continue
        try:
            convert_pair(dump, cache, ms, ps, print)
            ok += 1
        except ConvertError as e:
            print(f"FAIL {pair}: {e}")
            fail += 1
    print(f"{ok} converted, {fail} failed")


if __name__ == "__main__":
    main()
