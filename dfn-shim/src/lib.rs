//! C interface of DeepFilterNet for Headroom (see include/hr_dfn.h).
//!
//! libDF's own C API (`capi`) `expect()`s on every error, and since Rust 1.81 a panic that reaches an `extern "C"`
//! boundary aborts the process. Here every entry point catches panics and reports them as errors instead, the
//! embedded default model is not compiled in (the models are files next to the program), and a state can be
//! created from a loaded model without parsing the network again (libDF's `DfTract` is `Clone`).

use std::cell::Cell;
use std::ffi::{c_char, CStr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;

use df::tract::{DfParams, DfTract, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};

pub struct HrDfModel {
    path: PathBuf,
    /// The network parsed by `hr_df_model_load`. The first state takes it; later states parse the file again (0.15-0.5 s),
    /// because tract's tensors are reference counted without atomics and two states that share any of them cannot
    /// be used on different threads. Only touched under the caller's lock (see DfnRuntime.cpp).
    first: Cell<Option<DfTract>>,
    hop: usize,
    sr: usize,
    latency: usize,
}

pub struct HrDfState {
    df: DfTract,
    hop: usize,
    failed: bool,
}

fn write_err(err: *mut c_char, len: usize, msg: &str) {
    if err.is_null() || len == 0 {
        return;
    }
    let bytes = msg.as_bytes();
    let n = bytes.len().min(len - 1);
    // SAFETY: the caller promised `len` writable bytes at `err`.
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), err as *mut u8, n);
        *err.add(n) = 0;
    }
}

fn panic_text(p: &Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = p.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = p.downcast_ref::<String>() {
        s.clone()
    } else {
        "internal error".to_string()
    }
}

fn parse(path: &PathBuf) -> Result<DfTract, String> {
    let params = DfParams::new(path.clone()).map_err(|e| format!("{e:#}"))?;
    // No internal attenuation limit and no post filter: Headroom blends and switches those itself.
    let rt = RuntimeParams::default_with_ch(1).with_atten_lim(100.0).with_post_filter(0.0);
    DfTract::new(params, &rt).map_err(|e| format!("{e:#}"))
}

/// # Safety
/// `path` must be a valid NUL-terminated string; `err` (if not null) must have room for `err_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn hr_df_model_load(
    path: *const c_char,
    err: *mut c_char,
    err_len: usize,
) -> *mut HrDfModel {
    let result = catch_unwind(AssertUnwindSafe(|| -> Result<Box<HrDfModel>, String> {
        if path.is_null() {
            return Err("no path".into());
        }
        let path = PathBuf::from(CStr::from_ptr(path).to_str().map_err(|_| "path is not UTF-8".to_string())?);
        let proto = parse(&path)?;
        let (hop, sr) = (proto.hop_size, proto.sr);
        let latency = (proto.fft_size - proto.hop_size) + proto.lookahead * proto.hop_size;
        Ok(Box::new(HrDfModel { path, first: Cell::new(Some(proto)), hop, sr, latency }))
    }));

    match result {
        Ok(Ok(model)) => Box::into_raw(model),
        Ok(Err(msg)) => {
            write_err(err, err_len, &msg);
            std::ptr::null_mut()
        }
        Err(p) => {
            write_err(err, err_len, &panic_text(&p));
            std::ptr::null_mut()
        }
    }
}

/// # Safety
/// `m` must come from `hr_df_model_load` (or be null) and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn hr_df_model_free(m: *mut HrDfModel) {
    if !m.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(m))));
    }
}

/// # Safety
/// `m` must be a valid model pointer.
#[no_mangle]
pub unsafe extern "C" fn hr_df_model_sample_rate(m: *const HrDfModel) -> usize {
    m.as_ref().map(|m| m.sr).unwrap_or(0)
}

/// # Safety
/// `m` must be a valid model pointer.
#[no_mangle]
pub unsafe extern "C" fn hr_df_model_frame_length(m: *const HrDfModel) -> usize {
    m.as_ref().map(|m| m.hop).unwrap_or(0)
}

/// # Safety
/// `m` must be a valid model pointer.
#[no_mangle]
pub unsafe extern "C" fn hr_df_model_latency(m: *const HrDfModel) -> usize {
    m.as_ref().map(|m| m.latency).unwrap_or(0)
}

/// # Safety
/// `m` must be a valid model pointer.
#[no_mangle]
pub unsafe extern "C" fn hr_df_state_new(m: *const HrDfModel) -> *mut HrDfState {
    let Some(m) = m.as_ref() else { return std::ptr::null_mut() };
    match catch_unwind(AssertUnwindSafe(|| {
        let df = match m.first.take() {
            Some(df) => df,
            None => parse(&m.path).ok()?,
        };
        let hop = df.hop_size;
        Some(Box::new(HrDfState { df, hop, failed: false }))
    })) {
        Ok(Some(s)) => Box::into_raw(s),
        _ => std::ptr::null_mut(),
    }
}

/// # Safety
/// `s` must come from `hr_df_state_new` (or be null) and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn hr_df_state_free(s: *mut HrDfState) {
    if !s.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(s))));
    }
}

/// # Safety
/// `s` must be a valid state; `input` and `output` must each point to one frame (`hr_df_model_frame_length` floats).
#[no_mangle]
pub unsafe extern "C" fn hr_df_state_process(
    s: *mut HrDfState,
    input: *const f32,
    output: *mut f32,
) -> f32 {
    let Some(s) = s.as_mut() else { return f32::NAN };
    let hop = s.hop;

    if s.failed {
        std::ptr::copy_nonoverlapping(input, output, hop);
        return f32::NAN;
    }

    let r = catch_unwind(AssertUnwindSafe(|| {
        let i = std::slice::from_raw_parts(input, hop);
        let o = std::slice::from_raw_parts_mut(output, hop);
        let i = ArrayView2::from_shape((1, hop), i).unwrap();
        let o = ArrayViewMut2::from_shape((1, hop), o).unwrap();
        s.df.process(i, o)
    }));

    match r {
        Ok(Ok(lsnr)) => lsnr,
        _ => {
            s.failed = true;
            std::ptr::copy_nonoverlapping(input, output, hop);
            f32::NAN
        }
    }
}

/// # Safety
/// `s` must be a valid state.
#[no_mangle]
pub unsafe extern "C" fn hr_df_state_set_post_filter(s: *mut HrDfState, beta: f32) {
    if let Some(s) = s.as_mut() {
        let _ = catch_unwind(AssertUnwindSafe(|| s.df.set_pf_beta(beta.max(0.0))));
    }
}

/// # Safety
/// `s` must be a valid state.
#[no_mangle]
pub unsafe extern "C" fn hr_df_state_set_atten_limit(s: *mut HrDfState, db: f32) {
    if let Some(s) = s.as_mut() {
        let _ = catch_unwind(AssertUnwindSafe(|| s.df.set_atten_lim(db)));
    }
}

/// # Safety
/// `s` must be a valid state.
#[no_mangle]
pub unsafe extern "C" fn hr_df_state_set_thresholds(s: *mut HrDfState, min_db: f32, max_erb_db: f32, max_df_db: f32) {
    if let Some(s) = s.as_mut() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            s.df.min_db_thresh = min_db;
            s.df.max_db_erb_thresh = max_erb_db;
            s.df.max_db_df_thresh = max_df_db;
        }));
    }
}
