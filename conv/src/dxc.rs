//! The DirectX Shader Compiler through its COM interfaces: dxcompiler.dll (disassembler, assembler, HLSL
//! compiler) and the validator, which signs the container (dxil.dll if present, else dxcompiler.dll's own),
//! loaded from next to the exe. This
//! replaces dxc.exe and dxasm.exe; outputs are identical because the same DLL entry points do the work.
//! One `Dxc` per thread: the instances are not shared.

use std::ffi::c_void;
use std::path::Path;
use std::ptr::null_mut;

type HRESULT = i32;

#[repr(C)]
#[derive(Clone, Copy)]
struct Guid(u32, u16, u16, [u8; 8]);

const CLSID_DXC_COMPILER: Guid = Guid(0x73e22d93, 0xe6ce, 0x47f3, [0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0]);
const CLSID_DXC_LIBRARY: Guid = Guid(0x6245d6af, 0x66e0, 0x48fd, [0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c]);
const CLSID_DXC_VALIDATOR: Guid = Guid(0x8ca3e215, 0xf728, 0x4cf3, [0x8c, 0xdd, 0x88, 0xaf, 0x91, 0x75, 0x87, 0xa1]);
const CLSID_DXC_ASSEMBLER: Guid = Guid(0xd728db68, 0xf903, 0x4f80, [0x94, 0xcd, 0xdc, 0xcf, 0x76, 0xec, 0x71, 0x51]);
const IID_IDXC_LIBRARY: Guid = Guid(0xe5204dc7, 0xd18c, 0x4c3c, [0xbd, 0xfb, 0x85, 0x16, 0x73, 0x98, 0x0f, 0xe7]);
const IID_IDXC_COMPILER: Guid = Guid(0x8c210bf3, 0x011f, 0x4422, [0x8d, 0x70, 0x6f, 0x9a, 0xcb, 0x8d, 0xb6, 0x17]);
const IID_IDXC_ASSEMBLER: Guid = Guid(0x091f7a26, 0x1c1f, 0x4948, [0x90, 0x4b, 0xe6, 0xe3, 0xa8, 0xa7, 0x71, 0xd5]);
const IID_IDXC_VALIDATOR: Guid = Guid(0xa6e82bd2, 0x1fd7, 0x4826, [0x98, 0x11, 0x28, 0x57, 0xe7, 0x97, 0xf4, 0x9a]);
const CP_UTF8: u32 = 65001;
const VALIDATOR_IN_PLACE_EDIT: u32 = 1;

type CreateInstance = unsafe extern "system" fn(*const Guid, *const Guid, *mut *mut c_void) -> HRESULT;

#[link(name = "kernel32")]
extern "system" {
    fn LoadLibraryW(name: *const u16) -> *mut c_void;
    fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void;
}

/// An owned COM interface pointer (released on drop).
struct Com(*mut c_void);

impl Com {
    /// The function in vtable slot `slot` (IUnknown takes 0..2).
    unsafe fn method<F: Copy>(&self, slot: usize) -> F {
        let vtbl = *(self.0 as *const *const *const c_void);
        std::mem::transmute_copy(&*vtbl.add(slot))
    }
}

impl Drop for Com {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe {
                let release: unsafe extern "system" fn(*mut c_void) -> u32 = self.method(2);
                release(self.0);
            }
        }
    }
}

/// IDxcBlob::GetBufferPointer / GetBufferSize (also IDxcBlobEncoding).
fn blob_bytes(b: &Com) -> Vec<u8> {
    unsafe {
        let ptr: unsafe extern "system" fn(*mut c_void) -> *const u8 = b.method(3);
        let size: unsafe extern "system" fn(*mut c_void) -> usize = b.method(4);
        let (p, n) = (ptr(b.0), size(b.0));
        if p.is_null() || n == 0 { Vec::new() } else { std::slice::from_raw_parts(p, n).to_vec() }
    }
}

fn blob_text(b: &Com) -> String {
    let mut v = blob_bytes(b);
    while v.last() == Some(&0) {
        v.pop();
    }
    String::from_utf8_lossy(&v).into_owned()
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

pub struct Dxc {
    library: Com,
    compiler: Com,
    assembler: Com,
    validator: Com,
}

// Each thread creates its own instance; moving one to the thread that uses it is fine.
unsafe impl Send for Dxc {}

impl Dxc {
    /// Loads dxcompiler.dll and dxil.dll from `dir`.
    pub fn load(dir: &Path) -> Result<Dxc, String> {
        unsafe {
            let load = |name: &str| -> Result<CreateInstance, String> {
                let m = LoadLibraryW(wide(&dir.join(name).to_string_lossy()).as_ptr());
                if m.is_null() {
                    return Err(format!("{name} not found in {}", dir.display()));
                }
                let f = GetProcAddress(m, b"DxcCreateInstance\0".as_ptr());
                if f.is_null() {
                    return Err(format!("{name} has no DxcCreateInstance"));
                }
                Ok(std::mem::transmute::<*mut c_void, CreateInstance>(f))
            };
            // dxil.dll first when present: dxcompiler.dll looks for it by name and finds the loaded module.
            // Without it, dxcompiler.dll's own validator signs (DXC 1.8.2502 open-sourced the validator hash):
            // byte-identical containers, so the release ships only the open-source dxcompiler.dll.
            let dxil = if dir.join("dxil.dll").is_file() { Some(load("dxil.dll")?) } else { None };
            let comp = load("dxcompiler.dll")?;
            let dxil = dxil.unwrap_or(comp);
            let make = |f: CreateInstance, clsid: &Guid, iid: &Guid, what: &str| -> Result<Com, String> {
                let mut p = null_mut();
                let hr = f(clsid, iid, &mut p);
                if hr < 0 || p.is_null() { Err(format!("DxcCreateInstance({what}) failed {hr:08x}")) } else { Ok(Com(p)) }
            };
            Ok(Dxc {
                library: make(comp, &CLSID_DXC_LIBRARY, &IID_IDXC_LIBRARY, "library")?,
                compiler: make(comp, &CLSID_DXC_COMPILER, &IID_IDXC_COMPILER, "compiler")?,
                assembler: make(comp, &CLSID_DXC_ASSEMBLER, &IID_IDXC_ASSEMBLER, "assembler")?,
                validator: make(dxil, &CLSID_DXC_VALIDATOR, &IID_IDXC_VALIDATOR, "validator")?,
            })
        }
    }

    /// IDxcLibrary::CreateBlobWithEncodingFromPinned; `data` must outlive the blob.
    fn pinned(&self, data: &[u8], code_page: u32) -> Result<Com, String> {
        unsafe {
            let f: unsafe extern "system" fn(*mut c_void, *const c_void, u32, u32, *mut *mut c_void) -> HRESULT =
                self.library.method(6);
            let mut p = null_mut();
            let hr = f(self.library.0, data.as_ptr() as *const c_void, data.len() as u32, code_page, &mut p);
            if hr < 0 { Err(format!("CreateBlob failed {hr:08x}")) } else { Ok(Com(p)) }
        }
    }

    /// IDxcOperationResult status: Ok, or Err(error text).
    fn check(op: &Com) -> Result<(), String> {
        unsafe {
            let status: unsafe extern "system" fn(*mut c_void, *mut HRESULT) -> HRESULT = op.method(3);
            let errors: unsafe extern "system" fn(*mut c_void, *mut *mut c_void) -> HRESULT = op.method(5);
            let mut st = 0;
            if status(op.0, &mut st) >= 0 && st >= 0 {
                return Ok(());
            }
            let mut e = null_mut();
            if errors(op.0, &mut e) >= 0 && !e.is_null() {
                return Err(blob_text(&Com(e)));
            }
            Err(format!("status {st:08x}"))
        }
    }

    /// IDxcOperationResult: Ok(result blob) or Err(error text).
    fn result(op: Com) -> Result<Com, String> {
        Self::check(&op)?;
        unsafe {
            let result: unsafe extern "system" fn(*mut c_void, *mut *mut c_void) -> HRESULT = op.method(4);
            let mut p = null_mut();
            if result(op.0, &mut p) >= 0 && !p.is_null() { Ok(Com(p)) } else { Err("no result".into()) }
        }
    }

    /// `dxc -dumpbin`: the container's DXIL as LLVM text.
    pub fn disassemble(&self, container: &[u8]) -> Result<String, String> {
        let src = self.pinned(container, 0)?;
        unsafe {
            let f: unsafe extern "system" fn(*mut c_void, *mut c_void, *mut *mut c_void) -> HRESULT = self.compiler.method(5);
            let mut p = null_mut();
            let hr = f(self.compiler.0, src.0, &mut p);
            if hr < 0 || p.is_null() {
                return Err(format!("disassembly failed {hr:08x}"));
            }
            Ok(blob_text(&Com(p)))
        }
    }

    /// dxasm.exe: assemble LLVM text into a DXIL container, validate and sign it in place.
    pub fn assemble(&self, ll: &str) -> Result<Vec<u8>, String> {
        let src = self.pinned(ll.as_bytes(), CP_UTF8)?;
        unsafe {
            let asm: unsafe extern "system" fn(*mut c_void, *mut c_void, *mut *mut c_void) -> HRESULT = self.assembler.method(3);
            let mut op = null_mut();
            if asm(self.assembler.0, src.0, &mut op) < 0 || op.is_null() {
                return Err("assembly failed".into());
            }
            let container = Self::result(Com(op)).map_err(|e| format!("assembly failed\n{e}"))?;
            let val: unsafe extern "system" fn(*mut c_void, *mut c_void, u32, *mut *mut c_void) -> HRESULT =
                self.validator.method(3);
            let mut vop = null_mut();
            if val(self.validator.0, container.0, VALIDATOR_IN_PLACE_EDIT, &mut vop) < 0 || vop.is_null() {
                return Err("validation failed".into());
            }
            Self::check(&Com(vop)).map_err(|e| format!("validation failed\n{e}"))?;
            Ok(blob_bytes(&container))
        }
    }

    /// `dxc -T <target> -E main <args>`: compile HLSL (validated and signed by dxil.dll).
    pub fn compile(&self, hlsl: &str, name: &str, target: &str, args: &[&str]) -> Result<Vec<u8>, String> {
        let src = self.pinned(hlsl.as_bytes(), CP_UTF8)?;
        let (name, entry, target) = (wide(name), wide("main"), wide(target));
        let args: Vec<Vec<u16>> = args.iter().map(|a| wide(a)).collect();
        let argp: Vec<*const u16> = args.iter().map(|a| a.as_ptr()).collect();
        unsafe {
            #[allow(clippy::type_complexity)]
            let f: unsafe extern "system" fn(*mut c_void, *mut c_void, *const u16, *const u16, *const u16, *const *const u16,
                                             u32, *const c_void, u32, *mut c_void, *mut *mut c_void) -> HRESULT =
                self.compiler.method(3);
            let mut op = null_mut();
            let hr = f(self.compiler.0, src.0, name.as_ptr(), entry.as_ptr(), target.as_ptr(), argp.as_ptr(),
                       argp.len() as u32, std::ptr::null(), 0, null_mut(), &mut op);
            if hr < 0 || op.is_null() {
                return Err(format!("compile failed {hr:08x}"));
            }
            Self::result(Com(op)).map(|b| blob_bytes(&b)).map_err(|e| format!("compile failed\n{e}"))
        }
    }
}
