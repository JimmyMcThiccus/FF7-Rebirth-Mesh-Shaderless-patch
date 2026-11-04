//! meshemu-conv: the patch's shader converter as one self-contained exe (no Python). It needs only
//! dxcompiler.dll and dxil.dll next to it.
//!
//!   meshemu-conv [convert] <dump dir> <cache dir>     convert.py's job: every dumped <ms>_<ps>.pair
//!   meshemu-conv --lower16 <in.dxil> <out.dxil>        convert.py --lower16
//!   meshemu-conv precache <game dir> <out dir> [--lower16] [--jobs N]
//!       converts every mesh shader in the game's pak ahead of time, before the game ever creates its
//!       pipelines. VS-mode outputs do not depend on the pixel shader, so they are stored per mesh shader:
//!       <out>/<ms>.cs10.dxil, <ms>.vs10.dxil (written last), <ms>.meta10. A mesh shader that needs the
//!       per-pair fallback gets <ms>.blocked (the reason) and is left to `convert` once the game uses it.
//!       --lower16 also writes <out>/lower16/<id>.dxil for every other shader that needs native 16-bit
//!       ops. <out>/stamp.txt (pak size and time, converter build) skips the whole scan when unchanged.

mod dxc;
mod ir;
mod pak;

use dxc::Dxc;
use ir::ConvertError;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Instant;

fn exe_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_else(|| ".".into())
}

fn err<E: std::fmt::Display>(e: E) -> ConvertError {
    ConvertError(e.to_string())
}

fn read(path: &Path) -> ir::R<Vec<u8>> {
    fs::read(path).map_err(|e| ConvertError(format!("{}: {e}", path.display())))
}

fn write(path: &Path, data: impl AsRef<[u8]>) -> ir::R<()> {
    fs::write(path, data).map_err(|e| ConvertError(format!("{}: {e}", path.display())))
}

fn with_ext(p: &Path, ext: &str) -> PathBuf {
    PathBuf::from(format!("{}{ext}", p.display()))
}

fn env_set(name: &str) -> bool {
    std::env::var(name).is_ok_and(|v| !v.is_empty())
}

/// Assemble with the error text shaped like dxasm.exe's ("dxasm failed:" + its stderr), as convert.py logs it.
fn assemble(dxc: &Dxc, ll: &str) -> ir::R<Vec<u8>> {
    dxc.assemble(ll).map_err(|e| ConvertError(format!("dxasm failed:\n{e}")))
}

fn disassemble(dxc: &Dxc, container: &[u8]) -> ir::R<String> {
    dxc.disassemble(container).map_err(|e| ConvertError(format!("dxc -dumpbin failed:\n{e}")))
}

/// convert.py's first-line summary of a failed VS-mode attempt.
fn vs_mode_why(e: &ConvertError) -> String {
    let s = e.0.trim();
    if s.contains("validation failed") {
        let errs: Vec<&str> = s.lines().filter(|l| l.contains("error")).map(str::trim).collect();
        return format!("VS validation: {}", errs.join(" | ").chars().take(300).collect::<String>());
    }
    s.lines().next().map_or("VS conversion failed".into(), str::to_string)
}

// ------------------------------------------------------------------------------------------------
// convert: the dumped pairs (convert.py main)
// ------------------------------------------------------------------------------------------------

fn convert_pair(dxc: &Dxc, dump: &Path, cache: &Path, ms: &str, ps: &str) -> ir::R<String> {
    let pair = format!("{ms}_{ps}");
    let vs_out = cache.join(format!("{pair}.vs10.dxil"));
    if vs_out.exists() {
        return Ok(String::new());
    }
    let (ps_inputs, ps_ll) = if ps != "none" {
        let ll = disassemble(dxc, &read(&dump.join(format!("{ps}.ps.dxil")))?)?;
        (Some(ir::ps_inputs(&ll)), Some(ll))
    } else {
        (None, None)
    };
    let ms_ll = ir::lower_native16(&disassemble(dxc, &read(&dump.join(format!("{ms}.ms.dxil")))?)?)?;
    let tmp = with_ext(&vs_out, ".tmp");

    // VS mode: index-only CS + the mesh shader itself as the VS. Falls back to the v5 scheme when the shader
    // does not qualify or the VS does not link.
    let mut why = if env_set("MESHEMU_NO_VS_MODE") { Some("disabled".to_string()) } else { ir::vs_mode_blocker(&ms_ll)? };
    if why.is_none() {
        let attempt = || -> ir::R<Vec<u8>> {
            let vs_ll = ir::convert_ms_to_vs(&ms_ll)?;
            write(&cache.join(format!("{pair}.vs10.ll")), &vs_ll)?;
            let vs = assemble(dxc, &vs_ll)?;
            if let Some(ps_ll) = &ps_ll {
                let problems = ir::check_linkage(&disassemble(dxc, &vs)?, ps_ll);
                if !problems.is_empty() {
                    return Err(ConvertError(format!("VS/PS linkage mismatch: {}", problems.join("; "))));
                }
            }
            Ok(vs)
        };
        match attempt() {
            Ok(vs) => write(&tmp, vs)?,
            Err(e) => why = Some(vs_mode_why(&e)),
        }
    }
    let vs_mode = why.is_none();

    let (cs_ll, meta, outputs) = ir::convert_ms(&ms_ll, ps_inputs.as_deref(), vs_mode)?;
    write(&cache.join(format!("{pair}.cs10.ll")), &cs_ll)?;
    write(&cache.join(format!("{pair}.cs10.dxil")), assemble(dxc, &cs_ll)?)?;

    if !vs_mode {
        let hlsl_path = cache.join(format!("{pair}.vs10.hlsl"));
        let hlsl = ir::gen_vs(&meta, &outputs, ps_inputs.as_deref())?;
        write(&hlsl_path, &hlsl)?;
        let vs = dxc.compile(&hlsl, &hlsl_path.to_string_lossy(), "vs_6_6", &["-O3"])
            .map_err(|e| ConvertError(format!("dxc failed:\n{e}")))?;
        if let Some(ps_ll) = &ps_ll {
            let problems = ir::check_linkage(&disassemble(dxc, &vs)?, ps_ll);
            if !problems.is_empty() {
                return Err(ConvertError(format!("VS/PS linkage mismatch: {}", problems.join("; "))));
            }
        }
        write(&tmp, vs)?;
    }
    write(&cache.join(format!("{pair}.meta10")), meta.file())?;
    fs::rename(&tmp, &vs_out).map_err(err)?; // last: its presence marks the pair as converted
    Ok(if vs_mode {
        format!("  {pair}: VS mode (mesh shader as vertex shader, index-only CS), maxGroups {}", meta.max_groups)
    } else {
        format!("  {pair}: fallback ({}): {}B/vertex, maxGroups {}", why.unwrap(), meta.vertex_bytes, meta.max_groups)
    })
}

fn convert_main(dump: &Path, cache: &Path) -> Result<(), String> {
    fs::create_dir_all(cache).map_err(|e| e.to_string())?;
    let dxc = Dxc::load(&exe_dir())?;
    let mut pairs: Vec<String> = fs::read_dir(dump).map_err(|e| format!("{}: {e}", dump.display()))?
        .filter_map(|e| e.ok()?.file_name().to_str()?.strip_suffix(".pair").map(String::from))
        .collect();
    pairs.sort();
    let (mut ok, mut fail) = (0, 0);
    for pair in pairs {
        let parts: Vec<&str> = pair.split('_').collect();
        if parts.len() > 2 {
            println!("skip {pair}: amplification shader");
            fail += 1;
            continue;
        }
        match guarded(|| convert_pair(&dxc, dump, cache, parts[0], parts[1])) {
            Ok(line) => {
                if !line.is_empty() {
                    println!("{line}");
                }
                ok += 1;
            }
            Err(e) => {
                println!("FAIL {pair}: {e}");
                fail += 1;
            }
        }
    }
    println!("{ok} converted, {fail} failed");
    Ok(())
}

// ------------------------------------------------------------------------------------------------
// --lower16 (convert.py --lower16)
// ------------------------------------------------------------------------------------------------

fn lower16(dxc: &Dxc, container: &[u8], dst: &Path) -> ir::R<()> {
    let ll = ir::lower_native16(&disassemble(dxc, container)?)?;
    write(&with_ext(dst, ".ll"), &ll)?;
    let tmp = with_ext(dst, ".tmp");
    write(&tmp, assemble(dxc, &ll)?)?;
    fs::rename(&tmp, dst).map_err(err)
}

// ------------------------------------------------------------------------------------------------
// precache: every mesh shader in the game's pak
// ------------------------------------------------------------------------------------------------

fn precache_ms(dxc: &Dxc, container: &[u8], out: &Path, id: &str) -> ir::R<Result<u64, String>> {
    let ms_ll = ir::lower_native16(&disassemble(dxc, container)?)?;
    if let Some(why) = ir::vs_mode_blocker(&ms_ll)? {
        return Ok(Err(why));
    }
    let vs = match ir::convert_ms_to_vs(&ms_ll).and_then(|ll| assemble(dxc, &ll)) {
        Ok(vs) => vs,
        // The whole error on one line: unlike convert.py's log, .blocked is read to find out why.
        Err(e) => {
            let lines: Vec<&str> = e.0.lines().map(str::trim).filter(|l| !l.is_empty()).collect();
            return Ok(Err(lines.join(" | ").chars().take(400).collect()));
        }
    };
    let (cs_ll, meta, _) = ir::convert_ms(&ms_ll, None, true)?;
    write(&out.join(format!("{id}.cs10.dxil")), assemble(dxc, &cs_ll)?)?;
    write(&out.join(format!("{id}.meta10")), meta.file())?;
    let vs_out = out.join(format!("{id}.vs10.dxil"));
    let tmp = with_ext(&vs_out, ".tmp");
    write(&tmp, &vs)?;
    fs::rename(&tmp, &vs_out).map_err(err)?; // last: its presence marks the shader as converted
    Ok(Ok(meta.max_groups as u64))
}

/// Runs f, turning a panic (a converter bug on an unexpected shader) into an error for that shader only.
fn guarded<T>(f: impl FnOnce() -> ir::R<T>) -> ir::R<T> {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).unwrap_or_else(|p| {
        let msg = p.downcast_ref::<&str>().map(|s| s.to_string())
            .or_else(|| p.downcast_ref::<String>().cloned())
            .unwrap_or_default();
        Err(ConvertError(format!("converter panic: {msg}")))
    })
}

fn file_stamp(p: &Path) -> String {
    match fs::metadata(p) {
        Ok(m) => {
            let t = m.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs());
            format!("{} {t}", m.len())
        }
        Err(_) => "missing".into(),
    }
}

enum Job {
    Mesh(String, Vec<u8>),
    Lower16(String, Vec<u8>),
}

#[derive(Default)]
struct Stats {
    vs_mode: usize,
    blocked: Vec<(String, String)>,
    failed: Vec<(String, String)>,
    lowered: usize,
}

fn precache_main(game: &Path, out: &Path, want_lower16: bool, jobs: usize) -> Result<(), String> {
    let t0 = Instant::now();
    let root = pak::find_game_root(game).ok_or_else(|| format!("no {} above {}", pak::PAK, game.display()))?;
    fs::create_dir_all(out.join("lower16")).map_err(|e| e.to_string())?;
    let tool = format!("meshemu-conv {} abi10 exe {}", env!("CARGO_PKG_VERSION"),
                       std::env::current_exe().map_or("?".into(), |p| file_stamp(&p)));
    let stamp = format!("{tool}\npak {}\nlower16 {want_lower16}\n", file_stamp(&root.join(pak::PAK)));
    let stamp_path = out.join("stamp.txt");
    let old = fs::read_to_string(&stamp_path).unwrap_or_default();
    if old == stamp {
        println!("precache up to date");
        return Ok(());
    }
    if old.lines().next() != Some(tool.as_str()) {
        // A different converter build: its outputs may differ, so start over.
        let mut removed = 0;
        for dir in [out.to_path_buf(), out.join("lower16")] {
            for e in fs::read_dir(&dir).map_err(|e| e.to_string())?.flatten() {
                if e.path().is_file() && fs::remove_file(e.path()).is_ok() {
                    removed += 1;
                }
            }
        }
        if removed > 0 {
            println!("converter changed: removed {removed} old precache files");
        }
    }

    let shaders = pak::shaders(&root)?;
    let mut seen = std::collections::HashSet::new();
    let (mut todo, mut have, mut mesh) = (Vec::new(), 0, 0);
    for c in shaders {
        let id = pak::shader_id(&c);
        if !seen.insert(id.clone()) {
            continue;
        }
        let (kind, flags) = pak::classify(&c);
        if kind == Some(pak::KIND_MS) {
            mesh += 1;
            if out.join(format!("{id}.vs10.dxil")).exists() || out.join(format!("{id}.blocked")).exists() {
                have += 1;
            } else {
                todo.push(Job::Mesh(id, c));
            }
        } else if want_lower16 && flags & pak::NEEDS_NATIVE16 != 0 {
            if !out.join("lower16").join(format!("{id}.dxil")).exists() {
                todo.push(Job::Lower16(id, c));
            }
        }
    }
    println!("{} unique shaders in the pak, {mesh} mesh shaders ({have} already done), {} to convert",
             seen.len(), todo.len());

    let next = AtomicUsize::new(0);
    let done = AtomicUsize::new(0);
    let stats = Mutex::new(Stats::default());
    let total = todo.len();
    let load_err = Mutex::new(None);
    std::thread::scope(|s| {
        for _ in 0..jobs.max(1).min(total.max(1)) {
            s.spawn(|| {
                let dxc = match Dxc::load(&exe_dir()) {
                    Ok(d) => d,
                    Err(e) => {
                        *load_err.lock().unwrap() = Some(e);
                        return;
                    }
                };
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    let Some(job) = todo.get(i) else { break };
                    match job {
                        Job::Mesh(id, c) => {
                            let r = guarded(|| precache_ms(&dxc, c, out, id));
                            let mut st = stats.lock().unwrap();
                            match r {
                                Ok(Ok(_)) => st.vs_mode += 1,
                                Ok(Err(why)) => {
                                    let _ = fs::write(out.join(format!("{id}.blocked")), format!("{why}\n"));
                                    st.blocked.push((id.clone(), why));
                                }
                                Err(e) => {
                                    let _ = fs::write(out.join(format!("{id}.blocked")), format!("FAIL {e}\n"));
                                    st.failed.push((id.clone(), e.0));
                                }
                            }
                        }
                        Job::Lower16(id, c) => {
                            let r = guarded(|| lower16(&dxc, c, &out.join("lower16").join(format!("{id}.dxil"))));
                            let mut st = stats.lock().unwrap();
                            match r {
                                Ok(()) => st.lowered += 1,
                                Err(e) => st.failed.push((format!("lower16 {id}"), e.0)),
                            }
                        }
                    }
                    let d = done.fetch_add(1, Ordering::Relaxed) + 1;
                    if d % 250 == 0 || d == total {
                        println!("{d}/{total} shaders");
                    }
                }
            });
        }
    });
    if let Some(e) = load_err.into_inner().unwrap() {
        return Err(e);
    }
    let st = stats.into_inner().unwrap();
    let mut reasons = std::collections::BTreeMap::new();
    for (_, why) in &st.blocked {
        *reasons.entry(why.as_str()).or_insert(0) += 1;
    }
    for (why, n) in &reasons {
        println!("  {n} mesh shaders need per-pair conversion: {why}");
    }
    for (id, e) in &st.failed {
        println!("FAIL {id}: {}", e.lines().next().unwrap_or(""));
    }
    println!("precache: {} VS mode, {} per-pair, {} lowered 16-bit, {} failed, {:.1} s",
             st.vs_mode, st.blocked.len(), st.lowered, st.failed.len(), t0.elapsed().as_secs_f64());
    // A failed shader may succeed with a later converter, which changes the stamp anyway.
    fs::write(&stamp_path, stamp).map_err(|e| e.to_string())?;
    Ok(())
}

/// `extract <game dir> <out dir> [id...]`: writes the pak's containers (all, or the given ids) as <id>.dxil.
fn extract_main(game: &Path, out: &Path, ids: &[&str]) -> Result<(), String> {
    let root = pak::find_game_root(game).ok_or_else(|| format!("no {} above {}", pak::PAK, game.display()))?;
    fs::create_dir_all(out).map_err(|e| e.to_string())?;
    let mut n = 0;
    for c in pak::shaders(&root)? {
        let id = pak::shader_id(&c);
        if ids.is_empty() || ids.contains(&id.as_str()) {
            fs::write(out.join(format!("{id}.dxil")), &c).map_err(|e| e.to_string())?;
            n += 1;
        }
    }
    println!("{n} containers written");
    Ok(())
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let usage = "usage: meshemu-conv [convert] <dump> <cache> | --lower16 <in> <out> | \
                 precache <game dir> <out dir> [--lower16] [--jobs N]";
    let r = match args.iter().map(String::as_str).collect::<Vec<_>>().as_slice() {
        ["--lower16", src, dst] => {
            let name = Path::new(src).file_name().map_or(String::new(), |n| n.to_string_lossy().into_owned());
            let r = Dxc::load(&exe_dir()).map_err(ConvertError)
                .and_then(|dxc| guarded(|| lower16(&dxc, &read(Path::new(src))?, Path::new(dst))));
            match r {
                Ok(()) => {
                    println!("lowered {name}");
                    Ok(())
                }
                Err(e) => {
                    println!("FAIL lower16 {name}: {e}");
                    std::process::exit(1);
                }
            }
        }
        ["precache", game, out, rest @ ..] => {
            let mut lower16 = false;
            let mut jobs = std::thread::available_parallelism().map_or(4, |n| n.get());
            let mut it = rest.iter();
            let mut bad = false;
            while let Some(a) = it.next() {
                match *a {
                    "--lower16" => lower16 = true,
                    "--jobs" => match it.next().and_then(|n| n.parse().ok()) {
                        Some(n) => jobs = n,
                        None => bad = true,
                    },
                    _ => bad = true,
                }
            }
            if bad { Err(usage.to_string()) } else { precache_main(Path::new(game), Path::new(out), lower16, jobs) }
        }
        ["extract", game, out, ids @ ..] => extract_main(Path::new(game), Path::new(out), ids),
        ["convert", dump, cache] | [dump, cache] => convert_main(Path::new(dump), Path::new(cache)),
        _ => Err(usage.to_string()),
    };
    if let Err(e) = r {
        eprintln!("{e}");
        std::process::exit(2);
    }
}
