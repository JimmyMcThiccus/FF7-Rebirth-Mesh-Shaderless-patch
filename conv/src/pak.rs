//! Reads the game's SM6 shaders straight from pakchunk0-WindowsNoEditor.pak: a UE 4.26 v11 pak whose index
//! is not encrypted and whose two shader archives (FSerializedShaderArchive) are stored uncompressed. Each
//! shader in an archive is LZ4-compressed and starts with a short UE header before its DXBC container.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

pub const PAK: &str = "End/Content/Paks/pakchunk0-WindowsNoEditor.pak";
const ARCHIVES: [&str; 2] = [
    "End/Content/ShaderArchive-Global-PCD3D_SM6.ushaderbytecode",
    "End/Content/ShaderArchive-End-PCD3D_SM6.ushaderbytecode",
];
const PAK_MAGIC: u32 = 0x5A6F12E1;
const ENTRY_HEADER_V11: u64 = 53; // serialized FPakEntry in front of a stored, unencrypted entry's data

pub const KIND_MS: u32 = 13;
pub const NEEDS_NATIVE16: u64 = 0x40000; // SFI0: D3D_SHADER_REQUIRES_NATIVE_16BIT_OPS

struct Reader<'a> {
    d: &'a [u8],
    p: usize,
}

impl<'a> Reader<'a> {
    fn bytes(&mut self, n: usize) -> Result<&'a [u8], String> {
        let s = self.d.get(self.p..self.p + n).ok_or("pak: read past the end of a table")?;
        self.p += n;
        Ok(s)
    }
    fn u8(&mut self) -> Result<u8, String> { Ok(self.bytes(1)?[0]) }
    fn u32(&mut self) -> Result<u32, String> { Ok(u32::from_le_bytes(self.bytes(4)?.try_into().unwrap())) }
    fn i32(&mut self) -> Result<i32, String> { Ok(self.u32()? as i32) }
    fn u64(&mut self) -> Result<u64, String> { Ok(u64::from_le_bytes(self.bytes(8)?.try_into().unwrap())) }
    fn skip(&mut self, n: usize) -> Result<(), String> { self.bytes(n).map(|_| ()) }
    fn skip_array(&mut self, item: usize) -> Result<(), String> {
        let n = self.i32()?.max(0) as usize;
        self.skip(item * n)
    }
    /// FString: length with the terminator; negative for UTF-16.
    fn fstring(&mut self) -> Result<String, String> {
        let n = self.i32()?;
        let s = if n < 0 {
            let b = self.bytes(2 * (-n) as usize)?;
            String::from_utf16_lossy(&b.chunks(2).map(|c| u16::from_le_bytes([c[0], c[1]])).collect::<Vec<_>>())
        } else {
            self.bytes(n as usize)?.iter().map(|&c| c as char).collect()
        };
        Ok(s.trim_end_matches('\0').to_string())
    }
}

fn read_at(f: &mut File, at: u64, n: usize) -> Result<Vec<u8>, String> {
    f.seek(SeekFrom::Start(at)).map_err(|e| e.to_string())?;
    let mut v = Vec::with_capacity(n);
    f.take(n as u64).read_to_end(&mut v).map_err(|e| e.to_string())?;
    Ok(v)
}

/// Data offsets of the shader archives (stored, unencrypted entries of the v11 pak index).
fn archive_offsets(f: &mut File) -> Result<Vec<u64>, String> {
    let len = f.seek(SeekFrom::End(0)).map_err(|e| e.to_string())?;
    let tail = read_at(f, len.saturating_sub(512), 512)?;
    let i = tail.windows(4).rposition(|w| w == PAK_MAGIC.to_le_bytes()).ok_or("pak: no footer")?;
    let mut t = Reader { d: &tail, p: i + 4 };
    let (version, index_offset, index_size) = (t.u32()?, t.u64()?, t.u64()?);
    if version != 11 || i == 0 || tail[i - 1] != 0 {
        return Err(format!("pak: expected an unencrypted v11 index (version {version})"));
    }
    let index = read_at(f, index_offset, index_size as usize)?;
    let mut r = Reader { d: &index, p: 0 };
    r.fstring()?; // mount point
    r.i32()?; // file count
    r.u64()?; // path hash seed
    if r.i32()? != 0 {
        r.skip(36)?; // path hash index: offset, size, hash
    }
    r.i32()?; // has full directory index
    let (fdi_offset, fdi_size) = (r.u64()?, r.u64()?);
    r.skip(20)?; // full directory index hash
    let n = r.i32()? as usize;
    let encoded = r.bytes(n)?;

    let fdi = read_at(f, fdi_offset, fdi_size as usize)?;
    let mut d = Reader { d: &fdi, p: 0 };
    let mut found = vec![None; ARCHIVES.len()];
    for _ in 0..d.i32()? {
        let dir = d.fstring()?;
        for _ in 0..d.i32()? {
            let name = dir.clone() + &d.fstring()?;
            let at = d.i32()?;
            let Some(k) = ARCHIVES.iter().position(|a| *a == name) else { continue };
            if at < 0 {
                continue;
            }
            let mut e = Reader { d: encoded, p: at as usize };
            let bits = e.u32()?;
            if bits & 0x3f == 0x3f {
                e.u32()?; // compression block size
            }
            let offset = if bits >> 31 & 1 != 0 { e.u32()? as u64 } else { e.u64()? };
            if (bits >> 23) & 0x3f != 0 || (bits >> 22) & 1 != 0 {
                return Err(format!("pak: {name} is compressed or encrypted"));
            }
            found[k] = Some(offset + ENTRY_HEADER_V11);
        }
    }
    found
        .into_iter()
        .zip(ARCHIVES)
        .map(|(o, a)| o.ok_or_else(|| format!("pak: {a} not found")))
        .collect()
}

/// Every DXBC container in the game's SM6 shader archives, in archive order (duplicates included).
pub fn shaders(game_root: &Path) -> Result<Vec<Vec<u8>>, String> {
    let path = game_root.join(PAK);
    let mut f = File::open(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut out = Vec::new();
    for base in archive_offsets(&mut f)? {
        let head = read_at(&mut f, base, 64 << 20)?;
        let mut r = Reader { d: &head, p: 0 };
        r.u32()?; // version
        r.skip_array(20)?; // shader map hashes
        r.skip_array(20)?; // shader hashes
        r.skip_array(16)?; // shader map entries
        let n = r.i32()?.max(0) as usize;
        let mut entries = Vec::with_capacity(n);
        for _ in 0..n {
            let (offset, size, usize_) = (r.u64()?, r.u32()?, r.u32()?);
            r.u8()?; // frequency
            entries.push((offset, size as usize, usize_ as usize));
        }
        r.skip_array(16)?; // preload entries
        r.skip_array(4)?; // shader indices
        let code = base + r.p as u64;
        for (offset, size, usize_) in entries {
            let data = read_at(&mut f, code + offset, size)?;
            let data = if size != usize_ {
                lz4_flex::block::decompress(&data, usize_).map_err(|e| format!("pak: lz4: {e}"))?
            } else {
                data
            };
            if let Some(p) = data.windows(4).position(|w| w == b"DXBC") {
                if let Some(len) = data.get(p + 24..p + 28) {
                    let len = u32::from_le_bytes(len.try_into().unwrap()) as usize;
                    if let Some(c) = data.get(p..p + len) {
                        out.push(c.to_vec());
                    }
                }
            }
        }
    }
    Ok(out)
}

/// The proxy's shader id: the container's 16-byte digest at offset 4, as hex.
pub fn shader_id(container: &[u8]) -> String {
    container[4..20].iter().map(|b| format!("{b:02x}")).collect()
}

/// (DXIL shader kind, SFI0 feature flags) of a container.
pub fn classify(c: &[u8]) -> (Option<u32>, u64) {
    let rd32 = |at: usize| c.get(at..at + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()));
    let (mut kind, mut flags) = (None, 0);
    for k in 0..rd32(28).unwrap_or(0) as usize {
        let Some(at) = rd32(32 + 4 * k).map(|a| a as usize) else { break };
        match c.get(at..at + 4) {
            Some(b"DXIL") => kind = rd32(at + 8).map(|v| v >> 16),
            Some(b"SFI0") => {
                flags = c.get(at + 8..at + 16).map_or(0, |b| u64::from_le_bytes(b.try_into().unwrap()))
            }
            _ => {}
        }
    }
    (kind, flags)
}

/// Walks up from `dir` to the folder that contains the pak (the game root).
pub fn find_game_root(dir: &Path) -> Option<std::path::PathBuf> {
    dir.ancestors().find(|d| d.join(PAK).is_file()).map(|d| d.to_path_buf())
}
