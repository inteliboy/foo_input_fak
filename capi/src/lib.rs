//! C ABI over the FAK library for the foobar2000 component (`include/fak_capi.h` is the contract).
//!
//! Design: the host opens a `.fak` file either as bytes ([`fak_decoder_open`], which keeps a copy) or
//! through a read callback ([`fak_decoder_open_cb`], which keeps nothing of the file). Either way the
//! container is validated once and audio is decoded one chunk at a time (`decoder::FileReader`), so
//! with the callback memory is a small chunk index, the metadata and one chunk (about 8 MB for
//! 24-bit/96 kHz stereo), not the file. Every entry point catches
//! panics: a malformed file must never unwind into C++ (the decoder is designed not to panic on
//! hostile input; this is the second line of defense). Errors are reported as a return code plus a
//! message retrievable with [`fak_decoder_last_error`] / the `err` out-buffer.

use std::ffi::{c_char, c_int, c_void};
use std::io::{Read, Seek, SeekFrom};
use std::panic::{catch_unwind, AssertUnwindSafe};

use fak::cdrip;
use fak::decoder::{self, FileReader, ReadSeek};
use fak::encoder::{self, Effort};
use fak::format;
use fak::metadata::{Metadata, Picture, PictureType};

// The encoder's and decoder's allocation churn makes the system heap a measurable cost on
// Windows; the component uses the same allocator as the CLI.
#[global_allocator]
static GLOBAL: mimalloc::MiMalloc = mimalloc::MiMalloc;

pub struct FakDecoder {
    reader: FileReader<Box<dyn ReadSeek>>,
    last_error: Vec<u8>,
    /// NUL-terminated copies of the tags and picture strings, so C can borrow stable pointers.
    tags: Vec<Vec<u8>>,
    mimes: Vec<Vec<u8>>,
    descriptions: Vec<Vec<u8>>,
    vendor: Vec<u8>,
    /// The cue sheet as text (the CUESHEET tag, or generated from the binary cue sheet); empty if none.
    cuesheet: Vec<u8>,
}

#[repr(C)]
pub struct FakInfo {
    pub channels: u32,
    pub bits_per_sample: u32,
    pub sample_rate: u32,
    /// 0 = block-independent (the only mode since format v19).
    pub mode: u32,
    pub total_frames: u64,
    pub chunk_count: u64,
    pub format_version: u32,
    /// 1 when the source WAV declared a speaker mask (`channel_mask` is then meaningful).
    pub has_channel_mask: u32,
    pub channel_mask: u32,
    /// 1 when the stream holds 32-bit float PCM ((b)): decode it with `fak_decoder_decode_chunk_float`;
    /// `bits_per_sample` is then the width of the internal integer mapping, not of the samples.
    pub is_float: u32,
    /// Sample-frames in a full chunk (the encoder's chunk length; the last chunk may be shorter).
    pub chunk_frames: u32,
    /// Data chunks per FEC parity block (0 = no error-recovery data) and the number of parity blocks.
    pub fec_group: u32,
    pub parity_blocks: u32,
    /// SHA-256 of the decoded PCM, as stored in the stream header.
    pub pcm_sha256: [u8; 32],
}

#[repr(C)]
pub struct FakPicture {
    /// FLAC/ID3 picture type (3 = front cover, 4 = back cover, 8 = artist, ...).
    pub kind: u32,
    pub mime: *const c_char,
    pub description: *const c_char,
    pub data: *const u8,
    pub data_len: usize,
}

fn cstring(s: &str) -> Vec<u8> {
    let mut v: Vec<u8> = s.bytes().map(|b| if b == 0 { b' ' } else { b }).collect();
    v.push(0);
    v
}

fn write_err(err: *mut c_char, cap: usize, msg: &str) {
    if err.is_null() || cap == 0 { return; }
    let n = msg.len().min(cap - 1);
    // Safety: the caller guarantees `err` points to `cap` writable bytes.
    unsafe {
        std::ptr::copy_nonoverlapping(msg.as_ptr(), err as *mut u8, n);
        *err.add(n) = 0;
    }
}

fn guard<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(fallback)
}

/// Reads `len` bytes at absolute position `pos` of the file into `buf`; returns 0 on success, non-zero
/// on failure. Must not throw or unwind (catch inside the host).
pub type FakReadFn = unsafe extern "C" fn(user: *mut c_void, pos: u64, buf: *mut u8, len: usize) -> c_int;

/// A file behind the host's read callback.
struct CbFile { read: FakReadFn, user: *mut c_void, size: u64, pos: u64 }
// Safety: the header contract makes `user` usable from whichever thread calls the decoder, one call at a time.
unsafe impl Send for CbFile {}

impl Read for CbFile {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let n = (buf.len() as u64).min(self.size.saturating_sub(self.pos)) as usize;
        if n == 0 { return Ok(0); }
        // Safety: `buf` holds `n` writable bytes; the callback contract is documented on `FakReadFn`.
        let rc = unsafe { (self.read)(self.user, self.pos, buf.as_mut_ptr(), n) };
        if rc != 0 { return Err(std::io::Error::other("read callback failed")); }
        self.pos += n as u64;
        Ok(n)
    }
}

impl Seek for CbFile {
    fn seek(&mut self, to: SeekFrom) -> std::io::Result<u64> {
        let p = match to {
            SeekFrom::Start(n) => Some(n),
            SeekFrom::End(d) => self.size.checked_add_signed(d),
            SeekFrom::Current(d) => self.pos.checked_add_signed(d),
        };
        self.pos = p.ok_or_else(|| std::io::Error::other("seek before the start"))?;
        Ok(self.pos)
    }
}

fn open_reader(src: Box<dyn ReadSeek>, err: *mut c_char, err_cap: usize) -> *mut FakDecoder {
    match FileReader::open(src) {
        Ok(reader) => {
                let m = &reader.metadata;
                let tags = m.tags.iter().map(|t| cstring(t)).collect();
                let mimes = m.pictures.iter().map(|p| cstring(&p.mime)).collect();
                let descriptions = m.pictures.iter().map(|p| cstring(&p.description)).collect();
                let vendor = cstring(&m.vendor);
                let cuesheet = m.cue_sheet_text(reader.header.sample_rate, "CDImage.wav").map_or_else(Vec::new, |t| cstring(&t));
                Box::into_raw(Box::new(FakDecoder { reader, last_error: vec![0], tags, mimes, descriptions, vendor, cuesheet }))
            }
            Err(e) => { write_err(err, err_cap, &e.0); std::ptr::null_mut() }
    }
}

/// Opens a stream from `len` bytes at `data` (copied). Returns null on failure, with a message in
/// `err` (up to `err_cap` bytes including the terminator).
#[no_mangle]
pub extern "C" fn fak_decoder_open(data: *const u8, len: usize, err: *mut c_char, err_cap: usize) -> *mut FakDecoder {
    guard(std::ptr::null_mut(), || {
        if data.is_null() && len != 0 { write_err(err, err_cap, "null data"); return std::ptr::null_mut(); }
        // Safety: the caller guarantees `data` points to `len` readable bytes.
        let bytes = if len == 0 { Vec::new() } else { unsafe { std::slice::from_raw_parts(data, len) }.to_vec() };
        open_reader(Box::new(std::io::Cursor::new(bytes)), err, err_cap)
    })
}

/// Opens a stream of `size` bytes that stays with the host: the library reads what it needs through
/// `read` (the header, the metadata, a small index, then one chunk at a time) and keeps a copy of
/// none of the file. `user` must stay valid, and `read` callable, until `fak_decoder_free`. Returns
/// null on failure, with a message in `err`.
#[no_mangle]
pub extern "C" fn fak_decoder_open_cb(read: FakReadFn, user: *mut c_void, size: u64, err: *mut c_char, err_cap: usize) -> *mut FakDecoder {
    guard(std::ptr::null_mut(), || open_reader(Box::new(CbFile { read, user, size, pos: 0 }), err, err_cap))
}

#[no_mangle]
pub extern "C" fn fak_decoder_free(d: *mut FakDecoder) {
    if d.is_null() { return; }
    // Safety: `d` came from `fak_decoder_open` and is freed once, per the header contract.
    let _ = guard((), || drop(unsafe { Box::from_raw(d) }));
}

/// Message of the last failed call on `d` (empty if none). Valid until the next call on `d`.
#[no_mangle]
pub extern "C" fn fak_decoder_last_error(d: *const FakDecoder) -> *const c_char {
    if d.is_null() { return c"null decoder".as_ptr(); }
    // Safety: valid handle per contract.
    unsafe { (*d).last_error.as_ptr() as *const c_char }
}

#[no_mangle]
pub extern "C" fn fak_decoder_info(d: *const FakDecoder, out: *mut FakInfo) -> c_int {
    if d.is_null() || out.is_null() { return -1; }
    guard(-1, || {
        // Safety: valid handle and out-pointer per contract.
        let (d, out) = unsafe { (&*d, &mut *out) };
        let h = &d.reader.header;
        *out = FakInfo {
            channels: h.channels as u32,
            bits_per_sample: h.bits_per_sample as u32,
            sample_rate: h.sample_rate,
            mode: h.mode as u32,
            total_frames: d.reader.total_frames,
            chunk_count: d.reader.chunk_count() as u64,
            format_version: fak::format::VERSION as u32,
            has_channel_mask: d.reader.metadata.channel_mask.is_some() as u32,
            channel_mask: d.reader.metadata.channel_mask.unwrap_or(0),
            is_float: d.reader.metadata.float_info.is_some() as u32,
            chunk_frames: if d.reader.chunk_count() > 0 { d.reader.chunk_frames(0) as u32 } else { 0 },
            fec_group: d.reader.fec_group() as u32,
            parity_blocks: d.reader.parity_count() as u32,
            pcm_sha256: h.pcm_hash,
        };
        0
    })
}

/// Index of the chunk containing sample-frame `frame`, or -1 past the end.
#[no_mangle]
pub extern "C" fn fak_decoder_chunk_for_frame(d: *const FakDecoder, frame: u64) -> i64 {
    if d.is_null() { return -1; }
    // Safety: valid handle per contract.
    let d = unsafe { &*d };
    d.reader.chunk_for_frame(frame).map_or(-1, |i| i as i64)
}

/// First sample-frame of chunk `i` (0 if out of range).
#[no_mangle]
pub extern "C" fn fak_decoder_chunk_start(d: *const FakDecoder, i: u64) -> u64 {
    if d.is_null() { return 0; }
    // Safety: valid handle per contract.
    let d = unsafe { &*d };
    if (i as usize) < d.reader.chunk_count() { d.reader.chunk_start(i as usize) } else { 0 }
}

/// Sample-frames in chunk `i` (0 if out of range).
#[no_mangle]
pub extern "C" fn fak_decoder_chunk_frames(d: *const FakDecoder, i: u64) -> u64 {
    if d.is_null() { return 0; }
    // Safety: valid handle per contract.
    let d = unsafe { &*d };
    if (i as usize) < d.reader.chunk_count() { d.reader.chunk_frames(i as usize) as u64 } else { 0 }
}

/// Decodes chunk `i` into `out` as interleaved signed 32-bit samples (the stream's integer values,
/// not rescaled: a 16-bit stream yields values in -32768..=32767). `out_cap` is the capacity in
/// samples (frames x channels). Returns the frame count, or -1 on error (see `last_error`).
#[no_mangle]
pub extern "C" fn fak_decoder_decode_chunk(d: *mut FakDecoder, i: u64, out: *mut i32, out_cap: usize) -> i64 {
    if d.is_null() || out.is_null() { return -1; }
    guard(-1, || {
        // Safety: valid handle per contract.
        let d = unsafe { &mut *d };
        let fail = |d: &mut FakDecoder, m: &str| { d.last_error = cstring(m); -1 };
        if i as usize >= d.reader.chunk_count() { return fail(d, "chunk index out of range"); }
        let chans = match d.reader.decode_chunk(i as usize) { Ok(c) => c, Err(e) => return fail(d, &e.0) };
        let nch = chans.len();
        let n = chans.first().map_or(0, |c| c.len());
        if n.checked_mul(nch).is_none_or(|t| t > out_cap) { return fail(d, "output buffer too small"); }
        // Safety: `out` holds `out_cap >= n * nch` samples per the check above.
        let dst = unsafe { std::slice::from_raw_parts_mut(out, n * nch) };
        for (c, ch) in chans.iter().enumerate() {
            for (f, &v) in ch.iter().enumerate() { dst[f * nch + c] = v as i32; }
        }
        n as i64
    })
}

/// Like `fak_decoder_decode_chunk` for a float stream (`FakInfo::is_float`): writes interleaved
/// float32 samples, bit-exact to the source. `out_cap` is the capacity in samples.
#[no_mangle]
pub extern "C" fn fak_decoder_decode_chunk_float(d: *mut FakDecoder, i: u64, out: *mut f32, out_cap: usize) -> i64 {
    if d.is_null() || out.is_null() { return -1; }
    guard(-1, || {
        // Safety: valid handle per contract.
        let d = unsafe { &mut *d };
        let fail = |d: &mut FakDecoder, m: &str| { d.last_error = cstring(m); -1 };
        if i as usize >= d.reader.chunk_count() { return fail(d, "chunk index out of range"); }
        let Some(info) = d.reader.metadata.float_info.clone() else { return fail(d, "not a float stream") };
        let chans = match d.reader.decode_chunk(i as usize) { Ok(c) => c, Err(e) => return fail(d, &e.0) };
        let nch = chans.len();
        let n = chans.first().map_or(0, |c| c.len());
        if n.checked_mul(nch).is_none_or(|t| t > out_cap) { return fail(d, "output buffer too small"); }
        let sliced = fak::floatpcm::slice_info(&info, d.reader.chunk_start(i as usize), n as u64);
        let bits = fak::floatpcm::unmap_from_pcm(&chans, &sliced);
        // Safety: `out` holds `out_cap >= n * nch` samples per the check above.
        let dst = unsafe { std::slice::from_raw_parts_mut(out, n * nch) };
        for (c, ch) in bits.iter().enumerate() {
            for (f, &v) in ch.iter().enumerate() { dst[f * nch + c] = f32::from_bits(v); }
        }
        n as i64
    })
}

#[no_mangle]
pub extern "C" fn fak_decoder_vendor(d: *const FakDecoder) -> *const c_char {
    if d.is_null() { return c"".as_ptr(); }
    // Safety: valid handle per contract.
    unsafe { (*d).vendor.as_ptr() as *const c_char }
}

/// The file's cue sheet as `.cue` text: the CUESHEET tag verbatim, or text generated from the
/// binary cue sheet (FILE "CDImage.wav"). Null if the file has neither. Valid until `fak_decoder_free`.
#[no_mangle]
pub extern "C" fn fak_decoder_cuesheet(d: *const FakDecoder) -> *const c_char {
    if d.is_null() { return std::ptr::null(); }
    // Safety: valid handle per contract.
    let d = unsafe { &*d };
    if d.cuesheet.is_empty() { std::ptr::null() } else { d.cuesheet.as_ptr() as *const c_char }
}

#[no_mangle]
pub extern "C" fn fak_decoder_tag_count(d: *const FakDecoder) -> usize {
    if d.is_null() { return 0; }
    // Safety: valid handle per contract.
    unsafe { (*d).tags.len() }
}

/// Tag `i` as a NUL-terminated UTF-8 "KEY=VALUE" string, or null if out of range.
#[no_mangle]
pub extern "C" fn fak_decoder_tag(d: *const FakDecoder, i: usize) -> *const c_char {
    if d.is_null() { return std::ptr::null(); }
    // Safety: valid handle per contract.
    let d = unsafe { &*d };
    d.tags.get(i).map_or(std::ptr::null(), |t| t.as_ptr() as *const c_char)
}

#[no_mangle]
pub extern "C" fn fak_decoder_picture_count(d: *const FakDecoder) -> usize {
    if d.is_null() { return 0; }
    // Safety: valid handle per contract.
    unsafe { (*d).reader.metadata.pictures.len() }
}

/// Picture `i`; returns 0 on success. Pointers stay valid while `d` is open.
#[no_mangle]
pub extern "C" fn fak_decoder_picture(d: *const FakDecoder, i: usize, out: *mut FakPicture) -> c_int {
    if d.is_null() || out.is_null() { return -1; }
    // Safety: valid handle and out-pointer per contract.
    let (d, out) = unsafe { (&*d, &mut *out) };
    let Some(p) = d.reader.metadata.pictures.get(i) else { return -1 };
    *out = FakPicture { kind: p.kind_raw as u32, mime: d.mimes[i].as_ptr() as *const c_char, description: d.descriptions[i].as_ptr() as *const c_char, data: p.data.as_ptr(), data_len: p.data.len() };
    0
}

/// Picture to store with [`fak_rewrite_metadata`]. Strings are NUL-terminated UTF-8.
#[repr(C)]
pub struct FakPictureIn {
    pub kind: u32,
    pub mime: *const c_char,
    pub description: *const c_char,
    pub data: *const u8,
    pub data_len: usize,
}

/// A byte buffer allocated by this library; release with [`fak_buffer_free`].
#[repr(C)]
pub struct FakBuffer { pub data: *mut u8, pub len: usize }

#[no_mangle]
pub extern "C" fn fak_buffer_free(b: FakBuffer) {
    if b.data.is_null() { return; }
    // Safety: `b` came from this library (a boxed slice of exactly `len` bytes).
    let _ = guard((), || drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(b.data, b.len)) }));
    release_memory();
}

/// Gives freed memory back to the OS. mimalloc keeps freed pages for reuse, which in a long-lived
/// host (foobar2000) shows as the process holding gigabytes after a conversion has finished.
fn release_memory() {
    // Safety: `mi_collect` may be called at any time from any thread.
    unsafe { libmimalloc_sys::mi_collect(true) };
}

unsafe fn c_str<'a>(p: *const c_char) -> Result<&'a str, String> {
    if p.is_null() { return Ok(""); }
    std::ffi::CStr::from_ptr(p).to_str().map_err(|_| "string is not valid UTF-8".to_string())
}

/// The metadata a retag produces: tags (and, if `replace_pictures`, pictures) replaced, the cue sheet
/// following the `CUESHEET` tag and the CD tags kept current (see `fak_rewrite_metadata`).
fn retagged(
    d: &mut FakDecoder, tags: *const *const c_char, tag_count: usize,
    replace_pictures: c_int, pictures: *const FakPictureIn, picture_count: usize,
) -> Result<Metadata, String> {
    let mut meta: Metadata = d.reader.metadata.clone();
    meta.tags = (0..tag_count).map(|i| unsafe { c_str(*tags.add(i)) }.map(str::to_owned)).collect::<Result<_, _>>()?;
    if replace_pictures != 0 {
        meta.pictures = (0..picture_count).map(|i| {
            let p = unsafe { &*pictures.add(i) };
            let kind_raw = u8::try_from(p.kind).map_err(|_| "picture type out of range".to_string())?;
            let data = if p.data_len == 0 { Vec::new() } else { unsafe { std::slice::from_raw_parts(p.data, p.data_len) }.to_vec() };
            Ok(Picture {
                kind: match kind_raw { 3 => PictureType::FrontCover, 4 => PictureType::BackCover, 8 => PictureType::Artist, _ => PictureType::Other },
                kind_raw, mime: unsafe { c_str(p.mime) }?.to_owned(), description: unsafe { c_str(p.description) }?.to_owned(),
                width: 0, height: 0, depth: 0, colors: 0, data,
            })
        }).collect::<Result<_, String>>()?;
    }
    // The CUESHEET tag decides the cue sheet: removing the tag removes it, except in a file
    // that only ever had the binary form (then there is no tag for the host to pass back).
    let untagged_binary = d.reader.metadata.cuesheet_tag().is_none();
    meta.sync_cue_sheet(d.reader.header.sample_rate, d.reader.total_frames, untagged_binary).map_err(|e| e.0)?;
    // CD tags (`fak::cdrip`; opt-in), by the same rule as `fak edit`: in a file that
    // already has them (its FAK_ tags), recomputed when the cue sheet's track points change
    // (foobar2000 rewrites the sheet's text on every track retitle, which moves nothing and
    // so costs no decode), the FAK_ ones dropped with the sheet. Files without them get
    // none (`fak encode --cd-tags` / `fak edit --cd-tags` add them). The host passes every
    // tag, so a CD tag it *changed* in this edit is the user's and wins.
    let old = &d.reader.metadata;
    if cdrip::has_fak_disc_tags(old) && cdrip::cue_points_changed(old.cue_sheet.as_ref(), meta.cue_sheet.as_ref()) {
        let value = |m: &Metadata, k: &str| m.tags.iter().filter(|t| t.split_once('=').is_some_and(|(key, _)| key.eq_ignore_ascii_case(k))).cloned().collect::<Vec<_>>();
        let changed: Vec<String> = cdrip::DISC_TAGS.iter().filter(|k| value(old, k) != value(&meta, k)).map(|k| k.to_string()).collect();
        if meta.cue_sheet.is_some() {
            let h = d.reader.header.clone();
            let exact = h.channels == 2 && h.sample_rate == 44100 && h.bits_per_sample == 16
                && meta.cue_sheet.as_ref().and_then(|c| cdrip::Toc::from_cue(c, h.sample_rate, d.reader.total_frames)).is_some();
            let channels = if exact { decoder::decode_with_threads(&d.reader.read_all().map_err(|e| e.0)?, fak::parallel::default_threads()).map_err(|e| e.0)?.1 } else { Vec::new() };
            cdrip::apply_disc_tags(&mut meta, &channels, h.sample_rate, h.bits_per_sample, &changed);
        } else {
            cdrip::drop_fak_disc_tags(&mut meta, &changed);
        }
    }
    Ok(meta)
}

/// Builds a copy of the open stream with its tags (and, if `replace_pictures` is non-zero, its
/// pictures) replaced; vendor, cue sheet and channel mask are kept. Audio bytes are unchanged.
/// On success writes the new file into `*out` and returns 0; on failure returns -1 (see
/// `last_error`). `tags` are NUL-terminated "KEY=VALUE" strings.
#[no_mangle]
pub extern "C" fn fak_rewrite_metadata(
    d: *mut FakDecoder, tags: *const *const c_char, tag_count: usize,
    replace_pictures: c_int, pictures: *const FakPictureIn, picture_count: usize, out: *mut FakBuffer,
) -> c_int {
    if d.is_null() || out.is_null() || (tags.is_null() && tag_count > 0) || (pictures.is_null() && picture_count > 0) { return -1; }
    guard(-1, || {
        // Safety: valid handle, arrays of the stated lengths, per the header contract.
        let d = unsafe { &mut *d };
        let mut build = || -> Result<Vec<u8>, String> {
            let meta = retagged(d, tags, tag_count, replace_pictures, pictures, picture_count)?;
            decoder::rewrite_metadata(&d.reader.read_all().map_err(|e| e.0)?, &meta).map_err(|e| e.0)
        };
        match build() {
            Ok(v) => {
                let b = v.into_boxed_slice();
                let len = b.len();
                // Safety: `out` is a valid out-pointer per contract.
                unsafe { *out = FakBuffer { data: Box::into_raw(b) as *mut u8, len } };
                0
            }
            Err(m) => { d.last_error = cstring(&m); -1 }
        }
    })
}

/// As `fak_rewrite_metadata`, but the new file is written through `write` (positions ascend from 0) and
/// the audio is copied in pieces: memory does not grow with the file. Returns 0, or -1 (`last_error`).
#[no_mangle]
pub extern "C" fn fak_rewrite_metadata_stream(
    d: *mut FakDecoder, tags: *const *const c_char, tag_count: usize,
    replace_pictures: c_int, pictures: *const FakPictureIn, picture_count: usize,
    write: FakWriteFn, sink: *mut c_void,
) -> c_int {
    if d.is_null() || (tags.is_null() && tag_count > 0) || (pictures.is_null() && picture_count > 0) { return -1; }
    guard(-1, || {
        // Safety: valid handle, arrays of the stated lengths, per the header contract.
        let d = unsafe { &mut *d };
        let r = retagged(d, tags, tag_count, replace_pictures, pictures, picture_count)
            .and_then(|meta| d.reader.rewrite_metadata_to(&meta, &mut CbWriter { write, user: sink, pos: 0, end: 0 }).map_err(|e| e.0));
        match r {
            Ok(()) => 0,
            Err(m) => { d.last_error = cstring(&m); -1 }
        }
    })
}

// ---- encoding ---------------------------------------------------------------------------------

/// Collects a track's PCM (pushed a block at a time as the host decodes its source) and encodes it
/// in one go with the same encoder, levels and metadata handling as `fak encode`.
pub struct FakEncoder {
    /// Set by `fak_encoder_begin_stream`: chunks go to the sink as they fill instead of being kept.
    stream: Option<StreamState>,
    channels: Vec<Vec<i64>>,
    /// Float mode (`fak_encoder_new_float`): the raw float32 bit patterns, mapped to integers at finish.
    float_bits: Option<Vec<Vec<u32>>>,
    bits: u32,
    sample_rate: u32,
    channel_mask: Option<u32>,
    last_error: Vec<u8>,
}

/// Progress callback: `(user, frames_done, frames_total)`; return non-zero to cancel.
pub type FakProgress = Option<extern "C" fn(*mut std::ffi::c_void, u64, u64) -> c_int>;

/// New encoder for `channels` x `bits_per_sample` (8, 16, 24 or 32) at `sample_rate`. `expected_frames`
/// is a capacity hint. Returns null if the parameters are not supported.
#[no_mangle]
pub extern "C" fn fak_encoder_new(channels: u32, bits_per_sample: u32, sample_rate: u32, expected_frames: u64) -> *mut FakEncoder {
    guard(std::ptr::null_mut(), || {
        if channels == 0 || channels > 255 || !matches!(bits_per_sample, 8 | 16 | 24 | 32) || sample_rate == 0 { return std::ptr::null_mut(); }
        let cap = usize::try_from(expected_frames).unwrap_or(0).min(1 << 31);
        let chans = (0..channels).map(|_| Vec::with_capacity(cap)).collect();
        Box::into_raw(Box::new(FakEncoder { stream: None, channels: chans, float_bits: None, bits: bits_per_sample, sample_rate, channel_mask: None, last_error: vec![0] }))
    })
}

/// New encoder for 32-bit float PCM (stored losslessly, (b)); feed it with `fak_encoder_push_float`.
#[no_mangle]
pub extern "C" fn fak_encoder_new_float(channels: u32, sample_rate: u32, expected_frames: u64) -> *mut FakEncoder {
    guard(std::ptr::null_mut(), || {
        if channels == 0 || channels > 255 || sample_rate == 0 { return std::ptr::null_mut(); }
        let cap = usize::try_from(expected_frames).unwrap_or(0).min(1 << 31);
        let raw = (0..channels).map(|_| Vec::with_capacity(cap)).collect();
        Box::into_raw(Box::new(FakEncoder { stream: None, channels: Vec::new(), float_bits: Some(raw), bits: 32, sample_rate, channel_mask: None, last_error: vec![0] }))
    })
}

/// Appends `frames` interleaved float32 sample-frames (any bit pattern, including NaN and infinities,
/// is stored exactly). Only for an encoder made with `fak_encoder_new_float`. Returns 0 or -1.
#[no_mangle]
pub extern "C" fn fak_encoder_push_float(e: *mut FakEncoder, interleaved: *const f32, frames: usize) -> c_int {
    if e.is_null() || (interleaved.is_null() && frames > 0) { return -1; }
    guard(-1, || {
        // Safety: valid handle; `interleaved` holds frames x channels samples per contract.
        let e = unsafe { &mut *e };
        let Some(raw) = e.float_bits.as_mut() else { e.last_error = cstring("not a float encoder"); return -1 };
        let nch = raw.len();
        let Some(total) = frames.checked_mul(nch) else { return -1 };
        let src = if total == 0 { &[][..] } else { unsafe { std::slice::from_raw_parts(interleaved, total) } };
        for (c, ch) in raw.iter_mut().enumerate() {
            ch.extend(src.iter().skip(c).step_by(nch).map(|v| v.to_bits()));
        }
        0
    })
}

#[no_mangle]
pub extern "C" fn fak_encoder_free(e: *mut FakEncoder) {
    if e.is_null() { return; }
    // Safety: `e` came from `fak_encoder_new` and is freed once, per the header contract.
    let _ = guard((), || drop(unsafe { Box::from_raw(e) }));
    release_memory();
}

#[no_mangle]
pub extern "C" fn fak_encoder_last_error(e: *const FakEncoder) -> *const c_char {
    if e.is_null() { return c"null encoder".as_ptr(); }
    // Safety: valid handle per contract.
    unsafe { (*e).last_error.as_ptr() as *const c_char }
}

/// Speaker mask (WAVE_FORMAT_EXTENSIBLE dwChannelMask) to store; 0 (or a mask that does not match
/// the channel count) stores none.
#[no_mangle]
pub extern "C" fn fak_encoder_set_channel_mask(e: *mut FakEncoder, mask: u32) {
    if e.is_null() { return; }
    // Safety: valid handle per contract.
    let e = unsafe { &mut *e };
    let nch = e.float_bits.as_ref().map_or(e.channels.len(), |r| r.len());
    e.channel_mask = (mask != 0 && mask.count_ones() as usize == nch).then_some(mask);
}

/// Appends `frames` interleaved sample-frames at the stream's own scale (a 16-bit stream takes
/// values in -32768..=32767). Returns 0, or -1 if a value is out of range (nothing is stored then).
#[no_mangle]
pub extern "C" fn fak_encoder_push(e: *mut FakEncoder, interleaved: *const i32, frames: usize) -> c_int {
    if e.is_null() || (interleaved.is_null() && frames > 0) { return -1; }
    guard(-1, || {
        // Safety: valid handle; `interleaved` holds frames x channels samples per contract.
        let e = unsafe { &mut *e };
        if e.float_bits.is_some() { e.last_error = cstring("this is a float encoder; use fak_encoder_push_float"); return -1; }
        let nch = e.channels.len();
        let Some(total) = frames.checked_mul(nch) else { return -1 };
        let src = if total == 0 { &[][..] } else { unsafe { std::slice::from_raw_parts(interleaved, total) } };
        let (lo, hi) = (-(1i64 << (e.bits - 1)), (1i64 << (e.bits - 1)) - 1);
        if src.iter().any(|&v| !(lo..=hi).contains(&(v as i64))) {
            e.last_error = cstring("sample value out of range for the stream's bit depth");
            return -1;
        }
        for (c, ch) in e.channels.iter_mut().enumerate() {
            ch.extend(src.iter().skip(c).step_by(nch).map(|&v| v as i64));
        }
        match stream_drain(e, false) {
            Ok(()) => 0,
            Err(m) => { e.last_error = cstring(&m); -1 }
        }
    })
}

/// Tags, pictures, encoder string and settings shared by [`fak_encoder_finish`] and
/// [`fak_encoder_begin_stream`]: the metadata block (minus float and cue information, which need the
/// audio), the effort, the chunk length in frames and the FEC group.
#[allow(clippy::too_many_arguments)]
fn build_meta(
    e: &FakEncoder, level: c_int, chunk_seconds: u32, fec_group: u32,
    tags: *const *const c_char, tag_count: usize, pictures: *const FakPictureIn, picture_count: usize,
) -> Result<(Metadata, Effort, usize, Option<usize>), String> {
    let mut meta = Metadata::default();
    meta.tags = (0..tag_count).map(|i| unsafe { c_str(*tags.add(i)) }.map(str::to_owned)).collect::<Result<_, _>>()?;
    meta.pictures = (0..picture_count).map(|i| {
        let p = unsafe { &*pictures.add(i) };
        let kind_raw = u8::try_from(p.kind).map_err(|_| "picture type out of range".to_string())?;
        let data = if p.data_len == 0 { Vec::new() } else { unsafe { std::slice::from_raw_parts(p.data, p.data_len) }.to_vec() };
        Ok(Picture {
            kind: PictureType::from_u8(kind_raw), kind_raw, mime: unsafe { c_str(p.mime) }?.to_owned(),
            description: unsafe { c_str(p.description) }?.to_owned(), width: 0, height: 0, depth: 0, colors: 0, data,
        })
    }).collect::<Result<_, String>>()?;
    meta.channel_mask = e.channel_mask;
    let effort = match level {
        0 => Effort::Fast,
        1 => Effort::Normal,
        2 => Effort::Max,
        3 => Effort::Insane,
        _ => return Err(format!("unknown level {level}")),
    };
    let fec = match fec_group {
        0 => None,
        u32::MAX => Some(format::FEC_AUTO),
        g if (g as usize) <= format::MAX_FEC_GROUP => Some(g as usize),
        g => return Err(format!("invalid FEC group size {g}")),
    };
    let chunk_frames = if chunk_seconds == 0 {
        format::default_chunk_frames(e.sample_rate)
    } else {
        (e.sample_rate as usize).saturating_mul(chunk_seconds as usize).clamp(1, format::MAX_CHUNK_FRAMES as usize)
    };
    // Like FLAC's vendor string, but with what was asked of the encoder (the level itself is not
    // recoverable from the stream): "fak 1.0.0 (foo_input_fak 1.0.0; level=insane; fec=RS whole-file; chunk=auto)".
    meta.vendor = format!(
        "fak {} (foo_input_fak {}; level={}; fec={}; chunk={})", fak::LIB_VERSION, env!("CARGO_PKG_VERSION"),
        ["fast", "normal", "max", "insane"][level as usize],
        fec.map_or("none".to_string(), |g| if g == format::FEC_AUTO { "RS whole-file".to_string() } else { format!("RS per {g} chunks") }),
        if chunk_seconds == 0 { "auto".to_string() } else { format!("{chunk_seconds}s") },
    );
    Ok((meta, effort, chunk_frames, fec))
}

/// Writes `len` bytes at absolute position `pos` of the output; returns 0 on success, non-zero on
/// failure. The library also rewrites the stream header at position 0 when it finishes. Must not
/// throw or unwind.
pub type FakWriteFn = unsafe extern "C" fn(user: *mut c_void, pos: u64, buf: *const u8, len: usize) -> c_int;

/// The host's output file behind its write callback.
struct CbWriter { write: FakWriteFn, user: *mut c_void, pos: u64, end: u64 }

impl std::io::Write for CbWriter {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        if buf.is_empty() { return Ok(0); }
        // Safety: `buf` holds `buf.len()` readable bytes; the callback contract is on `FakWriteFn`.
        let rc = unsafe { (self.write)(self.user, self.pos, buf.as_ptr(), buf.len()) };
        if rc != 0 { return Err(std::io::Error::other("write callback failed")); }
        self.pos += buf.len() as u64;
        self.end = self.end.max(self.pos);
        Ok(buf.len())
    }
    fn flush(&mut self) -> std::io::Result<()> { Ok(()) }
}

impl Seek for CbWriter {
    fn seek(&mut self, to: SeekFrom) -> std::io::Result<u64> {
        let p = match to {
            SeekFrom::Start(n) => Some(n),
            SeekFrom::End(d) => self.end.checked_add_signed(d),
            SeekFrom::Current(d) => self.pos.checked_add_signed(d),
        };
        self.pos = p.ok_or_else(|| std::io::Error::other("seek before the start"))?;
        Ok(self.pos)
    }
}

struct StreamState {
    enc: Option<encoder::FileEncoder<CbWriter>>,
    chunk_frames: usize,
    frames_done: u64,
    expected: u64,
    progress: FakProgress,
    user: *mut c_void,
}

/// Hands every full chunk (or, with `all`, whatever is left) of a streaming encoder to the sink.
fn stream_drain(e: &mut FakEncoder, all: bool) -> Result<(), String> {
    let Some(st) = e.stream.as_mut() else { return Ok(()) };
    loop {
        let have = e.channels.first().map_or(0, |c| c.len());
        let take = if have >= st.chunk_frames { st.chunk_frames } else if all && have > 0 { have } else { break };
        let chunk: Vec<Vec<i64>> = e.channels.iter_mut().map(|c| c.drain(..take).collect()).collect();
        let enc = st.enc.as_mut().ok_or("the stream is already finished")?;
        enc.push_chunk(chunk).map_err(|x| x.0)?;
        st.frames_done += take as u64;
        if let Some(f) = st.progress {
            if f(st.user, st.frames_done, st.expected.max(st.frames_done)) != 0 { return Err("cancelled".into()); }
        }
    }
    Ok(())
}

/// Makes an encoder from `fak_encoder_new` write straight to a file as audio is pushed, so memory
/// stays at a few chunks instead of the whole track: metadata (`tags`, `pictures`) goes out first,
/// each chunk as it fills, and `fak_encoder_end_stream` writes the real header. Settings are as for
/// `fak_encoder_finish`; call before the first push. `expected_frames` is only for progress reports.
/// Not for float encoders or CD tags (both need all the audio first). Returns 0, or -1 (see
/// `fak_encoder_last_error`); `progress` non-zero cancels ("cancelled").
#[no_mangle]
pub extern "C" fn fak_encoder_begin_stream(
    e: *mut FakEncoder, write: FakWriteFn, sink: *mut c_void, level: c_int, threads: u32, chunk_seconds: u32, fec_group: u32,
    tags: *const *const c_char, tag_count: usize, pictures: *const FakPictureIn, picture_count: usize,
    expected_frames: u64, progress: FakProgress, progress_user: *mut c_void,
) -> c_int {
    if e.is_null() || (tags.is_null() && tag_count > 0) || (pictures.is_null() && picture_count > 0) { return -1; }
    guard(-1, || {
        // Safety: valid handle, arrays of the stated lengths, per the header contract.
        let e = unsafe { &mut *e };
        let mut go = || -> Result<(), String> {
            if e.stream.is_some() { return Err("streaming already begun".into()); }
            if e.float_bits.is_some() { return Err("a float encoder cannot stream (it needs every sample first); use fak_encoder_finish".into()); }
            if e.channels.iter().any(|c| !c.is_empty()) { return Err("begin the stream before pushing audio".into()); }
            let (meta, effort, chunk_frames, fec) = build_meta(e, level, chunk_seconds, fec_group, tags, tag_count, pictures, picture_count)?;
            let threads = if threads == 0 { fak::parallel::default_threads() } else { threads as usize };
            let sink = CbWriter { write, user: sink, pos: 0, end: 0 };
            let enc = encoder::FileEncoder::new(
                sink, e.channels.len(), e.sample_rate, e.bits as u8, format::MODE_BLOCK_INDEPENDENT, threads, fec, &meta, effort,
            ).map_err(|x| x.0)?;
            // `fak_encoder_new` reserved room for the whole track; a stream only ever holds a chunk or two.
            for c in e.channels.iter_mut() { *c = Vec::with_capacity(chunk_frames * 2); }
            e.stream = Some(StreamState { enc: Some(enc), chunk_frames, frames_done: 0, expected: expected_frames, progress, user: progress_user });
            Ok(())
        };
        match go() {
            Ok(()) => 0,
            Err(m) => { e.last_error = cstring(&m); -1 }
        }
    })
}

/// Encodes the last partial chunk and writes the final header; the output file is complete after
/// this. Returns 0, or -1 (see `fak_encoder_last_error`).
#[no_mangle]
pub extern "C" fn fak_encoder_end_stream(e: *mut FakEncoder) -> c_int {
    if e.is_null() { return -1; }
    guard(-1, || {
        // Safety: valid handle per contract.
        let e = unsafe { &mut *e };
        let mut go = || -> Result<(), String> {
            if e.stream.is_none() { return Err("no stream was begun".into()); }
            stream_drain(e, true)?;
            let st = e.stream.as_mut().unwrap();
            let enc = st.enc.take().ok_or("the stream is already finished")?;
            if enc.frames() == 0 { return Err("no audio was pushed".into()); }
            enc.finish().map_err(|x| x.0)?;
            Ok(())
        };
        match go() {
            Ok(()) => 0,
            Err(m) => { e.last_error = cstring(&m); -1 }
        }
    })
}

/// Level index: 0 fast, 1 normal, 2 max, 3 insane, as `fak encode --LEVEL` (`archival` is level 3 with
/// `fec_group` `u32::MAX`). `fec_group` 0 = no error-recovery data, `u32::MAX` = Reed-Solomon parity over
/// the whole file (about 1% of its chunks can be rebuilt), else 1..=60000: one Reed-Solomon parity block
/// per that many data chunks. `chunk_seconds` 0 = automatic. `threads` 0 = automatic. `cd_tags` non-zero adds
/// the CD identifiers and AccurateRip/CTDB checksums (`fak encode --cd-tags`) when a `CUESHEET` tag is
/// given and the audio is an exact CD image; otherwise none are written. The encoder string stored in the
/// file names both libraries' versions and the settings used (level, FEC, chunk length).
/// Encodes everything pushed so far into `*out` (release with `fak_buffer_free`).
/// Returns 0, or -1 (see `fak_encoder_last_error`; "cancelled" when `progress` returned non-zero).
#[no_mangle]
pub extern "C" fn fak_encoder_finish(
    e: *mut FakEncoder, level: c_int, threads: u32, chunk_seconds: u32, fec_group: u32, cd_tags: c_int,
    tags: *const *const c_char, tag_count: usize,
    pictures: *const FakPictureIn, picture_count: usize, progress: FakProgress, user: *mut std::ffi::c_void, out: *mut FakBuffer,
) -> c_int {
    if e.is_null() || out.is_null() || (tags.is_null() && tag_count > 0) || (pictures.is_null() && picture_count > 0) { return -1; }
    guard(-1, || {
        // Safety: valid handle, arrays of the stated lengths, per the header contract.
        let e = unsafe { &mut *e };
        let mut build = || -> Result<Vec<u8>, String> {
            let (mut meta, effort, chunk_frames, fec) =
                build_meta(e, level, chunk_seconds, fec_group, tags, tag_count, pictures, picture_count)?;
            if let Some(raw) = e.float_bits.take() {
                // Same reduction as `fak encode` of a float32 WAV: integer PCM plus the side information
                // that inverts it exactly (values that do not fit the grid are stored verbatim).
                let (mapped, bits, info) = fak::floatpcm::map_to_pcm(&raw);
                e.channels = mapped;
                e.bits = bits as u32;
                meta.float_info = Some(info);
            }
            let frames = e.channels.first().map_or(0, |c| c.len());
            meta.sync_cue_sheet(e.sample_rate, frames as u64, false).map_err(|x| x.0)?;
            if cd_tags != 0 {
                // No-op unless there is a cue sheet and the audio is an exact 44.1 kHz 16-bit stereo CD image.
                cdrip::apply_disc_tags(&mut meta, &e.channels, e.sample_rate, e.bits as u8, &[]);
            }
            let threads = if threads == 0 { fak::parallel::default_threads() } else { threads as usize };
            let mut buf = Vec::new();
            let mut cb = |done: u64, total: u64| progress.is_none_or(|f| f(user, done, total) == 0);
            encoder::encode_chunked_to_progress(
                &mut buf, &e.channels, e.sample_rate, e.bits as u8, format::MODE_BLOCK_INDEPENDENT,
                chunk_frames, threads, fec, &meta, effort, &mut cb,
            ).map_err(|x| x.0)?;
            Ok(buf)
        };
        match build() {
            Ok(v) => {
                let b = v.into_boxed_slice();
                let len = b.len();
                // Safety: `out` is a valid out-pointer per contract.
                unsafe { *out = FakBuffer { data: Box::into_raw(b) as *mut u8, len } };
                0
            }
            Err(m) => { e.last_error = cstring(&m); -1 }
        }
    })
}

/// Decodes the whole stream and checks it against the SHA-256 of the source PCM stored in its
/// header (`fak verify`). 0 on success; -1 with a message in `err` otherwise. `threads` 0 = automatic.
#[no_mangle]
pub extern "C" fn fak_verify(data: *const u8, len: usize, threads: u32, err: *mut c_char, err_cap: usize) -> c_int {
    if data.is_null() && len != 0 { write_err(err, err_cap, "null data"); return -1; }
    guard(-1, || {
        // Safety: `data` points to `len` readable bytes per contract.
        let bytes = if len == 0 { &[][..] } else { unsafe { std::slice::from_raw_parts(data, len) } };
        let threads = if threads == 0 { fak::parallel::default_threads() } else { threads as usize };
        match decoder::verify(bytes, threads) {
            Ok(_) => 0,
            Err(e) => { write_err(err, err_cap, &e.0); -1 }
        }
    })
}

/// [`fak_verify`] for a file that stays with the host (see [`fak_decoder_open_cb`]): chunks are read,
/// decoded and hashed in batches, so memory is a few chunks whatever the file size.
#[no_mangle]
pub extern "C" fn fak_verify_cb(read: FakReadFn, user: *mut c_void, size: u64, threads: u32, err: *mut c_char, err_cap: usize) -> c_int {
    guard(-1, || {
        let threads = if threads == 0 { fak::parallel::default_threads() } else { threads as usize };
        let mut r = match FileReader::open(Box::new(CbFile { read, user, size, pos: 0 }) as Box<dyn ReadSeek>) {
            Ok(r) => r,
            Err(e) => { write_err(err, err_cap, &e.0); return -1; }
        };
        match decoder::verify_file(&mut r, threads) {
            Ok(()) => 0,
            Err(e) => { write_err(err, err_cap, &e.0); -1 }
        }
    })
}
