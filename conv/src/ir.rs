//! The DXIL text rewrites of converter/convert.py (ABI v10), ported line for line so both produce the same
//! LLVM text and therefore byte-identical containers. Function names, comments and the order of every edit
//! follow convert.py; read its docstrings for the design.

use regex::Regex;
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::LazyLock;

pub const EMU_SPACE: i64 = 999;
pub const SCRATCH_CAP: i64 = 256 << 20; // must match kScratchCap in proxy.cpp
const DRAW_ARGS: i64 = SCRATCH_CAP;
const TRASH: i64 = SCRATCH_CAP + 256;

const SYSTEM_GENERATED_PS_INPUTS: [&str; 8] = ["SV_PrimitiveID", "SV_SampleIndex", "SV_IsFrontFace", "SV_Coverage",
                                               "SV_InnerCoverage", "SV_Barycentrics", "SV_ViewID", "SV_ShadingRate"];

fn comp_type(c: i64) -> Option<&'static str> {
    Some(match c {
        4 => "int",
        5 => "uint",
        8 => "min16float",
        9 => "float",
        2 => "min16int",
        3 => "min16uint",
        1 => "bool",
        _ => return None,
    })
}

fn interp_name(i: i64) -> &'static str {
    match i {
        1 => "nointerpolation",
        3 => "centroid",
        4 => "noperspective",
        5 => "noperspective centroid",
        6 => "sample",
        7 => "noperspective sample",
        _ => "",
    }
}

#[derive(Debug)]
pub struct ConvertError(pub String);

impl std::fmt::Display for ConvertError {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

pub type R<T> = Result<T, ConvertError>;

macro_rules! bail {
    ($($t:tt)*) => { return Err(ConvertError(format!($($t)*))) };
}

macro_rules! re {
    ($s:expr) => {{
        static RE: LazyLock<Regex> = LazyLock::new(|| Regex::new($s).unwrap());
        &*RE
    }};
}

// ------------------------------------------------------------------------------------------------
// LLVM text helpers
// ------------------------------------------------------------------------------------------------

pub struct Module {
    pub lines: Vec<String>,
    pub md: HashMap<i64, String>,
    next_md: i64,
}

fn parse_md(lines: &[String]) -> HashMap<i64, String> {
    let mut md = HashMap::new();
    for ln in lines {
        if let Some(c) = re!(r"^!(\d+) = (.*)$").captures(ln) {
            md.insert(c[1].parse().unwrap(), c[2].to_string());
        }
    }
    md
}

impl Module {
    pub fn new(text: &str) -> Module {
        let lines: Vec<String> = text.split('\n').map(String::from).collect();
        let md = parse_md(&lines);
        let next_md = md.keys().max().map_or(0, |m| m + 1);
        Module { lines, md, next_md }
    }

    fn named_md(&self, name: &str) -> Option<String> {
        let re = Regex::new(&format!(r"^!{} = !\{{(.*)\}}$", regex::escape(name))).unwrap();
        self.lines.iter().find_map(|ln| re.captures(ln).map(|c| c[1].to_string()))
    }

    fn set_md(&mut self, i: i64, body: &str) -> R<()> {
        self.md.insert(i, body.to_string());
        let prefix = format!("!{i} = ");
        for ln in self.lines.iter_mut() {
            if ln.starts_with(&prefix) {
                *ln = format!("!{i} = {body}");
                return Ok(());
            }
        }
        bail!("metadata !{i} not found")
    }

    fn add_md(&mut self, body: &str) -> i64 {
        let i = self.next_md;
        self.next_md += 1;
        self.md.insert(i, body.to_string());
        self.lines.push(format!("!{i} = {body}"));
        i
    }

    /// md_items(mod.md[md_ref(item)]): the items of the tuple an item refers to.
    fn items_of(&self, item: &str) -> Vec<String> {
        md_items(&self.md[&md_ref(item).expect("metadata reference")]).expect("metadata tuple")
    }

    pub fn text(&self) -> String {
        self.lines.join("\n")
    }
}

/// Split a metadata tuple body '!{a, b, c}' into top-level items.
fn md_items(body: &str) -> Option<Vec<String>> {
    let body = body.trim();
    if body == "null" {
        return None;
    }
    assert!(body.starts_with("!{") && body.ends_with('}'), "{body}");
    let inner = &body[2..body.len() - 1];
    let (mut items, mut depth, mut cur) = (Vec::new(), 0, String::new());
    for ch in inner.chars() {
        if "{[(<".contains(ch) {
            depth += 1;
        } else if "}])>".contains(ch) {
            depth -= 1;
        }
        if ch == ',' && depth == 0 {
            items.push(cur.trim().to_string());
            cur.clear();
        } else {
            cur.push(ch);
        }
    }
    if !cur.trim().is_empty() {
        items.push(cur.trim().to_string());
    }
    Some(items)
}

fn md_ref(item: &str) -> Option<i64> {
    re!(r"^!(\d+)$").captures(item.trim()).map(|c| c[1].parse().unwrap())
}

fn int_val(item: &str) -> i64 {
    item.split_whitespace().last().unwrap().parse().unwrap()
}

#[derive(Clone, Debug)]
pub struct SigElem {
    pub id: i64,
    pub name: String,
    pub comp: i64,
    pub sem_idx: Vec<i64>,
    pub interp: i64,
    pub rows: i64,
    pub cols: i64,
    pub start_row: i64,
    pub start_col: i64,
}

/// Signature element list -> elements.
fn parse_sig_elements(m: &Module, list_ref: Option<i64>) -> Vec<SigElem> {
    let Some(lr) = list_ref else { return vec![] };
    let mut elems = Vec::new();
    for r in md_items(&m.md[&lr]).unwrap_or_default() {
        let it = m.items_of(&r);
        let name = re!(r#"^!"(.*)"$"#).captures(&it[1]).unwrap()[1].to_string();
        let sem_idx = m.items_of(&it[4]).iter().map(|x| int_val(x)).collect();
        elems.push(SigElem { id: int_val(&it[0]), name, comp: int_val(&it[2]), sem_idx, interp: int_val(&it[5]),
                             rows: int_val(&it[6]), cols: int_val(&it[7]), start_row: int_val(&it[8]),
                             start_col: int_val(&it[9]) });
    }
    elems
}

/// (entry metadata id, entry items, signature items {inputs, outputs, patch constants}).
fn entry_info(m: &Module) -> (i64, Vec<String>, Vec<String>) {
    let ep = md_items(&format!("!{{{}}}", m.named_md("dx.entryPoints").unwrap())).unwrap();
    let eid = md_ref(&ep[0]).unwrap();
    let items = md_items(&m.md[&eid]).unwrap();
    let sig = match md_ref(&items[2]) {
        Some(_) => m.items_of(&items[2]),
        None => vec!["null".into(); 3],
    };
    (eid, items, sig)
}

fn shader_kind(m: &Module) -> (String, i64, i64) {
    let sm = &m.md[&md_ref(&m.named_md("dx.shaderModel").unwrap()).unwrap()];
    let c = re!(r#"!\{!"(\w+)", i32 (\d+), i32 (\d+)\}"#).captures(sm).unwrap();
    (c[1].to_string(), c[2].parse().unwrap(), c[3].parse().unwrap())
}

fn attr_group(m: &mut Module, want: &str) -> i64 {
    for ln in &m.lines {
        if let Some(c) = re!(r"^attributes #(\d+) = \{ (.*) \}$").captures(ln) {
            if &c[2] == want {
                return c[1].parse().unwrap();
            }
        }
    }
    let n = m.lines.iter()
        .filter_map(|ln| re!(r"^attributes #(\d+)").captures(ln).map(|c| c[1].parse::<i64>().unwrap()))
        .max()
        .map_or(0, |x| x + 1);
    m.lines.push(format!("attributes #{n} = {{ {want} }}"));
    n
}

fn ensure_type(m: &mut Module, name: &str, body: &str) {
    let prefix = format!("{name} = type");
    if !m.lines.iter().any(|ln| ln.starts_with(&prefix)) {
        // Insert after the last existing type definition.
        let idx = m.lines.iter().enumerate()
            .filter(|(_, ln)| re!(r#"^%[\w."<>, ]+ = type"#).is_match(ln))
            .map(|(k, _)| k)
            .max()
            .unwrap_or(2);
        m.lines.insert(idx + 1, format!("{name} = type {body}"));
    }
}

/// The validator rejects declared but unused external functions (e.g. a store type a rewrite no longer
/// emits): drop every declaration whose function is not called.
fn drop_unused_decls(lines: Vec<String>) -> Vec<String> {
    let call = re!(r"@([\w.$]+)\(");
    let mut used = HashSet::new();
    for ln in &lines {
        if !ln.starts_with("declare") {
            used.extend(call.captures_iter(ln).map(|c| c[1].to_string()));
        }
    }
    lines.into_iter()
        .filter(|ln| !ln.starts_with("declare") || used.contains(&call.captures(ln).unwrap()[1]))
        .collect()
}

fn ensure_decl(m: &mut Module, name: &str, decl: String) {
    let call = format!("@{name}(");
    if !m.lines.iter().any(|ln| ln.contains(&call) && ln.starts_with("declare")) {
        m.lines.push(decl);
    }
}

fn list_of(m: &Module, item: &str) -> Vec<String> {
    if item != "null" { m.items_of(item) } else { vec![] }
}

/// ABI v10: declare the group offset cbuffer (b0 space999: the proxy's 3 root constants {ox, oy, oz}) in
/// the resource list res (modified in place) and return the entry-block lines that load it into %emu.ox,
/// %emu.oy, %emu.oz.
fn add_group_offset(m: &mut Module, res: &mut [String]) -> Vec<String> {
    ensure_type(m, "%dx.types.CBufRet.i32", "{ i32, i32, i32, i32 }");
    ensure_type(m, "%struct.EmuGroupOffset", "{ <3 x i32> }");
    let mut lst = list_of(m, &res[2]);
    let nid = m.add_md(&format!("!{{i32 {}, %struct.EmuGroupOffset* undef, !\"\", i32 {EMU_SPACE}, i32 0, i32 1, \
                                 i32 12, null}}", lst.len()));
    lst.push(format!("!{nid}"));
    res[2] = format!("!{}", m.add_md(&format!("!{{{}}}", lst.join(", "))));
    let ro = attr_group(m, "nounwind readonly");
    ensure_decl(m, "dx.op.cbufferLoadLegacy.i32",
                format!("declare %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32) #{ro}"));
    vec![
        format!("  %emu.hc = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind \
                 {{ i32 0, i32 0, i32 {EMU_SPACE}, i8 2 }}, i32 0, i1 false)"),
        "  %emu.cb = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %emu.hc, \
         %dx.types.ResourceProperties { i32 13, i32 12 })".into(),
        "  %emu.o = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %emu.cb, i32 0)".into(),
        "  %emu.ox = extractvalue %dx.types.CBufRet.i32 %emu.o, 0".into(),
        "  %emu.oy = extractvalue %dx.types.CBufRet.i32 %emu.o, 1".into(),
        "  %emu.oz = extractvalue %dx.types.CBufRet.i32 %emu.o, 2".into(),
    ]
}

// ------------------------------------------------------------------------------------------------
// Mesh shader -> compute shader
// ------------------------------------------------------------------------------------------------

const ALWAYS_KEPT: [&str; 3] = ["SV_POSITION", "SV_CULLDISTANCE", "SV_CLIPDISTANCE"];

/// Mesh shader outputs worth storing: what the paired PS reads, plus position and clip/cull distances.
fn kept_outputs<'a>(outputs: &'a [SigElem], ps_inputs: Option<&[SigElem]>) -> Vec<&'a SigElem> {
    let wanted: HashSet<(String, i64)> = ps_inputs.unwrap_or_default().iter()
        .filter(|e| !SYSTEM_GENERATED_PS_INPUTS.contains(&e.name.as_str()))
        .map(|e| (e.name.to_uppercase(), e.sem_idx[0]))
        .collect();
    outputs.iter()
        .filter(|e| {
            let n = e.name.to_uppercase();
            ALWAYS_KEPT.contains(&n.as_str()) || wanted.contains(&(n, e.sem_idx[0]))
        })
        .collect()
}

/// Bytes one row of a signature element occupies in the vertex.
fn row_bytes(e: &SigElem) -> i64 {
    if e.cols == 3 && e.rows > 1 { 16 } else { 4 * e.cols }
}

/// Vertex layout for aligned vector stores and loads (see convert.py). Returns ({sig id: byte offset},
/// vertex_bytes).
fn plan_layout(outputs: &[SigElem], ps_inputs: Option<&[SigElem]>) -> (HashMap<i64, i64>, i64) {
    let kept = kept_outputs(outputs, ps_inputs);
    let pick = |n: i64| kept.iter().copied().filter(|e| row_bytes(e) == n).collect::<Vec<_>>();
    let (b16, b12, b8, mut b4) = (pick(16), pick(12), pick(8), std::collections::VecDeque::from(pick(4)));
    let mut order: Vec<Option<&SigElem>> = b16.into_iter().map(Some).collect();
    for e in b12 {
        order.push(Some(e));
        order.push(b4.pop_front()); // None: 4 bytes of padding
    }
    order.extend(b8.into_iter().map(Some));
    order.extend(b4.into_iter().map(Some));
    let (mut layout, mut off) = (HashMap::new(), 0);
    for e in order {
        match e {
            None => off += 4,
            Some(e) => {
                layout.insert(e.id, off);
                off += e.rows * row_bytes(e);
            }
        }
    }
    let (s8, s16) = ((off + 7) & !7, (off + 15) & !15);
    (layout, (if s16 - off <= 4 { s16 } else { s8 }).max(4))
}

/// Largest power of two (<= 16) dividing both off and stride.
fn alignment(off: i64, stride: i64) -> i64 {
    let mut a = 16;
    while a > 4 && (off % a != 0 || stride % a != 0) {
        a /= 2;
    }
    a
}

fn gcd(a: i64, b: i64) -> i64 {
    if b == 0 { a.abs() } else { gcd(b, a % b) }
}

fn label_of(ln: &str) -> Option<String> {
    re!(r"^; <label>:(\d+)").captures(ln).or_else(|| re!(r"^([\w.$]+):").captures(ln)).map(|c| format!("%{}", &c[1]))
}

/// True if the function body has exactly one call matching call_re and every path from the entry block to
/// a return passes through its basic block.
fn call_post_dominates(body: &[String], call_re: &Regex) -> bool {
    let mut cur = "%0".to_string();
    let mut succ: HashMap<String, Vec<String>> = HashMap::from([(cur.clone(), vec![])]);
    let mut rets = HashSet::new();
    let (mut call_block, mut calls) = (None, 0);
    for ln in body {
        if let Some(l) = label_of(ln) {
            cur = l;
            succ.insert(cur.clone(), vec![]);
            continue;
        }
        if call_re.is_match(ln) {
            calls += 1;
            call_block = Some(cur.clone());
        }
        let s = ln.trim();
        if s.starts_with("br ") || s.starts_with("switch ") || s.starts_with("indirectbr ") {
            succ.insert(cur.clone(), re!(r"label (%[\w.$]+)").captures_iter(s).map(|c| c[1].to_string()).collect());
        } else if s.starts_with("ret") || s.starts_with("unreachable") {
            rets.insert(cur.clone());
        }
    }
    let Some(call_block) = call_block.filter(|_| calls == 1) else { return false };
    let mut seen = HashSet::from(["%0".to_string()]);
    let mut todo = vec!["%0".to_string()];
    while let Some(b) = todo.pop() {
        if b == call_block {
            continue; // paths through the call block are fine
        }
        if rets.contains(&b) {
            return false;
        }
        for n in succ.get(&b).into_iter().flatten() {
            if seen.insert(n.clone()) {
                todo.push(n.clone());
            }
        }
    }
    true
}

pub struct Meta {
    pub max_verts: i64,
    pub max_prims: i64,
    pub vertex_bytes: i64,
    pub max_groups: i64,
    pub idx_region: i64,
    pub layout: HashMap<i64, i64>,
}

impl Meta {
    /// The .meta10 file the proxy reads.
    pub fn file(&self) -> String {
        format!("{} {} {} {} {}\n", self.max_verts, self.max_prims, self.vertex_bytes, self.max_groups, self.idx_region)
    }
}

fn entry_fn_name(eitems: &[String]) -> String {
    re!(r"@([\w.$]+)").captures(&eitems[0]).unwrap()[1].to_string()
}

/// Mesh shader -> compute shader. index_only (ABI v7 VS mode): store no vertex outputs, only indices + draw
/// arguments; the vertices are computed by the mesh shader itself running as the vertex shader.
pub fn convert_ms(ms_ll: &str, ps_inputs: Option<&[SigElem]>, index_only: bool) -> R<(String, Meta, Vec<SigElem>)> {
    let mut m = Module::new(ms_ll);
    let (kind, smaj, smin) = shader_kind(&m);
    if kind != "ms" {
        bail!("not a mesh shader ({kind})");
    }
    if (smaj, smin) < (6, 6) {
        bail!("mesh shaders below SM 6.6 not supported yet");
    }
    for bad in ["getMeshPayload", "storePrimitiveOutput"] {
        if ms_ll.contains(&format!("@dx.op.{bad}")) {
            bail!("unsupported op {bad}");
        }
    }

    let (eid, mut eitems, sig) = entry_info(&m);
    let outputs = parse_sig_elements(&m, if sig[1] != "null" { md_ref(&sig[1]) } else { None });
    if outputs.is_empty() {
        bail!("no vertex outputs");
    }
    let (layout, vertex_bytes) = if index_only { (HashMap::new(), 0) } else { plan_layout(&outputs, ps_inputs) };

    let props = m.items_of(&eitems[4]);
    let (mut ms_state, mut flags, mut other_props) = (None, 0i64, vec![]);
    for pair in props.chunks(2) {
        let (tag, val) = (&pair[0], &pair[1]);
        match int_val(tag) {
            9 => ms_state = Some(m.items_of(val)), // mesh state: {numthreads, maxVerts, maxPrims, topology, payload}
            0 => flags = int_val(val),             // shader flags
            _ => other_props.extend([tag.clone(), val.clone()]),
        }
    }
    let Some(ms_state) = ms_state else { bail!("no mesh shader state") };
    // Compute: shader flags (+ raw/structured buffers for the scratch UAV), numthreads from the MS state.
    let mut new_props = vec!["i32 0".to_string(), format!("i64 {}", flags | 0x10), "i32 4".into(), ms_state[0].clone()];
    new_props.extend(other_props);
    let (max_verts, max_prims) = (int_val(&ms_state[1]), int_val(&ms_state[2]));
    if int_val(&ms_state[3]) != 2 {
        bail!("only triangle topology supported");
    }
    let tdims: Vec<i64> = m.items_of(&ms_state[0]).iter().map(|x| int_val(x)).collect();
    let nthreads = tdims[0] * tdims[1] * tdims[2];
    let idx_bytes = max_prims * 12;
    let vtx_bytes = max_verts * vertex_bytes;
    let mut max_groups = SCRATCH_CAP / (idx_bytes + vtx_bytes);
    if let Some(cap) = std::env::var("MESHEMU_TEST_MAX_GROUPS").ok().filter(|v| !v.is_empty()) {
        max_groups = max_groups.min(cap.parse().unwrap());
    }
    let idx_region = (max_groups * idx_bytes + 255) & !255;
    while idx_region + max_groups * vtx_bytes > SCRATCH_CAP {
        max_groups -= 1;
    }

    // Metadata: shader model, entry (no signatures, no MS state), resources (+UAV, +CBV).
    let sm_id = md_ref(&m.named_md("dx.shaderModel").unwrap()).unwrap();
    m.set_md(sm_id, &format!("!{{!\"cs\", i32 {smaj}, i32 {smin}}}"))?;
    let props_id = m.add_md(&format!("!{{{}}}", new_props.join(", ")));

    ensure_type(&mut m, "%struct.RWByteAddressBuffer", "{ i32 }");
    ensure_type(&mut m, "%struct.ByteAddressBuffer", "{ i32 }");
    ensure_type(&mut m, "%dx.types.ResRet.i32", "{ i32, i32, i32, i32, i32 }");

    let res_ref = match md_ref(&eitems[3]) {
        Some(r) => r,
        None => {
            let r = m.add_md("!{null, null, null, null}");
            m.lines.push(format!("!dx.resources = !{{!{r}}}"));
            r
        }
    };
    let mut res = md_items(&m.md[&res_ref]).unwrap();

    let append_res = |m: &mut Module, res: &mut Vec<String>, slot: usize, entry: &dyn Fn(usize) -> String| {
        let mut lst = list_of(m, &res[slot]);
        let nid = m.add_md(&entry(lst.len()));
        lst.push(format!("!{nid}"));
        res[slot] = format!("!{}", m.add_md(&format!("!{{{}}}", lst.join(", "))));
    };
    append_res(&mut m, &mut res, 1, &|id| format!("!{{i32 {id}, %struct.RWByteAddressBuffer* undef, !\"\", i32 {EMU_SPACE}, \
                                                    i32 0, i32 1, i32 11, i1 false, i1 false, i1 false, null}}"));
    append_res(&mut m, &mut res, 0, &|id| format!("!{{i32 {id}, %struct.ByteAddressBuffer* undef, !\"\", i32 {EMU_SPACE}, \
                                                    i32 1, i32 1, i32 11, i32 0, null}}"));
    let offset_lines = add_group_offset(&mut m, &mut res);
    m.set_md(res_ref, &format!("!{{{}}}", res.join(", ")))?;
    eitems[2] = "null".into();
    eitems[3] = format!("!{res_ref}");
    eitems[4] = format!("!{props_id}");
    m.set_md(eid, &format!("!{{{}}}", eitems.join(", ")))?;
    m.lines.retain(|ln| !ln.starts_with("!dx.viewIdState"));

    // Unused primitive slots must hold degenerate triangles (see convert.py).
    let entry_fn = entry_fn_name(&eitems);
    let define = format!("define void @{entry_fn}()");
    let start = m.lines.iter().position(|ln| ln.starts_with(&define)).unwrap();
    let end = (start..m.lines.len()).find(|&k| m.lines[k].starts_with('}')).unwrap();
    let tail_fill = call_post_dominates(&m.lines[start + 1..end], re!(r"@dx\.op\.setMeshOutputCounts\("));

    // Declarations.
    let nounwind = attr_group(&mut m, "nounwind");
    let readnone = attr_group(&mut m, "nounwind readnone");
    let readonly = attr_group(&mut m, "nounwind readonly");
    ensure_decl(&mut m, "dx.op.rawBufferStore.i32",
                format!("declare void @dx.op.rawBufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8, i32) #{nounwind}"));
    ensure_decl(&mut m, "dx.op.rawBufferStore.f32",
                format!("declare void @dx.op.rawBufferStore.f32(i32, %dx.types.Handle, i32, i32, float, float, float, float, i8, i32) #{nounwind}"));
    ensure_decl(&mut m, "dx.op.rawBufferLoad.i32",
                format!("declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) #{readonly}"));
    ensure_decl(&mut m, "dx.op.groupId.i32", format!("declare i32 @dx.op.groupId.i32(i32, i32) #{readnone}"));
    ensure_decl(&mut m, "dx.op.flattenedThreadIdInGroup.i32",
                format!("declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #{readnone}"));
    if !tail_fill {
        let nodup = attr_group(&mut m, "noduplicate nounwind");
        ensure_decl(&mut m, "dx.op.barrier", format!("declare void @dx.op.barrier(i32, i32) #{nodup}"));
    }
    ensure_decl(&mut m, "dx.op.createHandleFromBinding",
                format!("declare %dx.types.Handle @dx.op.createHandleFromBinding(i32, %dx.types.ResBind, i32, i1) #{readnone}"));
    ensure_decl(&mut m, "dx.op.annotateHandle",
                format!("declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #{readnone}"));
    ensure_type(&mut m, "%dx.types.ResBind", "{ i32, i32, i32, i8 }");
    ensure_type(&mut m, "%dx.types.ResourceProperties", "{ i32, i32 }");
    m.lines.retain(|ln| !re!(r"^declare .*@dx\.op\.(setMeshOutputCounts|emitIndices|storeVertexOutput)").is_match(ln));

    let by_id: HashMap<i64, &SigElem> = outputs.iter().map(|e| (e.id, e)).collect();
    let mut out: Vec<String> = Vec::new();
    let mut counter = 0;
    let mut tmp = || {
        counter += 1;
        format!("%emu.t{counter}")
    };

    /// Emit base + const + sum(dyn) and return the value name.
    fn addr_expr(lines: &mut Vec<String>, tmp: &mut dyn FnMut() -> String, base: &str, parts_const: i64,
                 parts_dyn: &[String]) -> String {
        let mut cur = base.to_string();
        for d in parts_dyn {
            let t = tmp();
            lines.push(format!("  {t} = add i32 {cur}, {d}"));
            cur = t;
        }
        if parts_const != 0 {
            let t = tmp();
            lines.push(format!("  {t} = add i32 {cur}, {parts_const}"));
            cur = t;
        }
        cur
    }

    let fill_steps = (max_prims + nthreads - 1) / nthreads;

    // Store a degenerate triangle into every primitive slot p of this group with lo <= p < maxPrims.
    let degenerate_fill = |tag: &str, lo: Option<&str>| -> Vec<String> {
        let mut lines = vec![];
        for j in 0..fill_steps {
            let p = format!("%emu.{tag}p{j}");
            let cond = format!("%emu.{tag}in{j}");
            lines.push(format!("  {p} = add i32 %emu.tid, {}", j * nthreads));
            lines.push(format!("  {cond} = icmp ult i32 {p}, {max_prims}"));
            if let Some(lo) = lo {
                lines.extend([
                    format!("  %emu.{tag}ge{j} = icmp uge i32 {p}, {lo}"),
                    format!("  %emu.{tag}k{j} = and i1 {cond}, %emu.{tag}ge{j}"),
                    format!("  br i1 %emu.{tag}k{j}, label %emu.{tag}s{j}, label %emu.{tag}n{j}"),
                    format!("emu.{tag}s{j}:"),
                    format!("  %emu.{tag}a{j}x = mul i32 {p}, 12"),
                    format!("  %emu.{tag}a{j} = add i32 %emu.ib, %emu.{tag}a{j}x"),
                ]);
            } else {
                lines.extend([
                    format!("  %emu.{tag}a{j}x = mul i32 {p}, 12"),
                    format!("  %emu.{tag}a{j}y = add i32 %emu.ib, %emu.{tag}a{j}x"),
                    format!("  %emu.{tag}a{j} = select i1 {cond}, i32 %emu.{tag}a{j}y, i32 {TRASH}"),
                ]);
            }
            lines.push(format!("  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %emu.uav, i32 %emu.{tag}a{j}, \
                                i32 undef, i32 %emu.gv, i32 %emu.gv, i32 %emu.gv, i32 undef, i8 7, i32 4)"));
            if lo.is_some() {
                lines.push(format!("  br label %emu.{tag}n{j}"));
                lines.push(format!("emu.{tag}n{j}:"));
            }
        }
        lines
    };

    // Vertex output stores of one basic block are collected per (element, row, vertex), in first-seen order,
    // and emitted as vector stores (one per run of consecutive components) before the next other instruction.
    type Pending = Vec<((i64, String, String), std::collections::BTreeMap<i64, (String, String)>)>;
    let mut pending: Pending = vec![];

    let flush_stores = |out: &mut Vec<String>, tmp: &mut dyn FnMut() -> String, pending: &mut Pending| -> R<()> {
        for ((sig_id, row, vi), comps) in pending.drain(..) {
            let rb = row_bytes(by_id[&sig_id]);
            let t1 = tmp();
            out.push(format!("  {t1} = mul i32 {vi}, {vertex_bytes}"));
            let (mut dyn_, mut cnst, mut row_align) = (vec![t1], layout[&sig_id], vertex_bytes);
            if re!(r"^-?\d+$").is_match(&row) {
                cnst += row.parse::<i64>().unwrap() * rb;
            } else {
                let t2 = tmp();
                out.push(format!("  {t2} = mul i32 {row}, {rb}"));
                dyn_.push(t2);
                row_align = gcd(vertex_bytes, rb);
            }
            let mut runs: Vec<Vec<i64>> = vec![];
            for &c in comps.keys() {
                match runs.last_mut() {
                    Some(r) if *r.last().unwrap() == c - 1 => r.push(c),
                    _ => runs.push(vec![c]),
                }
            }
            for run in runs {
                let (mut vals, mut st) = (vec![], "");
                for c in &run {
                    let (ty, val) = &comps[c];
                    let sv = match ty.as_str() {
                        "f32" => { st = "f32"; val.clone() }
                        "i32" => { st = "i32"; val.clone() }
                        "f16" => {
                            let sv = tmp();
                            out.push(format!("  {sv} = fpext half {val} to float"));
                            st = "f32";
                            sv
                        }
                        "i16" => {
                            let sv = tmp();
                            out.push(format!("  {sv} = sext i16 {val} to i32"));
                            st = "i32";
                            sv
                        }
                        _ => bail!("storeVertexOutput.{ty} unsupported"),
                    };
                    vals.push(sv);
                }
                let vt = if st == "f32" { "float" } else { "i32" };
                let a = addr_expr(out, tmp, "%emu.vb", cnst + run[0] * 4, &dyn_);
                let n = vals.len();
                vals.extend(std::iter::repeat_n("undef".to_string(), 4 - n));
                let ops = vals.iter().map(|v| format!("{vt} {v}")).collect::<Vec<_>>().join(", ");
                let al = alignment(cnst + run[0] * 4, row_align);
                out.push(format!("  call void @dx.op.rawBufferStore.{st}(i32 140, %dx.types.Handle %emu.uav, i32 {a}, \
                                  i32 undef, {ops}, i8 {}, i32 {al})", (1 << run.len()) - 1));
            }
        }
        Ok(())
    };

    let mut in_main = false;
    let mut cur_block = "%emu.cont".to_string(); // the original entry block %0 is emitted as %emu.cont
    let mut split_block: Option<String> = None;
    let lines = std::mem::take(&mut m.lines);
    for ln in &lines {
        if ln.starts_with(&define) {
            in_main = true;
            out.push(ln.clone());
            out.extend([
                format!("  %emu.hu = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind \
                         {{ i32 0, i32 0, i32 {EMU_SPACE}, i8 1 }}, i32 0, i1 false)"),
                "  %emu.uav = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %emu.hu, \
                 %dx.types.ResourceProperties { i32 4107, i32 0 })".into(),
                format!("  %emu.ha = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind \
                         {{ i32 1, i32 1, i32 {EMU_SPACE}, i8 0 }}, i32 1, i1 false)"),
                "  %emu.args = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %emu.ha, \
                 %dx.types.ResourceProperties { i32 11, i32 0 })".into(),
                "  %emu.d = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %emu.args, \
                 i32 0, i32 undef, i8 7, i32 4)".into(),
                "  %emu.dimx = extractvalue %dx.types.ResRet.i32 %emu.d, 0".into(),
                "  %emu.dimy = extractvalue %dx.types.ResRet.i32 %emu.d, 1".into(),
                "  %emu.dimz = extractvalue %dx.types.ResRet.i32 %emu.d, 2".into(),
                "  %emu.gx = call i32 @dx.op.groupId.i32(i32 94, i32 0)".into(),
                "  %emu.gy = call i32 @dx.op.groupId.i32(i32 94, i32 1)".into(),
                "  %emu.gz = call i32 @dx.op.groupId.i32(i32 94, i32 2)".into(),
                "  %emu.l0 = mul i32 %emu.gz, %emu.dimy".into(),
                "  %emu.l1 = add i32 %emu.l0, %emu.gy".into(),
                "  %emu.l2 = mul i32 %emu.l1, %emu.dimx".into(),
                "  %emu.g = add i32 %emu.l2, %emu.gx".into(),
                format!("  %emu.ok = icmp ult i32 %emu.g, {max_groups}"),
                format!("  %emu.ib0 = mul i32 %emu.g, {idx_bytes}"),
                format!("  %emu.ib = select i1 %emu.ok, i32 %emu.ib0, i32 {TRASH}"),
                format!("  %emu.vb0 = mul i32 %emu.g, {vtx_bytes}"),
                format!("  %emu.vb1 = add i32 %emu.vb0, {idx_region}"),
                format!("  %emu.vb = select i1 %emu.ok, i32 %emu.vb1, i32 {TRASH}"),
                format!("  %emu.gv = mul i32 %emu.g, {max_verts}"),
                "  %emu.tid = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)".into(),
            ]);
            // Scratch addressing above stays chunk-local; the game's own group/dispatch thread ids get the
            // chunk's offset (see add_group_offset).
            out.extend(offset_lines.iter().cloned());
            for (a, n) in "xyz".chars().zip(&tdims) {
                out.push(format!("  %emu.t{a} = mul i32 %emu.o{a}, {n}"));
            }
            if !tail_fill {
                // Fallback: pre-fill every primitive slot with a degenerate triangle, then a group barrier.
                out.extend(degenerate_fill("pf", None));
                out.push("  call void @dx.op.barrier(i32 80, i32 3)".into());
            }
            // Group 0 thread 0 writes the DrawIndexed arguments for the whole draw.
            out.extend([
                "  %emu.w0 = or i32 %emu.g, %emu.tid".into(),
                "  %emu.w = icmp eq i32 %emu.w0, 0".into(),
                "  br i1 %emu.w, label %emu.wr, label %emu.cont".into(),
                "emu.wr:".into(),
                "  %emu.n0 = mul i32 %emu.dimx, %emu.dimy".into(),
                "  %emu.n1 = mul i32 %emu.n0, %emu.dimz".into(),
                format!("  %emu.n2 = icmp ult i32 %emu.n1, {max_groups}"),
                format!("  %emu.n3 = select i1 %emu.n2, i32 %emu.n1, i32 {max_groups}"),
                format!("  %emu.ic = mul i32 %emu.n3, {}", max_prims * 3),
                format!("  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %emu.uav, i32 {DRAW_ARGS}, i32 undef, \
                         i32 %emu.ic, i32 1, i32 0, i32 0, i8 15, i32 4)"),
                format!("  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %emu.uav, i32 {}, \
                         i32 undef, i32 0, i32 undef, i32 undef, i32 undef, i8 1, i32 4)", DRAW_ARGS + 16),
                "  br label %emu.cont".into(),
                "emu.cont:".into(),
            ]);
            continue;
        }
        if in_main && ln.starts_with('}') {
            flush_stores(&mut out, &mut tmp, &mut pending)?;
            in_main = false;
        }
        if !in_main {
            out.push(ln.clone());
            continue;
        }
        if let Some(l) = label_of(ln) {
            cur_block = l;
        }
        let mut ln = ln.clone();
        if ln.contains(" = phi ") {
            // The original entry block (implicitly %0) is now %emu.cont.
            ln = re!(r"(\[ [^\[\]]+, )%0 \]").replace_all(&ln, "${1}%emu.cont ]").into_owned();
        }

        let code = if ln.contains("@dx.op.") { ln.split(';').next().unwrap().trim_end().to_string() } else { ln.clone() };
        if let Some(c) = re!(r"^\s*(%[\w.]+) = call i32 @dx\.op\.(groupId|threadId)\.i32\(i32 (?:94|93), i32 ([012])\)$")
            .captures(&code)
        {
            let (lhs, op, comp) = (&c[1], &c[2], c[3].parse::<usize>().unwrap());
            let (raw, a) = (tmp(), &"xyz"[comp..comp + 1]);
            out.push(format!("  {raw} = call i32 @dx.op.{op}.i32(i32 {}, i32 {comp})", if op == "groupId" { 94 } else { 93 }));
            out.push(format!("  {lhs} = add i32 {raw}, %emu.{}{a}", if op == "groupId" { "o" } else { "t" }));
            continue;
        }
        if let Some(c) = re!(r"^\s*call void @dx\.op\.storeVertexOutput\.(\w+)\(i32 171, i32 (\d+), i32 ([^,]+), i8 (\d+), (\w+) ([^,]+), i32 ([^)]+)\)$")
            .captures(&code)
        {
            let sig_id: i64 = c[2].parse().unwrap();
            if layout.contains_key(&sig_id) {
                // else never read by the paired PS: drop the store (and its math)
                let key = (sig_id, c[3].to_string(), c[7].to_string());
                let col: i64 = c[4].parse().unwrap();
                let v = (c[1].to_string(), c[6].to_string());
                match pending.iter_mut().find(|(k, _)| *k == key) {
                    Some((_, comps)) => { comps.insert(col, v); }
                    None => pending.push((key, std::collections::BTreeMap::from([(col, v)]))),
                }
            }
            continue;
        }
        flush_stores(&mut out, &mut tmp, &mut pending)?;
        if let Some(c) = re!(r"^\s*call void @dx\.op\.setMeshOutputCounts\(i32 168, i32 ([^,]+), i32 ([^)]+)\)$").captures(&code) {
            // Counts are implied by the degenerate slots: fill [numPrims, maxPrims) here (see tail_fill).
            if tail_fill {
                out.extend(degenerate_fill("tf", Some(&c[2])));
                split_block = Some(cur_block.clone()); // its terminator now sits in %emu.tfn<last>
            }
            continue;
        }
        if let Some(c) = re!(r"^\s*call void @dx\.op\.emitIndices\(i32 169, i32 ([^,]+), i32 ([^,]+), i32 ([^,]+), i32 ([^)]+)\)$")
            .captures(&code)
        {
            let t = tmp();
            out.push(format!("  {t} = mul i32 {}, 12", &c[1]));
            let a = addr_expr(&mut out, &mut tmp, "%emu.ib", 0, &[t]);
            let mut gvs = vec![];
            for v in [&c[2], &c[3], &c[4]] {
                let g = tmp();
                out.push(format!("  {g} = add i32 {v}, %emu.gv"));
                gvs.push(g);
            }
            out.push(format!("  call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %emu.uav, i32 {a}, i32 undef, \
                              i32 {}, i32 {}, i32 {}, i32 undef, i8 7, i32 4)", gvs[0], gvs[1], gvs[2]));
            continue;
        }
        if re!(r"@dx\.op\.(setMeshOutputCounts|emitIndices|storeVertexOutput)").is_match(&code) {
            bail!("unhandled mesh op form: {}", code.trim());
        }
        out.push(ln);
    }

    if let Some(split) = split_block {
        // Successors of the split block name it as a phi predecessor; the edge now comes from the last
        // continuation block.
        let cont = format!("%emu.tfn{}", fill_steps - 1);
        let re = Regex::new(&format!(r"(\[ [^\[\]]+, ){} \]", regex::escape(&split))).unwrap();
        for ln in out.iter_mut() {
            if ln.contains(" = phi ") {
                *ln = re.replace_all(ln, format!("${{1}}{cont} ]").as_str()).into_owned();
            }
        }
    }
    m.lines = drop_unused_decls(out);
    let meta = Meta { max_verts, max_prims, vertex_bytes, max_groups, idx_region, layout };
    Ok((m.text(), meta, outputs))
}

// ------------------------------------------------------------------------------------------------
// Mesh shader -> vertex shader (ABI v7 VS mode)
// ------------------------------------------------------------------------------------------------

const THREAD_ID_OPS: &str = r"@dx\.op\.(threadIdInGroup|flattenedThreadIdInGroup|groupId|threadId)\.i32";

fn ms_state_of(m: &Module, eitems: &[String]) -> R<Vec<String>> {
    let props = m.items_of(&eitems[4]);
    props.chunks(2).find(|p| int_val(&p[0]) == 9).map(|p| m.items_of(&p[1]))
        .ok_or_else(|| ConvertError("no mesh shader state".into()))
}

/// None if the mesh shader can run as a vertex shader, else the reason it cannot (see convert.py).
pub fn vs_mode_blocker(ms_ll: &str) -> R<Option<String>> {
    let m = Module::new(ms_ll);
    let (_, eitems, sig) = entry_info(&m);
    let ms_state = ms_state_of(&m, &eitems)?;
    let dims: Vec<i64> = m.items_of(&ms_state[0]).iter().map(|x| int_val(x)).collect();
    if dims[1] != 1 || dims[2] != 1 {
        return Ok(Some(format!("numthreads {dims:?}")));
    }
    if sig.len() > 2 && sig[2] != "null" {
        return Ok(Some("per-primitive outputs".into()));
    }
    if ms_ll.contains("addrspace(3)") {
        return Ok(Some("groupshared memory".into()));
    }
    if re!(r"call [^@]*@dx\.op\.(barrier|wave\w*|quad\w*)\.?").is_match(ms_ll) {
        return Ok(Some("barrier or wave/quad op".into()));
    }
    if re!(r"call [^@]*@dx\.op\.(bufferStore|rawBufferStore|textureStore\w*|atomic\w+|bufferUpdateCounter)").is_match(ms_ll) {
        return Ok(Some("UAV writes".into()));
    }
    if ms_ll.contains("@dx.op.getMeshPayload") {
        return Ok(Some("payload".into()));
    }
    let mut tids: HashSet<&str> = HashSet::new();
    for c in re!(r"(%[\w.]+) = call i32 @dx\.op\.threadIdInGroup\.i32\(i32 95, i32 0\)").captures_iter(ms_ll) {
        tids.insert(c.get(1).unwrap().as_str());
    }
    for c in re!(r"(%[\w.]+) = call i32 @dx\.op\.flattenedThreadIdInGroup\.i32\(i32 96\)").captures_iter(ms_ll) {
        tids.insert(c.get(1).unwrap().as_str());
    }
    let vis: Vec<&str> = re!(r"@dx\.op\.storeVertexOutput\.\w+\(i32 171, [^)]*, i32 (%[\w.]+|\d+)\)")
        .captures_iter(ms_ll).map(|c| c.get(1).unwrap().as_str()).collect();
    if vis.is_empty() || vis.iter().any(|v| !tids.contains(v)) {
        return Ok(Some("vertex outputs not indexed by the thread id".into()));
    }
    Ok(None)
}

/// The mesh shader as a vertex shader for one vertex (see convert.py convert_ms_to_vs).
pub fn convert_ms_to_vs(ms_ll: &str) -> R<String> {
    let mut m = Module::new(ms_ll);
    let (_, smaj, smin) = shader_kind(&m);
    let (eid, mut eitems, sig) = entry_info(&m);
    let props = m.items_of(&eitems[4]);
    let (mut new_props, mut ms_state, mut flags) = (vec![], None, 0i64);
    for p in props.chunks(2) {
        match int_val(&p[0]) {
            9 => ms_state = Some(m.items_of(&p[1])),
            0 => flags = int_val(&p[1]),
            _ => new_props.extend([p[0].clone(), p[1].clone()]),
        }
    }
    let Some(ms_state) = ms_state else { bail!("no mesh shader state") };
    let max_verts = int_val(&ms_state[1]);
    let nthreads = int_val(&m.items_of(&ms_state[0])[0]);
    let mut np = vec!["i32 0".to_string(), format!("i64 {}", flags | 0x10)];
    np.extend(new_props);
    let new_props = np;

    let sm_id = md_ref(&m.named_md("dx.shaderModel").unwrap()).unwrap();
    m.set_md(sm_id, &format!("!{{!\"vs\", i32 {smaj}, i32 {smin}}}"))?;
    let mask = m.add_md("!{i32 3, i32 1}");
    let semidx = m.add_md("!{i32 0}");
    let vid = m.add_md(&format!("!{{i32 0, !\"SV_VertexID\", i8 5, i8 1, !{semidx}, i8 0, i32 1, i8 1, i32 0, i8 0, !{mask}}}"));
    let inputs = m.add_md(&format!("!{{!{vid}}}"));
    let sig_id = m.add_md(&format!("!{{!{inputs}, {}, null}}", sig[1]));

    ensure_type(&mut m, "%struct.ByteAddressBuffer", "{ i32 }");
    ensure_type(&mut m, "%dx.types.ResRet.i32", "{ i32, i32, i32, i32, i32 }");
    ensure_type(&mut m, "%dx.types.ResBind", "{ i32, i32, i32, i8 }");
    ensure_type(&mut m, "%dx.types.ResourceProperties", "{ i32, i32 }");
    let res_ref = match md_ref(&eitems[3]) {
        Some(r) => r,
        None => {
            let r = m.add_md("!{null, null, null, null}");
            m.lines.push(format!("!dx.resources = !{{!{r}}}"));
            r
        }
    };
    let mut res = md_items(&m.md[&res_ref]).unwrap();
    let mut lst = list_of(&m, &res[0]);
    let nid = m.add_md(&format!("!{{i32 {}, %struct.ByteAddressBuffer* undef, !\"\", i32 {EMU_SPACE}, i32 1, i32 1, \
                                 i32 11, i32 0, null}}", lst.len()));
    lst.push(format!("!{nid}"));
    res[0] = format!("!{}", m.add_md(&format!("!{{{}}}", lst.join(", "))));
    let offset_lines = add_group_offset(&mut m, &mut res);
    m.set_md(res_ref, &format!("!{{{}}}", res.join(", ")))?;
    eitems[2] = format!("!{sig_id}");
    eitems[3] = format!("!{res_ref}");
    eitems[4] = format!("!{}", m.add_md(&format!("!{{{}}}", new_props.join(", "))));
    m.set_md(eid, &format!("!{{{}}}", eitems.join(", ")))?;
    m.lines.retain(|ln| !ln.starts_with("!dx.viewIdState"));

    let nounwind = attr_group(&mut m, "nounwind");
    let readnone = attr_group(&mut m, "nounwind readnone");
    let readonly = attr_group(&mut m, "nounwind readonly");
    ensure_decl(&mut m, "dx.op.loadInput.i32", format!("declare i32 @dx.op.loadInput.i32(i32, i32, i32, i8, i32) #{readnone}"));
    ensure_decl(&mut m, "dx.op.rawBufferLoad.i32",
                format!("declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) #{readonly}"));
    ensure_decl(&mut m, "dx.op.createHandleFromBinding",
                format!("declare %dx.types.Handle @dx.op.createHandleFromBinding(i32, %dx.types.ResBind, i32, i1) #{readnone}"));
    ensure_decl(&mut m, "dx.op.annotateHandle",
                format!("declare %dx.types.Handle @dx.op.annotateHandle(i32, %dx.types.Handle, %dx.types.ResourceProperties) #{readnone}"));
    let tys: BTreeSet<&str> = re!(r"@dx\.op\.storeVertexOutput\.(\w+)\(").captures_iter(ms_ll)
        .map(|c| c.get(1).unwrap().as_str()).collect();
    for ty in tys {
        let llty = match ty {
            "f32" => "float",
            "i32" => "i32",
            "f16" => "half",
            "i16" => "i16",
            _ => bail!("storeVertexOutput.{ty} unsupported"),
        };
        ensure_decl(&mut m, &format!("dx.op.storeOutput.{ty}"),
                    format!("declare void @dx.op.storeOutput.{ty}(i32, i32, i32, i8, {llty}) #{nounwind}"));
    }
    let decl_tid = Regex::new(&format!("^declare .*{THREAD_ID_OPS}")).unwrap();
    m.lines.retain(|ln| !(re!(r"^declare .*@dx\.op\.(setMeshOutputCounts|emitIndices|storeVertexOutput)").is_match(ln)
                          || decl_tid.is_match(ln)));

    let call_tid = Regex::new(&format!(r"^\s*(%[\w.]+) = call i32 {THREAD_ID_OPS}\(i32 (\d+)(?:, i32 (\d+))?\)$")).unwrap();
    let left = Regex::new(&format!(r"@dx\.op\.(setMeshOutputCounts|emitIndices|storeVertexOutput)|{THREAD_ID_OPS}")).unwrap();
    let define = format!("define void @{}()", entry_fn_name(&eitems));
    let (mut out, mut in_main) = (Vec::new(), false);
    for ln in &m.lines {
        if ln.starts_with(&define) {
            in_main = true;
            out.push(ln.clone());
            out.extend([
                "  %emu.vid = call i32 @dx.op.loadInput.i32(i32 4, i32 0, i32 0, i8 0, i32 undef)".to_string(),
                format!("  %emu.g = udiv i32 %emu.vid, {max_verts}"),
                format!("  %emu.t = urem i32 %emu.vid, {max_verts}"),
                format!("  %emu.ha = call %dx.types.Handle @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind \
                         {{ i32 1, i32 1, i32 {EMU_SPACE}, i8 0 }}, i32 1, i1 false)"),
                "  %emu.args = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %emu.ha, \
                 %dx.types.ResourceProperties { i32 11, i32 0 })".into(),
                "  %emu.d = call %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %emu.args, \
                 i32 0, i32 undef, i8 3, i32 4)".into(),
                "  %emu.dimx = extractvalue %dx.types.ResRet.i32 %emu.d, 0".into(),
                "  %emu.dimy = extractvalue %dx.types.ResRet.i32 %emu.d, 1".into(),
                "  %emu.cx = urem i32 %emu.g, %emu.dimx".into(),
                "  %emu.q = udiv i32 %emu.g, %emu.dimx".into(),
                "  %emu.cy = urem i32 %emu.q, %emu.dimy".into(),
                "  %emu.cz = udiv i32 %emu.q, %emu.dimy".into(),
            ]);
            out.extend(offset_lines.iter().cloned());
            out.extend([
                // Chunk-local group coordinates (G, the dims) plus the chunk's offset: the game's SV_GroupID.
                "  %emu.gx = add i32 %emu.cx, %emu.ox".to_string(),
                "  %emu.gy = add i32 %emu.cy, %emu.oy".into(),
                "  %emu.gz = add i32 %emu.cz, %emu.oz".into(),
                format!("  %emu.dx0 = mul i32 %emu.gx, {nthreads}"),
                "  %emu.dx = add i32 %emu.dx0, %emu.t".into(),
            ]);
            continue;
        }
        if in_main && ln.starts_with('}') {
            in_main = false;
        }
        if !in_main {
            out.push(ln.clone());
            continue;
        }
        let code = if ln.contains("@dx.op.") { ln.split(';').next().unwrap().trim_end().to_string() } else { ln.clone() };
        if let Some(c) = call_tid.captures(&code) {
            let c_ = c.get(4).map_or(0, |v| v.as_str().parse::<usize>().unwrap());
            let src: &[&str] = match &c[2] {
                "threadIdInGroup" => &["%emu.t", "0", "0"],
                "flattenedThreadIdInGroup" => &["%emu.t"],
                "groupId" => &["%emu.gx", "%emu.gy", "%emu.gz"],
                _ => &["%emu.dx", "%emu.gy", "%emu.gz"],
            };
            out.push(format!("  {} = add i32 {}, 0", &c[1], src[c_]));
            continue;
        }
        if re!(r"^\s*call void @dx\.op\.(setMeshOutputCounts|emitIndices)\(").is_match(&code) {
            continue;
        }
        if let Some(c) = re!(r"^\s*call void @dx\.op\.storeVertexOutput\.(\w+)\(i32 171, i32 (\d+), i32 ([^,]+), i8 (\d+), (\w+) ([^,]+), i32 ([^)]+)\)$")
            .captures(&code)
        {
            out.push(format!("  call void @dx.op.storeOutput.{}(i32 5, i32 {}, i32 {}, i8 {}, {} {})",
                             &c[1], &c[2], &c[3], &c[4], &c[5], &c[6]));
            continue;
        }
        if left.is_match(&code) {
            bail!("unhandled op form in VS mode: {}", code.trim());
        }
        out.push(ln.clone());
    }
    m.lines = drop_unused_decls(out);
    Ok(m.text())
}

// ------------------------------------------------------------------------------------------------
// Vertex shader generation
// ------------------------------------------------------------------------------------------------

fn hlsl_type(comp: i64, cols: i64) -> R<String> {
    let Some(base) = comp_type(comp) else { bail!("unsupported component type {comp}") };
    Ok(if cols == 1 { base.to_string() } else { format!("{base}{cols}") })
}

/// HLSL for the pass-through VS. ps_inputs None -> depth-only (position + cull distance).
pub fn gen_vs(meta: &Meta, ms_outputs: &[SigElem], ps_inputs: Option<&[SigElem]>) -> R<String> {
    let mut ms_by_sem: HashMap<(String, i64), &SigElem> = HashMap::new();
    for e in ms_outputs {
        for &si in &e.sem_idx {
            ms_by_sem.insert((e.name.to_uppercase(), si), e);
        }
    }
    let wanted: Vec<SigElem> = match ps_inputs {
        None => ms_outputs.iter()
            .filter(|e| ["SV_POSITION", "SV_CULLDISTANCE", "SV_CLIPDISTANCE"].contains(&e.name.to_uppercase().as_str()))
            .cloned()
            .collect(),
        Some(ps) => {
            let mut w: Vec<SigElem> = ps.iter()
                .filter(|e| !SYSTEM_GENERATED_PS_INPUTS.contains(&e.name.as_str()))
                .cloned()
                .collect();
            let names: HashSet<String> = w.iter().map(|e| e.name.to_uppercase()).collect();
            // Keep culling/clipping semantics even if the PS does not read them; position is mandatory.
            for e in ms_outputs {
                let n = e.name.to_uppercase();
                if ["SV_CULLDISTANCE", "SV_CLIPDISTANCE", "SV_POSITION"].contains(&n.as_str()) && !names.contains(&n) {
                    w.push(e.clone());
                }
            }
            w
        }
    };
    let stride = meta.vertex_bytes;
    let unit = if stride % 16 == 0 { 16 } else if stride % 8 == 0 { 8 } else { 4 };
    let n = unit / 4;
    let mut rows = BTreeSet::new();
    let mut comp = |byte: i64| -> String {
        let (r, lane) = (byte / unit, (byte % unit) / 4);
        rows.insert(r);
        if n == 1 { format!("r{r}") } else { format!("r{r}.{}", &"xyzw"[lane as usize..lane as usize + 1]) }
    };
    let (mut fields, mut reads) = (vec![], vec![]);
    for (i, e) in wanted.iter().enumerate() {
        if e.rows != 1 || e.sem_idx.len() != 1 {
            bail!("array signature element {} unsupported", e.name);
        }
        let si = e.sem_idx[0];
        let sv = e.name.to_uppercase().starts_with("SV_");
        let sem = if !sv || si != 0 { format!("{}{si}", e.name) } else { e.name.clone() };
        let ty = hlsl_type(e.comp, e.cols)?;
        let interp = interp_name(e.interp);
        fields.push(format!("    {interp} {ty} f{i} : {sem};").replace("     ", "    "));
        let Some(me) = ms_by_sem.get(&(e.name.to_uppercase(), si)) else {
            reads.push(format!("    o.f{i} = ({ty})0;  // not written by the mesh shader"));
            continue;
        };
        let off = meta.layout[&me.id];
        let comps: Vec<String> = (0..e.cols).map(|k| comp(off + 4 * k)).collect();
        let vec = if e.cols == 1 { comps[0].clone() } else { format!("uint{}({})", e.cols, comps.join(", ")) };
        match comp_type(e.comp).unwrap() {
            "float" | "min16float" => reads.push(format!("    o.f{i} = ({ty})asfloat({vec});")),
            "int" | "min16int" => reads.push(format!("    o.f{i} = ({ty})asint({vec});")),
            _ => reads.push(format!("    o.f{i} = ({ty})({vec});")),
        }
    }
    let row_ty = if n == 1 { "uint".to_string() } else { format!("uint{n}") };
    let loads: Vec<String> = rows.iter().map(|r| format!("    {row_ty} r{r} = Emu[vb + {r}];")).collect();
    Ok(format!(
        "// Generated by meshemu converter (ABI v5).\n\
         StructuredBuffer<{row_ty}> Emu : register(t0, space{EMU_SPACE});\n\
         \n\
         struct VSOut {{\n\
         {}\n\
         }};\n\
         \n\
         VSOut main(uint vid : SV_VertexID)\n\
         {{\n\
         \x20   VSOut o;\n\
         \x20   uint vb = {} + vid * {};\n\
         {}\n\
         {}\n\
         \x20   return o;\n\
         }}\n",
        fields.join("\n"), meta.idx_region / unit, stride / unit, loads.join("\n"), reads.join("\n")))
}

/// VS outputs must sit at the same rows/cols as the PS inputs they feed.
pub fn check_linkage(vs_ll: &str, ps_ll: &str) -> Vec<String> {
    let vs = Module::new(vs_ll);
    let (_, _, vsig) = entry_info(&vs);
    let vouts: HashMap<(String, Vec<i64>), SigElem> = parse_sig_elements(&vs, md_ref(&vsig[1])).into_iter()
        .map(|e| ((e.name.to_uppercase(), e.sem_idx.clone()), e))
        .collect();
    let ps = Module::new(ps_ll);
    let (_, _, psig) = entry_info(&ps);
    let mut problems = vec![];
    for e in parse_sig_elements(&ps, md_ref(&psig[0])) {
        if SYSTEM_GENERATED_PS_INPUTS.contains(&e.name.as_str()) {
            continue;
        }
        match vouts.get(&(e.name.to_uppercase(), e.sem_idx.clone())) {
            None => problems.push(format!("{}{:?} missing", e.name, e.sem_idx)),
            Some(v) if (v.start_row, v.start_col) != (e.start_row, e.start_col) => problems.push(format!(
                "{}{:?} at {}.{} vs PS {}.{}", e.name, e.sem_idx, v.start_row, v.start_col, e.start_row, e.start_col)),
            _ => {}
        }
    }
    problems
}

/// Input signature of a pixel shader (for kept_outputs / gen_vs).
pub fn ps_inputs(ps_ll: &str) -> Vec<SigElem> {
    let pm = Module::new(ps_ll);
    let (_, _, psig) = entry_info(&pm);
    parse_sig_elements(&pm, md_ref(&psig[0]))
}

// ------------------------------------------------------------------------------------------------
// Native 16-bit lowering (GPUs without Native16BitShaderOps, e.g. Pascal)
// ------------------------------------------------------------------------------------------------

const LOWER16_LOADS: [&str; 10] = ["bufferLoad", "textureLoad", "sample", "sampleLevel", "sampleGrad", "sampleBias",
                                   "sampleCmp", "sampleCmpLevelZero", "textureGather", "textureGatherCmp"];

/// 16-bit-float loads from typed resources become 32-bit loads (see convert.py lower_native16). Anything else
/// 16-bit is an error. Returns ll unchanged when the shader has no 16-bit types.
pub fn lower_native16(ll: &str) -> R<String> {
    if !re!(r"\bhalf\b|\bi16\b").is_match(ll) {
        return Ok(ll.to_string());
    }
    let mut m = Module::new(ll);
    let (_, eitems, _) = entry_info(&m);
    let loads = re!(r"@dx\.op\.(bufferLoad|textureLoad|sample|sampleLevel|sampleGrad|sampleBias|sampleCmp|sampleCmpLevelZero|textureGather|textureGatherCmp)\.f16\(");
    let mut lines = vec![];
    for ln in &m.lines {
        if ln.starts_with("%dx.types.ResRet.f16 = type") || re!(r"^declare .*@dx\.op\.\w+\.f16\(").is_match(ln) {
            continue; // replaced by the f32 forms below
        }
        let ln = if let Some(c) = re!(r"^(\s*%[\w.]+) = fpext half (%[\w.]+) to float(.*)$").captures(ln) {
            format!("{} = fmul float {}, 1.000000e+00{}", &c[1], &c[2], &c[3])
        } else {
            let mut l = loads.replace_all(ln, "@dx.op.${1}.f32(").into_owned();
            l = l.replace("%dx.types.ResRet.f16", "%dx.types.ResRet.f32");
            // Typed resource element type F16 (8) -> F32 (9) in handle annotations: props word 1, low byte.
            l = re!(r"%dx\.types\.ResourceProperties \{ i32 (\d+), i32 (\d+) \}")
                .replace_all(&l, |c: &regex::Captures| {
                    let w: i64 = c[2].parse().unwrap();
                    format!("%dx.types.ResourceProperties {{ i32 {}, i32 {} }}", &c[1], if w & 0xff == 8 { w + 1 } else { w })
                })
                .into_owned();
            // Resource struct type bodies: <N x half> / half -> float (the type names keep "half").
            if let Some(c) = re!(r#"^(%"?class\.[^=]+ = type )(.*)$"#).captures(&l) {
                l = format!("{}{}", &c[1], re!(r"\bhalf\b").replace_all(&c[2], "float"));
            }
            l
        };
        lines.push(ln);
    }
    m.lines = lines;
    m.md = parse_md(&m.lines);
    // Resource metadata: element type tag {0, 8 (F16)} -> {0, 9 (F32)} on SRV/UAV entries, in file order.
    let order: Vec<(i64, String)> = m.lines.iter()
        .filter_map(|ln| re!(r"^!(\d+) = (.*)$").captures(ln).map(|c| (c[1].parse().unwrap(), c[2].to_string())))
        .collect();
    for (i, body) in order {
        if let Some(c) = re!(r"^!\{i32 \d+, .*, !(\d+)\}$").captures(&body) {
            let tag: i64 = c[1].parse().unwrap();
            if body.contains("undef") && m.md.get(&tag).map(String::as_str) == Some("!{i32 0, i32 8}") {
                let new = m.add_md("!{i32 0, i32 9}");
                m.set_md(i, &format!("{}!{new}}}", &body[..body.rfind('!').unwrap()]))?;
            }
        }
    }
    // Shader flags: no longer uses native low precision (0x20).
    let props_ref = md_ref(&eitems[4]).unwrap();
    let mut props = m.items_of(&eitems[4]);
    let mut k = 0;
    while k + 1 < props.len() {
        if int_val(&props[k]) == 0 {
            props[k + 1] = format!("i64 {}", int_val(&props[k + 1]) & !0x20);
        }
        k += 2;
    }
    m.set_md(props_ref, &format!("!{{{}}}", props.join(", ")))?;
    ensure_type(&mut m, "%dx.types.ResRet.f32", "{ float, float, float, float, i32 }");
    let mut text = m.text();
    let ops: BTreeSet<String> = re!(r"@dx\.op\.(\w+)\.f32\(").captures_iter(&text).map(|c| c[1].to_string()).collect();
    for op in ops {
        let declared = Regex::new(&format!(r"(?m)^declare .*@dx\.op\.{op}\.f32\(")).unwrap();
        if LOWER16_LOADS.contains(&op.as_str()) && !declared.is_match(&text) {
            let call = Regex::new(&format!(r"call %dx\.types\.ResRet\.f32 @dx\.op\.{op}\.f32\(([^)]*)\)")).unwrap();
            let c = call.captures(&text).unwrap();
            let args = c[1].split(',')
                .map(|a| { let a = a.trim(); a.rsplit_once(' ').map_or(a, |(t, _)| t).to_string() })
                .collect::<Vec<_>>()
                .join(", ");
            let readonly = attr_group(&mut m, "nounwind readonly");
            m.lines.push(format!("declare %dx.types.ResRet.f32 @dx.op.{op}.f32({args}) #{readonly}"));
            text = m.text();
        }
    }
    let body = re!(r"(?sm)^define void @[\w.$]+\(\) \{\n(.*?)^\}").captures(&text).unwrap()[1].to_string();
    let left: BTreeSet<&str> = body.lines().filter(|l| re!(r"\bhalf\b|\bi16\b").is_match(l)).map(str::trim).collect();
    if let Some(first) = left.iter().next() {
        bail!("native 16-bit ops other than typed loads: {}", first.chars().take(160).collect::<String>());
    }
    Ok(text)
}
