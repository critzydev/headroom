//! Developer probe, not part of the app: how long does a DeepFilterNet frame take and does it allocate?
//!
//!   cargo run --release --example probe -- <model.tar.gz> [seconds] [noisy_mono_16bit_48k.wav]
//!
//! Counts heap allocations through a global allocator wrapper (this is how the "direct or worker thread" decision
//! for Headroom's noise removal was made).

use std::alloc::{GlobalAlloc, Layout, System};
use std::ffi::CString;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

use headroom_dfn::*;

struct Counting;
static ALLOCS: AtomicU64 = AtomicU64::new(0);
static BYTES: AtomicU64 = AtomicU64::new(0);
static FREES: AtomicU64 = AtomicU64::new(0);

unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        ALLOCS.fetch_add(1, Ordering::Relaxed);
        BYTES.fetch_add(l.size() as u64, Ordering::Relaxed);
        System.alloc(l)
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        FREES.fetch_add(1, Ordering::Relaxed);
        System.dealloc(p, l)
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
        ALLOCS.fetch_add(1, Ordering::Relaxed);
        BYTES.fetch_add(n as u64, Ordering::Relaxed);
        System.realloc(p, l, n)
    }
}

#[global_allocator]
static A: Counting = Counting;

fn read_wav(path: &str) -> Vec<f32> {
    let b = std::fs::read(path).expect("wav");
    let mut i = 12;
    while i + 8 <= b.len() {
        let id = &b[i..i + 4];
        let n = u32::from_le_bytes([b[i + 4], b[i + 5], b[i + 6], b[i + 7]]) as usize;
        if id == b"data" {
            return b[i + 8..i + 8 + n]
                .chunks_exact(2)
                .map(|c| i16::from_le_bytes([c[0], c[1]]) as f32 / 32768.0)
                .collect();
        }
        i += 8 + n;
    }
    panic!("no data chunk");
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let model_path = CString::new(args.get(1).expect("model path").as_str()).unwrap();
    let seconds: f32 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(20.0);

    let t0 = Instant::now();
    let mut err = [0 as std::ffi::c_char; 256];
    let model = unsafe { hr_df_model_load(model_path.as_ptr(), err.as_mut_ptr(), err.len()) };
    assert!(!model.is_null(), "model load failed");
    let load_ms = t0.elapsed().as_secs_f64() * 1000.0;

    let hop = unsafe { hr_df_model_frame_length(model) };
    let latency = unsafe { hr_df_model_latency(model) };

    let t1 = Instant::now();
    let state = unsafe { hr_df_state_new(model) };
    let clone_ms = t1.elapsed().as_secs_f64() * 1000.0;
    assert!(!state.is_null());

    let signal: Vec<f32> = match args.get(3) {
        Some(p) => read_wav(p),
        None => {
            // Noise plus a harmonic "voice" with a syllable rhythm; good enough to exercise every code path.
            let mut seed = 12345u32;
            (0..(48000.0 * seconds) as usize)
                .map(|n| {
                    seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
                    let noise = ((seed >> 8) as f32 / 8388608.0 - 1.0) * 0.05;
                    let t = n as f32 / 48000.0;
                    let env = (0.5 + 0.5 * (2.0 * std::f32::consts::PI * 3.0 * t).sin()).powi(2);
                    let voice: f32 = (1..12)
                        .map(|k| (2.0 * std::f32::consts::PI * 140.0 * k as f32 * t).sin() / k as f32)
                        .sum::<f32>()
                        * 0.15
                        * env;
                    voice + noise
                })
                .collect()
        }
    };

    let frames = signal.len() / hop;
    let warm = 300.min(frames / 4);
    let mut out = vec![0.0f32; hop];
    let mut times = Vec::with_capacity(frames);
    let mut allocs_after_warm = 0u64;
    let mut bytes_after_warm = 0u64;
    let mut worst_allocs = 0u64;
    let mut trace = String::new();

    for f in 0..frames {
        let a0 = ALLOCS.load(Ordering::Relaxed);
        let b0 = BYTES.load(Ordering::Relaxed);
        let t = Instant::now();
        let lsnr = unsafe { hr_df_state_process(state, signal[f * hop..].as_ptr(), out.as_mut_ptr()) };
        let dt = t.elapsed().as_secs_f64() * 1000.0;
        assert!(!lsnr.is_nan(), "frame failed");
        if std::env::var("PROBE_TRACE").is_ok() {
            let e: f32 = signal[f * hop..(f + 1) * hop].iter().map(|x| x * x).sum::<f32>() / hop as f32;
            let eo: f32 = out.iter().map(|x| x * x).sum::<f32>() / hop as f32;
            trace.push_str(&format!("{},{:.2},{:.2},{:.2}
", f, 10.0 * (e + 1e-12).log10(), 10.0 * (eo + 1e-12).log10(), lsnr));
        }
        let a = ALLOCS.load(Ordering::Relaxed) - a0;
        if f >= warm {
            times.push(dt);
            allocs_after_warm += a;
            bytes_after_warm += BYTES.load(Ordering::Relaxed) - b0;
            worst_allocs = worst_allocs.max(a);
        }
    }

    times.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let n = times.len();
    let mean: f64 = times.iter().sum::<f64>() / n as f64;
    println!("model load          {:8.1} ms", load_ms);
    println!("state from model    {:8.1} ms", clone_ms);
    println!("frame {} samples, latency after collection {} samples ({:.1} ms)", hop, latency, latency as f64 / 48.0);
    println!("frames measured     {} (after {} warm-up)", n, warm);
    println!(
        "frame time ms       mean {:.3}  p50 {:.3}  p99 {:.3}  p99.9 {:.3}  max {:.3}   ({:.1} % of a 10 ms frame on average)",
        mean,
        times[n / 2],
        times[(n as f64 * 0.99) as usize],
        times[((n as f64 * 0.999) as usize).min(n - 1)],
        times[n - 1],
        mean / 10.0 * 100.0
    );
    println!(
        "heap allocations    {} in {} frames = {:.1} per frame (worst frame {}), {:.0} bytes per frame",
        allocs_after_warm,
        n,
        allocs_after_warm as f64 / n as f64,
        worst_allocs,
        bytes_after_warm as f64 / n as f64
    );
    println!("frees {}", FREES.load(Ordering::Relaxed));
    if let Ok(path) = std::env::var("PROBE_TRACE") {
        std::fs::write(path, trace).unwrap();
    }

    unsafe {
        hr_df_state_free(state);
        hr_df_model_free(model);
    }
}
