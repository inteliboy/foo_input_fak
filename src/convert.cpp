#include "fak_common.h"
#include <cmath>
#include <functional>

// The "FAK" context menu: Convert to FAK (at the level chosen in Preferences), Convert with level >
// Fast .. Archival, Convert to one FAK file with an embedded cue sheet, Verify FAK files, Preferences.
// Conversion decodes the selected tracks through foobar2000's own input components, hands the PCM to
// the FAK library (fak_capi.h) and writes the file with the source's tags and album art. Runs in a
// threaded_process, so the usual progress dialog with Abort applies. Only documented SDK services are
// used.

namespace {

using namespace fak_settings;

const GUID guid_group_fak = { 0x9fa0d141, 0x011b, 0x4053, { 0x96, 0x5e, 0x55, 0x1e, 0x16, 0xa6, 0xd5, 0x86 } };
const GUID guid_group_level = { 0xf773ec79, 0x24ae, 0x42e3, { 0x8f, 0x71, 0x19, 0x87, 0xa6, 0x76, 0xc3, 0x33 } };
// Item GUIDs: a base GUID plus the item's index in Data1.
const GUID guid_item_base = { 0x4a237a68, 0x19bb, 0x442d, { 0x99, 0x24, 0xf1, 0x46, 0x1a, 0x20, 0x19, 0xed } };
GUID item_guid(uint32_t n) { GUID g = guid_item_base; g.Data1 += n; return g; }

contextmenu_group_popup_factory g_group_fak(guid_group_fak, contextmenu_groups::root, "FAK", 0);
contextmenu_group_popup_factory g_group_level(guid_group_level, guid_group_fak, "Convert with level", 1);

bool is_fak_path(const char* path) {
	const char* dot = strrchr(path, '.');
	return dot != nullptr && stricmp_utf8(dot + 1, "fak") == 0;
}

// ---- destination paths ------------------------------------------------------------------------

// One path component made safe for Windows: forbidden characters replaced, no trailing dot/space,
// bounded length (cut on a UTF-8 boundary).
std::string sanitize_component(std::string s) {
	for (char& c : s) if (static_cast<unsigned char>(c) < 0x20 || strchr("<>:\"/\\|?*", c) != nullptr) c = '_';
	if (s.size() > 200) {
		size_t n = 200;
		while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
		s.resize(n);
	}
	while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
	return s;
}

// A formatted pattern ("dir\name") as a relative path of safe components.
std::string relative_path_from(const char* formatted) {
	std::string out, part;
	auto flush = [&] {
		std::string c = sanitize_component(part);
		part.clear();
		if (c.empty() || c == "." || c == "..") return;
		if (!out.empty()) out += '\\';
		out += c;
	};
	for (const char* p = formatted; *p; ++p) {
		if (*p == '\\' || *p == '/') flush(); else part += *p;
	}
	flush();
	return out;
}

// Native path (no file://) of a local source, or empty.
// The canonical step resolves the "file-relative://" paths a portable foobar2000 stores (relative to
// its own folder) into absolute ones.
std::string native_source(const char* path) {
	pfc::string8 canonical, native;
	filesystem::g_get_canonical_path(path, canonical);
	if (!filesystem::g_get_native_path(canonical, native)) return {};
	return std::string(native.c_str());
}

std::string folder_of(std::string const& native_file) {
	const size_t slash = native_file.find_last_of("\\/");
	return slash == std::string::npos ? std::string() : native_file.substr(0, slash + 1);
}

struct job {
	metadb_handle_ptr handle;
	std::string dest_native;  // destination without extension, native path
	std::string note;         // planning problem (the job is then reported as failed)
};

// Builds the destination (before the collision rule) of every selected item. Main thread: uses
// title formatting.
std::vector<job> plan(metadb_handle_list_cref items, values const& v) {
	std::vector<job> jobs;
	titleformat_object::ptr script;
	static_api_ptr_t<titleformat_compiler>()->compile_safe_ex(script, v.pattern.c_str(), "%title%");
	titleformat_object::ptr track_script;
	static_api_ptr_t<titleformat_compiler>()->compile_safe_ex(track_script, "%tracknumber%. %title%", "%title%");
	for (size_t i = 0; i < items.get_count(); ++i) {
		job j;
		j.handle = items[i];
		const char* path = items[i]->get_path();
		if (v.dest == dest_custom_folder) {
			if (v.folder.is_empty()) { j.note = "no output folder is set (Preferences > Tools > FAK)"; jobs.push_back(std::move(j)); continue; }
			pfc::string8 text;
			items[i]->format_title(nullptr, text, script, nullptr);
			std::string rel = relative_path_from(text.c_str());
			if (rel.empty()) { j.note = "the file name pattern produced an empty name"; jobs.push_back(std::move(j)); continue; }
			std::string folder = v.folder.c_str();
			while (!folder.empty() && (folder.back() == '\\' || folder.back() == '/')) folder.pop_back();
			j.dest_native = folder + "\\" + rel;
		} else {
			std::string src = native_source(path);
			if (src.empty()) { j.note = "not a local file; choose an output folder in Preferences > Tools > FAK"; jobs.push_back(std::move(j)); continue; }
			const size_t slash = src.find_last_of("\\/");
			const size_t dot = src.find_last_of('.');
			if (items[i]->get_subsong_index() != 0) {
				// A track of a cue-sheet image: the file name is shared, so name it after the track.
				pfc::string8 text;
				items[i]->format_title(nullptr, text, track_script, nullptr);
				std::string name = sanitize_component(text.c_str());
				if (name.empty()) name = "track";
				j.dest_native = src.substr(0, slash == std::string::npos ? 0 : slash + 1) + name;
			} else {
				j.dest_native = (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? src.substr(0, dot) : src;
			}
		}
		jobs.push_back(std::move(j));
	}
	return jobs;
}

// The destination (without extension) of a single-file conversion of `items`, named by the album
// pattern after the first track: in the first track's folder, or in the custom folder. `note` is set
// when that cannot be worked out.
void plan_image(metadb_handle_list_cref items, values const& v, std::string& base, std::string& note) {
	titleformat_object::ptr script;
	static_api_ptr_t<titleformat_compiler>()->compile_safe_ex(script, v.image_pattern.c_str(), "%album%");
	pfc::string8 text;
	items[0]->format_title(nullptr, text, script, nullptr);
	std::string rel = relative_path_from(text.c_str());
	if (rel.empty()) rel = "album";
	if (v.dest == dest_custom_folder) {
		if (v.folder.is_empty()) { note = "no output folder is set (Preferences > Tools > FAK)"; return; }
		std::string folder = v.folder.c_str();
		while (!folder.empty() && (folder.back() == '\\' || folder.back() == '/')) folder.pop_back();
		base = folder + "\\" + rel;
	} else {
		const std::string src = native_source(items[0]->get_path());
		if (src.empty()) { note = "the first track is not a local file; choose an output folder in Preferences > Tools > FAK"; return; }
		base = folder_of(src) + rel;
	}
}

pfc::string8 url_of(std::string const& native) { pfc::string8 u = "file://"; u += native.c_str(); return u; }

// Creates every folder of `native_file`'s path that does not exist yet (errors surface when the
// file itself is opened).
void make_folders(std::string const& native_file, abort_callback& abort) {
	for (size_t pos = native_file.find('\\', 3); pos != std::string::npos; pos = native_file.find('\\', pos + 1)) {
		try { filesystem::g_create_directory(url_of(native_file.substr(0, pos)).c_str(), abort); }
		catch (exception_io_already_exists const&) {}
		catch (exception_aborted const&) { throw; }
		catch (exception_io const&) {}
	}
}

// The final ".fak" path under the collision rule; empty when the file is to be skipped.
std::string resolve_collision(std::string const& base, int rule, abort_callback& abort) {
	std::string first = base + ".fak";
	if (rule == exist_overwrite || !filesystem::g_exists(url_of(first).c_str(), abort)) return first;
	if (rule == exist_skip) return {};
	for (int n = 2; n < 10000; ++n) {
		std::string cand = base + " (" + std::to_string(n) + ").fak";
		if (!filesystem::g_exists(url_of(cand).c_str(), abort)) return cand;
	}
	return {};
}

// ---- decoding sources into an encoder ---------------------------------------------------------

struct enc_deleter { void operator()(FakEncoder* e) const { fak_encoder_free(e); } };
using encoder_ptr = std::unique_ptr<FakEncoder, enc_deleter>;
struct buf_guard { FakBuffer b = {}; ~buf_guard() { fak_buffer_free(b); } };

// True when foobar2000 describes the source as floating point (its input components label such
// sources "PCM (floating-point)"; Matroska has the codec id A_PCM/FLOAT/IEEE). Any field mentioning
// "float" counts, so the label need not sit in one particular field.
bool source_is_float(file_info const& info) {
	auto has_float = [](const char* v) {
		if (v == nullptr) return false;
		for (const char* p = v; *p; ++p) if (_strnicmp(p, "float", 5) == 0) return true;
		return false;
	};
	for (size_t i = 0, n = info.info_get_count(); i < n; ++i) if (has_float(info.info_enum_value(i))) return true;
	return false;
}

int clamp_i32(double x, double lo, double hi) { return static_cast<int>(x < lo ? lo : x > hi ? hi : x); }

struct source {
	input_decoder::ptr dec;
	file_info_impl info;
	bool is_float = false;
	int bits = 16;
	double length = 0;
};

// Opens the track for decoding and works out how it is to be stored.
void open_source(metadb_handle_ptr const& h, abort_callback& abort, source& s) {
	const char* path = h->get_path();
	const t_uint32 subsong = h->get_subsong_index();
	input_entry::g_open_for_decoding(s.dec, nullptr, path, abort);
	s.dec->get_info(subsong, s.info, abort);
	if (const char* enc = s.info.info_get("encoding"); enc != nullptr && stricmp_utf8(enc, "lossy") == 0)
		throw std::runtime_error("the source is lossy; converting it to a lossless format would only make it larger");
	// Float sources are stored losslessly as float32. Integer sources are stored at 8, 16, 24 or
	// 32 bits: foobar2000's audio_sample is a double on x64, which holds any of them exactly.
	s.is_float = source_is_float(s.info);
	s.bits = static_cast<int>(s.info.info_get_int("bitspersample"));
	if (s.bits <= 0) s.bits = 16;
	if (!s.is_float) {
		if (s.bits > 32) throw std::runtime_error(pfc::format("the source is ", s.bits, "-bit; FAK stores 8, 16, 24 or 32-bit integer audio and 32-bit float").c_str());
		s.bits = s.bits <= 8 ? 8 : s.bits <= 16 ? 16 : s.bits <= 24 ? 24 : 32;
	}
	s.length = s.info.get_length();
	s.dec->initialize(subsong, input_flag_simpledecode, abort);
}

// Feeds decoded audio to one FAK encoder. The encoder is made from the first chunk; every later chunk
// (of the same or of a following track) must have the same format.
// Where a streaming feeder writes and how (see fak_encoder_begin_stream). The strings and pictures
// are owned by the caller and must outlive the feeder.
struct stream_config {
	fak_common::file_sink* sink = nullptr;
	int effort = 0;
	uint32_t threads = 0, chunk_secs = 0, fec_group = 0;
	const char* const* tags = nullptr;
	size_t ntags = 0;
	const FakPictureIn* pics = nullptr;
	size_t npics = 0;
	uint64_t expected_frames = 0;
};

int stream_cancel_check(void* user, uint64_t, uint64_t) {
	return static_cast<abort_callback*>(user)->is_aborting() ? 1 : 0;
}

class pcm_feeder {
public:
	// Write to a file as audio arrives instead of keeping it (integer sources only; not with CD tags).
	void stream_to(stream_config const& cfg, abort_callback& abort) { m_cfg = cfg; m_streaming = true; m_abort = &abort; }
	// Encodes the last partial chunk and completes the file.
	void end_stream(abort_callback& abort) {
		if (fak_encoder_end_stream(m_enc.get()) != 0) {
			abort.check();
			throw std::runtime_error(fak_encoder_last_error(m_enc.get()));
		}
	}
	uint64_t frames() const { return m_frames; }
	bool started() const { return m_enc != nullptr; }
	unsigned rate() const { return m_rate; }
	// The exact CD-DA layout: 44.1 kHz, 16-bit integer, stereo.
	bool cd_format() const { return m_rate == 44100 && m_nch == 2 && !m_is_float && m_bits == 16; }
	FakEncoder* get() { return m_enc.get(); }

	// Decodes all of `s`; `progress(frames_of_this_source_so_far)` follows every chunk. Returns the frames added.
	uint64_t add(source& s, abort_callback& abort, std::function<void(uint64_t)> const& progress) {
		uint64_t added = 0;
		audio_chunk_fast_impl chunk;
		while (s.dec->run(chunk, abort)) {
			abort.check();
			const size_t frames = chunk.get_sample_count();
			if (frames == 0) continue;
			if (!m_enc) start(s, chunk); else check(s, chunk);
			push(chunk);
			added += frames;
			m_frames += frames;
			progress(added);
		}
		return added;
	}

private:
	void start(source const& s, audio_chunk const& chunk) {
		m_rate = chunk.get_sample_rate();
		m_nch = chunk.get_channel_count();
		m_is_float = s.is_float;
		m_bits = s.bits;
		const uint64_t expected = static_cast<uint64_t>(s.length * m_rate) + 1;
		m_enc.reset(m_is_float ? fak_encoder_new_float(m_nch, m_rate, expected) : fak_encoder_new(m_nch, m_bits, m_rate, expected));
		if (!m_enc) throw std::runtime_error(pfc::format("unsupported format: ", m_nch, " channels, ", m_bits, " bits, ", m_rate, " Hz").c_str());
		const unsigned cfg = chunk.get_channel_config();
		if (cfg != audio_chunk::g_guess_channel_config(m_nch)) fak_encoder_set_channel_mask(m_enc.get(), audio_chunk::g_channel_config_to_wfx(cfg));
		m_scale = static_cast<double>(int64_t(1) << (m_bits - 1));
		if (m_streaming) {
			if (m_is_float) throw std::runtime_error("internal error: a float source cannot stream");
			if (fak_encoder_begin_stream(m_enc.get(), &fak_common::write_cb, m_cfg.sink, m_cfg.effort, m_cfg.threads, m_cfg.chunk_secs, m_cfg.fec_group,
			                             m_cfg.tags, m_cfg.ntags, m_cfg.pics, m_cfg.npics, m_cfg.expected_frames, &stream_cancel_check, m_abort) != 0)
				throw std::runtime_error(fak_encoder_last_error(m_enc.get()));
		}
	}

	void check(source const& s, audio_chunk const& chunk) const {
		if (chunk.get_sample_rate() != m_rate || chunk.get_channel_count() != m_nch)
			throw std::runtime_error("the audio changes sample rate or channel count part-way through");
		if (s.is_float != m_is_float || s.bits != m_bits)
			throw std::runtime_error("a track's sample format differs from the first track's, so they cannot go in one file");
	}

	void push(audio_chunk const& chunk) {
		const size_t frames = chunk.get_sample_count();
		const size_t n = frames * m_nch;
		const audio_sample* s = chunk.get_data();
		if (m_is_float) {
			m_fpcm.resize(n);
			for (size_t k = 0; k < n; ++k) m_fpcm[k] = static_cast<float>(s[k]);
			if (fak_encoder_push_float(m_enc.get(), m_fpcm.data(), frames) != 0) throw std::runtime_error(fak_encoder_last_error(m_enc.get()));
		} else {
			m_pcm.resize(n);
			for (size_t k = 0; k < n; ++k) {
				const double x = static_cast<double>(s[k]) * m_scale, r = std::nearbyint(x);
				// At 32 bits a source that is really float (unlabelled) would be silently rounded; refuse.
				if (m_bits == 32 && x != r) throw std::runtime_error("the source's 32-bit samples are not integers (is it a float source?); not converted");
				m_pcm[k] = clamp_i32(r, -m_scale, m_scale - 1);
			}
			if (fak_encoder_push(m_enc.get(), m_pcm.data(), frames) != 0) {
				if (m_abort != nullptr) m_abort->check();
				throw std::runtime_error(fak_encoder_last_error(m_enc.get()));
			}
		}
	}

	encoder_ptr m_enc;
	stream_config m_cfg;
	bool m_streaming = false;
	abort_callback* m_abort = nullptr;
	std::vector<int32_t> m_pcm;
	std::vector<float> m_fpcm;
	unsigned m_rate = 0, m_nch = 0;
	int m_bits = 0;
	bool m_is_float = false;
	double m_scale = 1;
	uint64_t m_frames = 0;
};

// "NAME=value" for every value of every field, plus ReplayGain (foobar2000 keeps that apart from the
// fields). `keep_cuesheet`: whether a "cuesheet" field is passed on (a track cut from an image must not
// carry the image's sheet; a file that is to embed one must).
std::vector<std::string> tags_of(file_info const& info, bool keep_cuesheet) {
	std::vector<std::string> tags;
	for (size_t i = 0, n = info.meta_get_count(); i < n; ++i) {
		const char* name = info.meta_enum_name(i);
		if (name == nullptr || *name == 0) continue;
		if (!keep_cuesheet && stricmp_utf8(name, "cuesheet") == 0) continue;
		for (size_t k = 0, m = info.meta_enum_value_count(i); k < m; ++k) tags.push_back(std::string(name) + "=" + info.meta_enum_value(i, k));
	}
	const replaygain_info rg = info.get_replaygain();
	replaygain_info::t_text_buffer t;
	if (rg.format_track_gain(t)) tags.push_back(std::string("REPLAYGAIN_TRACK_GAIN=") + t);
	if (rg.format_track_peak(t)) tags.push_back(std::string("REPLAYGAIN_TRACK_PEAK=") + t);
	if (rg.format_album_gain(t)) tags.push_back(std::string("REPLAYGAIN_ALBUM_GAIN=") + t);
	if (rg.format_album_peak(t)) tags.push_back(std::string("REPLAYGAIN_ALBUM_PEAK=") + t);
	return tags;
}

// The album art of `h` (front, back, disc, artist, icon) as FAK pictures. No art is not an error.
struct pictures {
	std::vector<album_art_data_ptr> data;
	std::vector<uint32_t> kinds;
	std::vector<std::string> mimes;
	std::vector<FakPictureIn> list;

	void collect(metadb_handle_ptr const& h, abort_callback& abort) {
		try {
			pfc::list_t<GUID> ids;
			size_t n;
			const fak_common::art_kind* k = fak_common::art_kinds(n);
			for (size_t i = 0; i < n; ++i) ids.add_item(k[i].id);
			metadb_handle_list one;
			one.add_item(h);
			auto inst = album_art_manager_v2::get()->open(one, ids, abort);
			for (size_t i = 0; i < n; ++i) {
				try {
					album_art_data_ptr d = inst->query(k[i].id, abort);
					if (d.is_valid() && d->size() > 0) { data.push_back(d); kinds.push_back(k[i].kind); }
				} catch (exception_aborted const&) { throw; }
				catch (std::exception const&) {}
			}
		} catch (exception_aborted const&) { throw; }
		catch (std::exception const&) {}
		mimes.reserve(data.size());
		for (size_t i = 0; i < data.size(); ++i) {
			const uint8_t* p = static_cast<const uint8_t*>(data[i]->data());
			mimes.emplace_back(fak_common::sniff_mime(p, data[i]->size()));
			list.push_back({ kinds[i], mimes.back().c_str(), "", p, data[i]->size() });
		}
	}
};

// ---- progress ---------------------------------------------------------------------------------

struct progress_ctx {
	threaded_process_status* status;
	abort_callback* abort;
	double from, to;  // this phase's share of the current item, 0..1
	size_t index, count;
};

void report_progress(progress_ctx const& c, double phase) {
	const double item = c.from + (c.to - c.from) * phase;
	c.status->set_progress_float((static_cast<double>(c.index) + item) / static_cast<double>(c.count));
}

int encode_progress(void* user, uint64_t done, uint64_t total) {
	auto* c = static_cast<progress_ctx*>(user);
	report_progress(*c, total ? static_cast<double>(done) / static_cast<double>(total) : 1.0);
	return c->abort->is_aborting() ? 1 : 0;
}

// Encodes what `feeder` holds and returns the file bytes in `out`.
void encode_all(pcm_feeder& feeder, int level, bool fec, bool cd_tags, values const& v, std::vector<std::string> const& tags,
                pictures const& pics, progress_ctx& ctx, abort_callback& abort, buf_guard& out) {
	std::vector<const char*> tag_ptrs;
	for (auto const& s : tags) tag_ptrs.push_back(s.c_str());
	const int effort = level < archival_level ? level : 3;  // Archival = Insane effort
	const uint32_t fec_group = fak_settings::abi_fec_group(fec, v.fec_group);
	if (fak_encoder_finish(feeder.get(), effort, static_cast<uint32_t>(v.threads), static_cast<uint32_t>(v.chunk_secs), fec_group, cd_tags ? 1 : 0,
	                       tag_ptrs.data(), tag_ptrs.size(), pics.list.data(), pics.list.size(), &encode_progress, &ctx, &out.b) != 0) {
		abort.check();
		throw std::runtime_error(fak_encoder_last_error(feeder.get()));
	}
}

// Reads the file at `url` back from disk and checks it against the SHA-256 of the source audio stored
// in its header (that checks the bytes that were written). `expected_size` 0 = do not check the size.
void verify_written(pfc::string8 const& url, t_filesize expected_size, abort_callback& abort) {
	fak_common::file_source src;
	filesystem::g_open_read(src.f, url, abort);
	src.abort = &abort;
	const t_filesize size = src.f->get_size_ex(abort);
	if (expected_size != 0 && size != expected_size) throw std::runtime_error("the written file has the wrong size");
	char err[512] = {};
	if (fak_verify_cb(&fak_common::read_cb, &src, size, 0, err, sizeof err) != 0)
		throw std::runtime_error(std::string("verification failed: ") + err);
}

// Writes `buf` to `dest` and, if asked, reads the file back from disk and verifies it (that checks the
// bytes that were written, not the buffer). A file that failed is removed. `progress(0..1)` follows the
// writing and the verification.
void write_and_verify(std::string const& dest, buf_guard& out, bool verify, abort_callback& abort, std::function<void(double)> const& progress) {
	const FakBuffer buf = out.b;
	const pfc::string8 url = url_of(dest);
	bool created = false;
	try {
		{
			file::ptr f;
			filesystem::g_open_write_new(f, url, abort);
			created = true;
			f->write_object(buf.data, buf.len, abort);
		}
		progress(0.5);
		const t_filesize written = buf.len;
		fak_buffer_free(out.b);  // the encoded bytes are on disk: release them before the read-back
		out.b = {};
		if (verify) verify_written(url, written, abort);
		progress(1.0);
	} catch (...) {
		if (created) { try { filesystem::g_remove(url, fb2k::noAbort); } catch (...) {} }
		throw;
	}
}

// ---- conversion worker: one file per track ----------------------------------------------------

class convert_task : public threaded_process_callback {
public:
	convert_task(std::vector<job> jobs, values v, int level, bool fec) : m_jobs(std::move(jobs)), m_v(std::move(v)), m_level(level), m_fec(fec) {}

	void run(threaded_process_status& status, abort_callback& abort) override {
		for (size_t i = 0; i < m_jobs.size(); ++i) {
			job const& j = m_jobs[i];
			const char* path = j.handle->get_path();
			status.set_item_path(path);
			report_progress({ &status, &abort, 0, 0, i, m_jobs.size() }, 0);
			try {
				abort.check();
				if (!j.note.empty()) throw std::runtime_error(j.note);
				convert_one(j, status, abort, i);
			} catch (exception_aborted const&) {
				m_aborted = true;
				return;
			} catch (std::exception const& e) {
				++m_failed;
				m_report << "Failed: " << path << "\n    " << e.what() << "\n";
				FB2K_console_formatter() << "FAK: failed: " << path << ": " << e.what();
			}
		}
	}

	void on_done(ctx_t, bool was_aborted) override {
		if (was_aborted) m_aborted = true;
		pfc::string8 summary;
		summary << "FAK: " << m_ok << " converted";
		if (m_skipped) summary << ", " << m_skipped << " skipped";
		if (m_failed) summary << ", " << m_failed << " failed";
		if (m_aborted) summary << " (aborted)";
		FB2K_console_formatter() << summary;
		if (m_failed || m_skipped) {
			pfc::string8 msg = summary;
			msg << "\n\n" << m_report;
			popup_message::g_show(msg, "FAK conversion");
		}
	}

private:
	void convert_one(job const& j, threaded_process_status& status, abort_callback& abort, size_t index) {
		const char* path = j.handle->get_path();
		if (is_fak_path(path)) { ++m_skipped; m_report << "Skipped: " << path << "\n    already a FAK file\n"; return; }

		std::string base = j.dest_native;
		make_folders(base, abort);
		const std::string dest = resolve_collision(base, m_v.exist, abort);
		if (dest.empty()) { ++m_skipped; m_report << "Skipped: " << path << "\n    the destination file exists\n"; return; }
		if (native_source(path) == dest) { ++m_skipped; m_report << "Skipped: " << path << "\n    the destination is the source file\n"; return; }

		source src;
		open_source(j.handle, abort, src);
		if (!src.is_float) { stream_one(j, src, dest, status, abort, index); return; }
		pcm_feeder feeder;
		progress_ctx dctx = { &status, &abort, 0.0, 0.2, index, m_jobs.size() };
		feeder.add(src, abort, [&](uint64_t done) {
			if (src.length > 0) report_progress(dctx, (std::min)(1.0, static_cast<double>(done) / (src.length * feeder.rate())));
		});
		if (!feeder.started()) throw std::runtime_error("the source contains no audio");

		// An embedded cue sheet belongs to the whole image, not to a track cut from it.
		const std::vector<std::string> tags = tags_of(src.info, false);
		pictures pics;
		if (m_v.album_art) pics.collect(j.handle, abort);

		progress_ctx ectx = { &status, &abort, 0.2, 0.9, index, m_jobs.size() };
		buf_guard out;
		encode_all(feeder, m_level, m_fec, false, m_v, tags, pics, ectx, abort, out);
		feeder = pcm_feeder();  // release the PCM

		write_and_verify(dest, out, m_v.verify, abort, [&](double p) {
			report_progress({ &status, &abort, 0.9, 1.0, index, m_jobs.size() }, p);
		});
		++m_ok;
	}

	// Integer sources go to the file as they are decoded, so memory is a few chunks whatever the
	// length (float sources, which need every sample first, take the buffered path in convert_one).
	void stream_one(job const& j, source& src, std::string const& dest, threaded_process_status& status, abort_callback& abort, size_t index) {
		// An embedded cue sheet belongs to the whole image, not to a track cut from it.
		const std::vector<std::string> tags = tags_of(src.info, false);
		std::vector<const char*> tag_ptrs;
		for (auto const& t : tags) tag_ptrs.push_back(t.c_str());
		pictures pics;
		if (m_v.album_art) pics.collect(j.handle, abort);

		fak_common::file_sink sink;
		sink.abort = &abort;
		stream_config cfg;
		cfg.sink = &sink;
		cfg.effort = m_level < archival_level ? m_level : 3;  // Archival = Insane effort
		cfg.threads = static_cast<uint32_t>(m_v.threads);
		cfg.chunk_secs = static_cast<uint32_t>(m_v.chunk_secs);
		cfg.fec_group = fak_settings::abi_fec_group(m_fec, m_v.fec_group);
		cfg.tags = tag_ptrs.data();
		cfg.ntags = tag_ptrs.size();
		cfg.pics = pics.list.data();
		cfg.npics = pics.list.size();
		cfg.expected_frames = static_cast<uint64_t>(src.length * 48000.0);

		const pfc::string8 url = url_of(dest);
		bool created = false;
		try {
			filesystem::g_open_write_new(sink.f, url, abort);
			created = true;
			pcm_feeder feeder;
			feeder.stream_to(cfg, abort);
			progress_ctx dctx = { &status, &abort, 0.0, m_v.verify ? 0.8 : 1.0, index, m_jobs.size() };
			feeder.add(src, abort, [&](uint64_t done) {
				if (src.length > 0) report_progress(dctx, (std::min)(1.0, static_cast<double>(done) / (src.length * feeder.rate())));
			});
			if (!feeder.started()) throw std::runtime_error("the source contains no audio");
			feeder.end_stream(abort);
			sink.f.release();  // close the file before it is read back
			if (m_v.verify) {
				verify_written(url, 0, abort);
				report_progress({ &status, &abort, 0.8, 1.0, index, m_jobs.size() }, 1.0);
			}
		} catch (...) {
			sink.f.release();
			if (created) { try { filesystem::g_remove(url, fb2k::noAbort); } catch (...) {} }
			throw;
		}
		++m_ok;
	}

	std::vector<job> m_jobs;
	values m_v;
	int m_level;
	bool m_fec;
	unsigned m_ok = 0, m_skipped = 0, m_failed = 0;
	bool m_aborted = false;
	pfc::string8 m_report;
};

// ---- conversion worker: all selected tracks into one file with an embedded cue sheet ---------------

// The selected tracks, in order, become one FAK file. The tracks' boundaries are written as an embedded
// cue sheet, laid out the way foobar2000 itself lays out an embedded cue sheet (fields common to all
// tracks are file tags, the rest sit in the sheet), so the file shows as one playlist entry per track.
// Track positions in a cue sheet are whole CD frames (1/75 s), as with FLAC: a track that does not start
// on a frame boundary is placed at the nearest one.
class image_task : public threaded_process_callback {
public:
	image_task(metadb_handle_list items, std::string dest_base, values v, int level, bool fec)
		: m_items(std::move(items)), m_base(std::move(dest_base)), m_v(std::move(v)), m_level(level), m_fec(fec) {}

	void run(threaded_process_status& status, abort_callback& abort) override {
		try {
			do_run(status, abort);
		} catch (exception_aborted const&) {
			m_aborted = true;
		} catch (std::exception const& e) {
			m_error = e.what();
			FB2K_console_formatter() << "FAK: single-file conversion failed: " << e.what();
		}
	}

	void on_done(ctx_t, bool was_aborted) override {
		if (was_aborted) m_aborted = true;
		if (!m_error.is_empty()) {
			pfc::string8 msg = "The tracks were not converted to a single file:\n\n";
			msg += m_error;
			popup_message::g_show(msg, "FAK conversion");
		} else if (m_aborted) {
			FB2K_console_formatter() << "FAK: single-file conversion aborted";
		} else {
			FB2K_console_formatter() << "FAK: wrote " << m_dest.c_str() << " (" << m_items.get_count() << " tracks" << (m_cd ? ", CD tags" : "") << ")";
		}
	}

private:
	void do_run(threaded_process_status& status, abort_callback& abort) {
		const size_t n = m_items.get_count();
		for (size_t i = 0; i < n; ++i)
			if (is_fak_path(m_items[i]->get_path())) throw std::runtime_error("the selection contains a FAK file");
		make_folders(m_base, abort);
		m_dest = resolve_collision(m_base, m_v.exist, abort);
		if (m_dest.empty()) throw std::runtime_error("the destination file exists");

		// 1. decode every track into the one encoder. Integer audio is encoded to a temporary file as it is
		// decoded, so memory stays at a few chunks however long the album is; the cue sheet and tags, which
		// need every track's length, are added afterwards by rewriting the file's metadata block (the audio
		// is copied as it is). Float audio, and CD tags on a CD-format source (they are computed from all
		// the audio), are buffered instead.
		pcm_feeder feeder;
		fak_common::file_sink sink;
		sink.abort = &abort;
		const pfc::string8 part_url = url_of(m_dest + ".part");
		part_guard part{ &sink, part_url };
		pictures pics;
		stream_config cfg;
		bool streaming = false;
		std::vector<std::unique_ptr<file_info_impl>> infos;
		std::vector<uint64_t> starts;
		for (size_t i = 0; i < n; ++i) {
			abort.check();
			const char* path = m_items[i]->get_path();
			status.set_item_path(path);
			source src;
			open_source(m_items[i], abort, src);
			if (i == 0) {
				const bool cd_like = src.bits == 16 && src.info.info_get_int("samplerate") == 44100 && src.info.info_get_int("channels") == 2;
				streaming = !src.is_float && !(m_v.cd_tags && cd_like);
				if (m_v.album_art) pics.collect(m_items[0], abort);
				if (streaming) {
					double seconds = 0;
					for (size_t k = 0; k < n; ++k) seconds += m_items[k]->get_length();
					filesystem::g_open_write_new(sink.f, part_url, abort);
					part.active = true;
					cfg.sink = &sink;
					cfg.effort = m_level < archival_level ? m_level : 3;  // Archival = Insane effort
					cfg.threads = static_cast<uint32_t>(m_v.threads);
					cfg.chunk_secs = static_cast<uint32_t>(m_v.chunk_secs);
					cfg.fec_group = fak_settings::abi_fec_group(m_fec, m_v.fec_group);
					cfg.npics = pics.list.size();
					cfg.pics = pics.list.data();
					cfg.expected_frames = static_cast<uint64_t>(seconds * static_cast<double>(src.info.info_get_int("samplerate")));
					feeder.stream_to(cfg, abort);
				}
			}
			const uint64_t start = feeder.frames();
			const double share = streaming ? 0.9 : 0.6;
			const uint64_t added = feeder.add(src, abort, [&](uint64_t done) {
				const double frac = src.length > 0 ? (std::min)(1.0, static_cast<double>(done) / (src.length * feeder.rate())) : 0.0;
				status.set_progress_float(share * (static_cast<double>(i) + frac) / static_cast<double>(n));
			});
			if (added == 0) throw std::runtime_error(pfc::format("track ", i + 1, " (", path, ") contains no audio").c_str());
			starts.push_back(start);
			infos.push_back(std::make_unique<file_info_impl>(src.info));
		}
		if (streaming) {
			feeder.end_stream(abort);
			sink.f.release();  // close the file before it is read back
		}

		// 2. the cue sheet, and the tags of the file, through foobar2000's own embedding logic
		const unsigned rate = feeder.rate();
		cue_creator::t_entry_list entries;
		for (size_t i = 0; i < n; ++i) {
			auto it = entries.insert_last();
			it->m_file = "CDImage.wav";
			it->m_fileType = "WAVE";
			it->m_trackType = "AUDIO";
			it->m_track_number = static_cast<unsigned>(i + 1);
			it->set_simple_index(static_cast<double>(starts[i]) / rate);
		}
		pfc::string_formatter minimal = cue_creator::create(entries);
		file_info_impl image;
		image.set_length(static_cast<double>(feeder.frames()) / rate);
		image.meta_set("cuesheet", minimal);
		replaygain_info rg = infos[0]->get_replaygain();
		for (size_t i = 1; i < n; ++i) rg = rg.extract_common(infos[i]->get_replaygain());
		image.set_replaygain(rg);
		cue_parser::embeddedcue_metadata_manager cue;
		cue.set_tag(image);
		if (!cue.have_cuesheet()) throw std::runtime_error("the cue sheet could not be built (a track may be empty)");
		for (size_t i = 0; i < n; ++i) cue.set_track_info(static_cast<unsigned>(i + 1), *infos[i]);
		file_info_impl tagged;
		cue.get_tag(tagged);
		const std::vector<std::string> tags = tags_of(tagged, true);

		// CD tags only make sense (and are only exact) when the tracks are a CD image: 44.1 kHz 16-bit
		// stereo and every track boundary on a whole CD sector (588 sample-frames).
		bool exact_cd = feeder.cd_format() && feeder.frames() % 588 == 0;
		for (uint64_t s : starts) exact_cd = exact_cd && s % 588 == 0;
		m_cd = m_v.cd_tags && exact_cd;

		// 3. write the file (and verify it)
		status.set_item_path(m_base.c_str());
		if (streaming) {
			finish_streamed(part_url, tags, abort, status);
			part.remove();
			return;
		}
		progress_ctx ectx = { &status, &abort, 0.6, 0.95, 0, 1 };
		buf_guard out;
		encode_all(feeder, m_level, m_fec, m_cd, m_v, tags, pics, ectx, abort, out);
		feeder = pcm_feeder();
		write_and_verify(m_dest, out, m_v.verify, abort, [&](double p) { status.set_progress_float(0.95 + 0.05 * p); });
	}

	// The temporary file of a streamed conversion, removed unless it was handed on.
	struct part_guard {
		fak_common::file_sink* sink;
		pfc::string8 url;
		bool active = false;
		void remove() {
			if (!active) return;
			active = false;
			sink->f.release();
			try { filesystem::g_remove(url, fb2k::noAbort); } catch (...) {}
		}
		~part_guard() { remove(); }
	};

	// Rewrites the finished temporary file to `m_dest` with the tags (and with them the cue sheet)
	// added, copying the audio in pieces, then verifies it. A destination that failed is removed.
	void finish_streamed(pfc::string8 const& part_url, std::vector<std::string> const& tags, abort_callback& abort, threaded_process_status& status) {
		std::vector<const char*> ptrs;
		for (auto const& t : tags) ptrs.push_back(t.c_str());
		const pfc::string8 url = url_of(m_dest);
		bool created = false;
		fak_common::file_sink out;
		out.abort = &abort;
		try {
			fak_common::file_source in;
			filesystem::g_open_read(in.f, part_url, abort);
			in.abort = &abort;
			char err[512] = {};
			struct dec_free { void operator()(FakDecoder* d) const { fak_decoder_free(d); } };
			std::unique_ptr<FakDecoder, dec_free> dec(fak_decoder_open_cb(&fak_common::read_cb, &in, in.f->get_size_ex(abort), err, sizeof err));
			if (!dec) throw std::runtime_error(std::string("the encoded audio could not be reopened: ") + err);
			filesystem::g_open_write_new(out.f, url, abort);
			created = true;
			if (fak_rewrite_metadata_stream(dec.get(), ptrs.data(), ptrs.size(), 0, nullptr, 0, &fak_common::write_cb, &out) != 0) {
				abort.check();
				throw std::runtime_error(fak_decoder_last_error(dec.get()));
			}
			out.f.release();
			status.set_progress_float(0.95);
			if (m_v.verify) verify_written(url, 0, abort);
			status.set_progress_float(1.0);
		} catch (...) {
			out.f.release();
			if (created) { try { filesystem::g_remove(url, fb2k::noAbort); } catch (...) {} }
			throw;
		}
	}

	metadb_handle_list m_items;
	std::string m_base, m_dest;
	values m_v;
	int m_level;
	bool m_fec;
	bool m_cd = false, m_aborted = false;
	pfc::string8 m_error;
};

// ---- verification worker ----------------------------------------------------------------------

class verify_task : public threaded_process_callback {
public:
	explicit verify_task(std::vector<pfc::string8> paths) : m_paths(std::move(paths)) {}

	void run(threaded_process_status& status, abort_callback& abort) override {
		for (size_t i = 0; i < m_paths.size(); ++i) {
			const char* path = m_paths[i].c_str();
			status.set_item_path(path);
			status.set_progress_float(static_cast<double>(i) / static_cast<double>(m_paths.size()));
			try {
				fak_common::file_source src;
				filesystem::g_open_read(src.f, path, abort);
				src.abort = &abort;
				const t_filesize size = src.f->get_size_ex(abort);
				char err[512] = {};
				if (fak_verify_cb(&fak_common::read_cb, &src, size, 0, err, sizeof err) == 0) ++m_ok;
				else { ++m_bad; m_report << "FAILED: " << path << "\n    " << err << "\n"; }
			} catch (exception_aborted const&) {
				m_aborted = true;
				return;
			} catch (std::exception const& e) {
				++m_bad;
				m_report << "FAILED: " << path << "\n    " << e.what() << "\n";
			}
		}
	}

	void on_done(ctx_t, bool was_aborted) override {
		pfc::string8 msg;
		msg << m_ok << " file(s) decode to exactly the audio that was encoded";
		if (m_bad) msg << ", " << m_bad << " failed";
		if (m_aborted || was_aborted) msg << " (aborted before the end)";
		msg << ".";
		if (m_bad) msg << "\n\n" << m_report;
		FB2K_console_formatter() << "FAK verify: " << msg;
		popup_message::g_show(msg, "FAK verify");
	}

private:
	std::vector<pfc::string8> m_paths;
	unsigned m_ok = 0, m_bad = 0;
	bool m_aborted = false;
	pfc::string8 m_report;
};

// ---- menu -------------------------------------------------------------------------------------

constexpr unsigned run_flags = threaded_process::flag_show_progress | threaded_process::flag_show_item | threaded_process::flag_show_abort;

// `explicit_level`: a level picked from the "Convert with level" menu; Archival there always adds FEC.
// Otherwise the level and the FEC option are the ones in Preferences.
void convert_selection(metadb_handle_list_cref items, int level, bool explicit_level) {
	values v = load();
	std::vector<job> jobs = plan(items, v);
	if (jobs.empty()) return;
	const bool fec = v.fec || (explicit_level && level == archival_level);
	threaded_process::g_run_modal(fb2k::service_new<convert_task>(std::move(jobs), v, level, fec), run_flags,
		core_api::get_main_window(), "Converting to FAK");
}

bool any_fak(metadb_handle_list_cref items) {
	for (size_t i = 0; i < items.get_count(); ++i) if (is_fak_path(items[i]->get_path())) return true;
	return false;
}

void convert_single_selection(metadb_handle_list_cref items) {
	values v = load();
	std::string base, note;
	plan_image(items, v, base, note);
	if (!note.empty()) { popup_message::g_show(note.c_str(), "FAK conversion"); return; }
	threaded_process::g_run_modal(fb2k::service_new<image_task>(metadb_handle_list(items), base, v, v.level, v.fec), run_flags,
		core_api::get_main_window(), "Converting to one FAK file");
}

void verify_selection(metadb_handle_list_cref items) {
	std::vector<pfc::string8> paths;
	for (size_t i = 0; i < items.get_count(); ++i) {
		const char* p = items[i]->get_path();
		if (!is_fak_path(p)) continue;
		if (std::find_if(paths.begin(), paths.end(), [&](pfc::string8 const& q) { return q == p; }) == paths.end()) paths.emplace_back(p);
	}
	if (paths.empty()) return;
	threaded_process::g_run_modal(fb2k::service_new<verify_task>(std::move(paths)), run_flags, core_api::get_main_window(), "Verifying FAK files");
}

class fak_item : public contextmenu_item_simple {
public:
	using name_fn = std::function<void(pfc::string_base&)>;
	using vis_fn = std::function<bool(metadb_handle_list_cref)>;
	using run_fn = std::function<void(metadb_handle_list_cref)>;

	fak_item(GUID id, GUID parent, double priority, name_fn name, vis_fn visible, run_fn run, const char* description)
		: m_id(id), m_parent(parent), m_priority(priority), m_name(std::move(name)), m_visible(std::move(visible)), m_run(std::move(run)), m_desc(description) {}

	unsigned get_num_items() override { return 1; }
	void get_item_name(unsigned, pfc::string_base& out) override { m_name(out); }
	void context_command(unsigned, metadb_handle_list_cref items, const GUID&) override { m_run(items); }
	bool context_get_display(unsigned, metadb_handle_list_cref items, pfc::string_base& out, unsigned&, const GUID&) override {
		if (m_visible && !m_visible(items)) return false;
		m_name(out);
		return true;
	}
	GUID get_item_guid(unsigned) override { return m_id; }
	bool get_item_description(unsigned, pfc::string_base& out) override { out = m_desc; return true; }
	GUID get_parent() override { return m_parent; }
	double get_sort_priority() override { return m_priority; }

private:
	GUID m_id, m_parent;
	double m_priority;
	name_fn m_name;
	vis_fn m_visible;
	run_fn m_run;
	const char* m_desc;
};

service_factory_single_t<fak_item> g_item_convert(item_guid(0), guid_group_fak, 0.0,
	[](pfc::string_base& out) { auto v = load(); out = "Convert to FAK (level: "; out << level_names[v.level] << (v.fec ? ", with FEC" : "") << ")"; },
	nullptr, [](metadb_handle_list_cref items) { convert_selection(items, load().level, false); },
	"Converts the selected tracks to FAK at the level set in Preferences > Tools > FAK.");

// Two or more tracks (none of them FAK): one file with an embedded cue sheet.
service_factory_single_t<fak_item> g_item_single(item_guid(3), guid_group_fak, 0.5,
	[](pfc::string_base& out) { out = "Convert to one FAK file with a cue sheet"; },
	[](metadb_handle_list_cref items) { return items.get_count() >= 2 && !any_fak(items); },
	convert_single_selection,
	"Joins the selected tracks, in order, into one FAK file with an embedded cue sheet (and CD tags for an exact CD image, if enabled).");

// Five items under the "Convert with level" popup.
#define FAK_LEVEL_ITEM(N) 	service_factory_single_t<fak_item> g_item_level##N(item_guid(10 + N), guid_group_level, static_cast<double>(N), 		[](pfc::string_base& out) { out = level_names[N]; }, nullptr, 		[](metadb_handle_list_cref items) { convert_selection(items, N, true); }, level_descriptions[N])
FAK_LEVEL_ITEM(0);
FAK_LEVEL_ITEM(1);
FAK_LEVEL_ITEM(2);
FAK_LEVEL_ITEM(3);
FAK_LEVEL_ITEM(4);
#undef FAK_LEVEL_ITEM

service_factory_single_t<fak_item> g_item_verify(item_guid(1), guid_group_fak, 2.0,
	[](pfc::string_base& out) { out = "Verify FAK files"; }, any_fak, verify_selection,
	"Decodes the selected FAK files and checks them against the stored SHA-256 of the original audio.");

service_factory_single_t<fak_item> g_item_prefs(item_guid(2), guid_group_fak, 3.0,
	[](pfc::string_base& out) { out = "FAK preferences..."; }, nullptr,
	[](metadb_handle_list_cref) { ui_control::get()->show_preferences(guid_prefs_page); },
	"Opens Preferences > Tools > FAK.");

} // namespace
