#include "fak_common.h"

// foobar2000 input for FAK (.fak): playback with sample-accurate seeking, technical info, tags
// (incl. ReplayGain), tag writing, embedded cue sheets as subsongs, and embedded album art (read
// and edit). All codec work happens
// in the Rust library behind fak_capi.h; this file only adapts it to the foobar2000 SDK.

namespace {

#ifdef FAK_ADV
const GUID guid_input = { 0x6eb816fd, 0x9aaa, 0x4ca7, { 0xa0, 0xa9, 0xa7, 0x29, 0x3d, 0x57, 0x0c, 0x88 } };  // foo_input_fak_adv
#else
const GUID guid_input = { 0x9253c97d, 0x5df4, 0x4d21, { 0xbb, 0xbd, 0x94, 0x2c, 0x28, 0xb0, 0x23, 0x54 } };
#endif
#ifdef FAK_ADV
const GUID guid_aa_extractor = { 0x4ff23063, 0x7de8, 0x4979, { 0x9a, 0xc1, 0xab, 0x19, 0x44, 0xac, 0x2d, 0x6a } };  // foo_input_fak_adv
#else
const GUID guid_aa_extractor = { 0x2b8a81ee, 0x5cfc, 0x4ad4, { 0xa5, 0xaa, 0x29, 0xce, 0x21, 0xa2, 0x0f, 0x81 } };
#endif
#ifdef FAK_ADV
const GUID guid_aa_editor = { 0x78147cd4, 0x230e, 0x4525, { 0x98, 0x75, 0xa0, 0x6a, 0xc5, 0x23, 0x8c, 0x1b } };  // foo_input_fak_adv
#else
const GUID guid_aa_editor = { 0xeed378db, 0x0b20, 0x45b8, { 0x85, 0xb3, 0xb4, 0x5a, 0xa0, 0xd8, 0xb5, 0x9b } };
#endif

// Playback keeps nothing of the file in memory: the library reads it through `read_cb` (header,
// metadata and a small chunk index when opened, then one chunk at a time while decoding). Only the
// editing paths below (tag and album-art writes) hold a whole file, briefly. Refuse absurd sizes up
// front rather than attempting that allocation.
constexpr t_filesize max_file_size = t_filesize(1) << 32;

using fak_common::file_source;
using fak_common::read_cb;

struct decoder_deleter { void operator()(FakDecoder* d) const { fak_decoder_free(d); } };
using decoder_ptr = std::unique_ptr<FakDecoder, decoder_deleter>;

void read_all(file::ptr const& f, std::vector<uint8_t>& out, abort_callback& abort) {
	const t_filesize size = f->get_size_ex(abort);
	if (size > max_file_size) throw exception_io_data("FAK file too large");
	out.resize(static_cast<size_t>(size));
	f->seek(0, abort);
	if (!out.empty()) f->read_object(out.data(), out.size(), abort);
}

// Opens `src.f` through the read callback (nothing of the file is kept); `src` must outlive the decoder.
decoder_ptr open_decoder_cb(file_source& src, t_filesize size) {
	char err[512] = {};
	FakDecoder* d = fak_decoder_open_cb(&read_cb, &src, size, err, sizeof(err));
	if (d == nullptr) throw exception_io_data(pfc::format("Invalid or unsupported FAK file: ", err));
	return decoder_ptr(d);
}

decoder_ptr open_decoder(std::vector<uint8_t> const& bytes) {
	char err[512] = {};
	FakDecoder* d = fak_decoder_open(bytes.data(), bytes.size(), err, sizeof(err));
	if (d == nullptr) throw exception_io_data(pfc::format("Invalid or unsupported FAK file: ", err));
	return decoder_ptr(d);
}

using fak_common::kind_to_id;
using fak_common::id_to_kind;
using fak_common::sniff_mime;

// Writes `buf` over the whole file and truncates it.
void write_all(file::ptr const& f, FakBuffer const& buf, abort_callback& abort) {
	f->seek(0, abort);
	f->write_object(buf.data, buf.len, abort);
	f->set_eof(abort);
}

// Tags currently in the file, as "KEY=VALUE" strings.
std::vector<std::string> current_tags(FakDecoder* d) {
	std::vector<std::string> out;
	for (size_t i = 0, n = fak_decoder_tag_count(d); i < n; ++i) out.emplace_back(fak_decoder_tag(d, i));
	return out;
}

// Rewrites the file with the given tags and (optionally) pictures; throws on failure.
void rewrite(file::ptr const& f, FakDecoder* d, std::vector<std::string> const& tags,
             bool replace_pictures, std::vector<FakPictureIn> const& pictures, abort_callback& abort) {
	std::vector<const char*> ptrs;
	for (auto const& t : tags) ptrs.push_back(t.c_str());
	FakBuffer buf = {};
	if (fak_rewrite_metadata(d, ptrs.data(), ptrs.size(), replace_pictures ? 1 : 0, pictures.data(), pictures.size(), &buf) != 0)
		throw exception_io_data(pfc::format("FAK tag update failed: ", fak_decoder_last_error(d)));
	try { write_all(f, buf, abort); } catch (...) { fak_buffer_free(buf); throw; }
	fak_buffer_free(buf);
}

class input_fak : public input_stubs {
public:
	void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason, abort_callback& p_abort) {
		m_file = p_filehint;
		input_open_file_helper(m_file, p_path, p_reason, p_abort);
		load(p_abort);
	}

	void get_info(file_info& p_info, abort_callback&) {
		const double length = m_info.sample_rate ? double(m_info.total_frames) / double(m_info.sample_rate) : 0.0;
		p_info.set_length(length);
		p_info.info_set_int("samplerate", m_info.sample_rate);
		p_info.info_set_int("channels", m_info.channels);
		// A float stream stores 32-bit floats; bits_per_sample is only its internal integer mapping.
		p_info.info_set_int("bitspersample", m_info.is_float ? 32 : m_info.bits_per_sample);
		if (m_info.is_float) p_info.info_set("sample_format", "float32");
		p_info.info_set("encoding", "lossless");
		p_info.info_set("codec", "FAK");
		p_info.info_set_int("fak_version", m_info.format_version);
		if (length > 0) p_info.info_set_bitrate(static_cast<t_int64>(double(m_size) * 8.0 / length / 1000.0 + 0.5));
		if (m_info.has_channel_mask) p_info.info_set_wfx_chanMask(m_info.channel_mask);
		const char* vendor = fak_decoder_vendor(m_dec.get());
		if (vendor && *vendor) p_info.info_set("tool", vendor);

		// The stream profile, from the file itself: chunk length (the unit of seeking and repair) and
		// error-recovery data. The compression level is not recorded in the stream; when the encoder
		// wrote its settings into the encoder string ("... level=insane; fec=RS whole-file; chunk=auto ..."),
		// the level is shown from there.
		pfc::string8 chunk_text, fec_text;
		if (m_info.chunk_frames > 0 && m_info.sample_rate > 0) chunk_text << pfc::format_float(double(m_info.chunk_frames) / m_info.sample_rate, 0, 2) << " s";
		if (m_info.fec_group > 0) fec_text << "Reed-Solomon, " << m_info.parity_blocks << (m_info.parity_blocks == 1 ? " block" : " blocks") << " of up to " << m_info.fec_group << " chunks";
		else fec_text = "none";
		pfc::string8 profile = "block";
		if (!chunk_text.is_empty()) profile << ", " << chunk_text << " chunks";
		if (m_info.fec_group > 0) profile << ", FEC RS"; else profile << ", no FEC";
		if (vendor) {
			if (const char* lv = strstr(vendor, "level=")) {
				lv += 6;
				const char* end = lv;
				while (*end && *end != ';' && *end != ')' && *end != ' ') ++end;
				if (end > lv) {
					pfc::string8 level;
					level.add_string(lv, static_cast<t_size>(end - lv));
					p_info.info_set("fak_level", level);
					pfc::string8 with_level;
					with_level << level << ", " << profile;
					profile = with_level;
				}
			}
		}
		p_info.info_set("codec_profile", profile);
		if (!chunk_text.is_empty()) p_info.info_set("fak_chunk_length", chunk_text);
		p_info.info_set_int("fak_chunks", static_cast<t_int64>(m_info.chunk_count));
		p_info.info_set("fak_fec", fec_text);
		pfc::string8 hash;
		for (uint8_t b : m_info.pcm_sha256) hash << pfc::format_hex(b, 2);
		p_info.info_set("pcm_sha256", hash);

		for (auto const& tag : current_tags(m_dec.get())) {
			const size_t eq = tag.find('=');
			if (eq == std::string::npos || eq == 0) continue;
			const std::string name = tag.substr(0, eq), value = tag.substr(eq + 1);
			if (!p_info.info_set_replaygain(name.c_str(), value.c_str())) p_info.meta_add(name.c_str(), value.c_str());
		}
		// A file with only the binary cue sheet (no CUESHEET tag) still shows its tracks: the SDK's
		// cue wrapper reads the "cuesheet" field, so hand it the generated text. Writing tags then
		// stores it as a tag, which the library turns back into the same binary cue sheet.
		const char* cue = fak_decoder_cuesheet(m_dec.get());
		if (cue != nullptr && p_info.meta_get("cuesheet", 0) == nullptr) p_info.meta_set("cuesheet", cue);
	}

	t_filestats2 get_stats2(unsigned f, abort_callback& a) { return m_file->get_stats2_(f, a); }
	t_filestats get_file_stats(abort_callback& p_abort) { return m_file->get_stats(p_abort); }

	void decode_initialize(unsigned, abort_callback&) {
		m_chunk = 0;
		m_skip = 0;
	}

	bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
		p_abort.check();
		m_src.abort = &p_abort;
		if (m_chunk >= m_info.chunk_count) return false;
		const uint64_t frames = fak_decoder_chunk_frames(m_dec.get(), m_chunk);
		const size_t nch = m_info.channels;
		int64_t got;
		if (m_info.is_float) {
			m_fbuffer.resize(static_cast<size_t>(frames) * nch);
			got = fak_decoder_decode_chunk_float(m_dec.get(), m_chunk, m_fbuffer.data(), m_fbuffer.size());
		} else {
			m_buffer.resize(static_cast<size_t>(frames) * nch);
			got = fak_decoder_decode_chunk(m_dec.get(), m_chunk, m_buffer.data(), m_buffer.size());
		}
		if (got < 0) throw exception_io_data(pfc::format("FAK decode error: ", fak_decoder_last_error(m_dec.get())));
		++m_chunk;
		const size_t skip = static_cast<size_t>(pfc::min_t<uint64_t>(m_skip, static_cast<uint64_t>(got)));
		m_skip = 0;
		const size_t n = static_cast<size_t>(got) - skip;
		if (n == 0) return decode_run(p_chunk, p_abort);
		if (m_info.is_float) {
			// The samples are the source's own float32 values, bit for bit (float -> double is exact).
			const float* f = m_fbuffer.data() + skip * nch;
			m_abuffer.resize(n * nch);
			for (size_t i = 0; i < n * nch; ++i) m_abuffer[i] = static_cast<audio_sample>(f[i]);
			p_chunk.set_data(m_abuffer.data(), n, m_info.channels, m_info.sample_rate, channel_config());
			return true;
		}
		// Samples arrive at the stream's own scale; left-justify them into 32-bit so foobar2000's
		// fixed-point import sees full scale (exact: audio_sample is a double on x64, which holds 32 bits).
		const unsigned shift = 32 - m_info.bits_per_sample;
		int32_t* s = m_buffer.data() + skip * nch;
		for (size_t i = 0; i < n * nch; ++i) s[i] = static_cast<int32_t>(static_cast<uint32_t>(s[i]) << shift);
		p_chunk.set_data_fixedpoint_signed(s, n * nch * sizeof(int32_t), m_info.sample_rate, m_info.channels, 32, channel_config());
		return true;
	}

	void decode_seek(double p_seconds, abort_callback&) {
		const uint64_t target = audio_math::time_to_samples(p_seconds, m_info.sample_rate);
		const int64_t chunk = fak_decoder_chunk_for_frame(m_dec.get(), target);
		if (chunk < 0) { m_chunk = m_info.chunk_count; m_skip = 0; return; }
		m_chunk = static_cast<uint64_t>(chunk);
		m_skip = target - fak_decoder_chunk_start(m_dec.get(), m_chunk);
	}

	bool decode_can_seek() { return true; }
	bool decode_get_dynamic_info(file_info&, double&) { return false; }
	bool decode_get_dynamic_info_track(file_info&, double&) { return false; }
	void decode_on_idle(abort_callback& p_abort) { m_file->on_idle(p_abort); }

	void retag(const file_info& p_info, abort_callback& p_abort) {
		std::vector<std::string> tags;
		for (size_t i = 0, n = p_info.meta_get_count(); i < n; ++i) {
			const char* name = p_info.meta_enum_name(i);
			for (size_t j = 0, m = p_info.meta_enum_value_count(i); j < m; ++j)
				tags.push_back(std::string(name) + "=" + p_info.meta_enum_value(i, j));
		}
		const replaygain_info rg = p_info.get_replaygain();
		replaygain_info::t_text_buffer t;
		if (rg.format_track_gain(t)) tags.push_back(std::string("REPLAYGAIN_TRACK_GAIN=") + t);
		if (rg.format_track_peak(t)) tags.push_back(std::string("REPLAYGAIN_TRACK_PEAK=") + t);
		if (rg.format_album_gain(t)) tags.push_back(std::string("REPLAYGAIN_ALBUM_GAIN=") + t);
		if (rg.format_album_peak(t)) tags.push_back(std::string("REPLAYGAIN_ALBUM_PEAK=") + t);
		m_src.abort = &p_abort;
		rewrite(m_file, m_dec.get(), tags, false, {}, p_abort);
		load(p_abort);
	}

	void remove_tags(abort_callback& p_abort) {
		m_src.abort = &p_abort;
		rewrite(m_file, m_dec.get(), {}, true, {}, p_abort);
		load(p_abort);
	}

	static bool g_is_our_content_type(const char*) { return false; }
	static bool g_is_our_path(const char*, const char* p_extension) { return stricmp_utf8(p_extension, "fak") == 0; }
	static const char* g_get_name() { return "FAK lossless audio decoder"; }
	static GUID g_get_guid() { return guid_input; }

private:
	void load(abort_callback& p_abort) {
		m_dec.reset();  // the old decoder reads through m_src: drop it before m_src is repointed
		m_src.f = m_file;
		m_src.abort = &p_abort;
		m_size = m_file->get_size_ex(p_abort);
		m_dec = open_decoder_cb(m_src, m_size);
		if (fak_decoder_info(m_dec.get(), &m_info) != 0) throw exception_io_data("FAK: cannot read stream info");
		if (m_info.channels == 0 || m_info.bits_per_sample == 0 || m_info.sample_rate == 0)
			throw exception_io_unsupported_format();
	}

	unsigned channel_config() const {
		if (m_info.has_channel_mask && m_info.channel_mask != 0 && static_cast<uint32_t>(std::popcount(m_info.channel_mask)) == m_info.channels)
			return audio_chunk::g_channel_config_from_wfx(m_info.channel_mask);
		return audio_chunk::g_guess_channel_config(m_info.channels);
	}

	service_ptr_t<file> m_file;
	file_source m_src;
	t_filesize m_size = 0;
	decoder_ptr m_dec;
	FakInfo m_info = {};
	std::vector<int32_t> m_buffer;
	std::vector<float> m_fbuffer;
	std::vector<audio_sample> m_abuffer;
	uint64_t m_chunk = 0;
	uint64_t m_skip = 0;
};

// Embedded cue sheets (the "cuesheet" field) become subsongs, edited per track, via the SDK wrapper.
static input_cuesheet_factory_t<input_fak> g_input_fak_factory;

// Embedded pictures of a FAK file, keyed by foobar2000 album art id (first picture of each type).
album_art_extractor_instance_ptr open_art(file_ptr f, abort_callback& abort, bool allow_empty) {
	file_source src;
	src.f = f;
	src.abort = &abort;
	decoder_ptr d = open_decoder_cb(src, f->get_size_ex(abort));
	auto inst = fb2k::service_new<album_art_extractor_instance_simple>();
	for (size_t i = 0, n = fak_decoder_picture_count(d.get()); i < n; ++i) {
		FakPicture p = {};
		GUID id;
		if (fak_decoder_picture(d.get(), i, &p) != 0 || !kind_to_id(p.kind, id) || inst->have_item(id)) continue;
		inst->set(id, album_art_data_impl::g_create(p.data, p.data_len));
	}
	if (inst->is_empty() && !allow_empty) throw exception_album_art_not_found();
	return inst;
}

class album_art_extractor_fak : public album_art_extractor_v2 {
public:
	bool is_our_path(const char*, const char* p_extension) override { return stricmp_utf8(p_extension, "fak") == 0; }
	album_art_extractor_instance_ptr open(file_ptr p_filehint, const char* p_path, abort_callback& p_abort) override {
		file_ptr f = p_filehint;
		if (f.is_empty()) filesystem::g_open_read(f, p_path, p_abort);
		return open_art(f, p_abort, false);
	}
	GUID get_guid() override { return guid_input; }
};

// Edits pictures in place: pictures of types foobar2000 has no id for are kept unless remove_all().
class album_art_editor_instance_fak : public album_art_editor_instance_v2 {
public:
	album_art_editor_instance_fak(file_ptr f, abort_callback& abort) : m_file(f) {
		m_src.f = m_file;
		m_src.abort = &abort;
		m_dec = open_decoder_cb(m_src, m_file->get_size_ex(abort));
		for (size_t i = 0, n = fak_decoder_picture_count(m_dec.get()); i < n; ++i) {
			FakPicture p = {};
			if (fak_decoder_picture(m_dec.get(), i, &p) != 0) continue;
			GUID id;
			if (kind_to_id(p.kind, id)) {
				if (!m_art.have_item(id)) m_art.set(id, album_art_data_impl::g_create(p.data, p.data_len));
			} else {
				m_other.push_back({ p.kind, p.mime, p.description, std::vector<uint8_t>(p.data, p.data + p.data_len) });
			}
		}
	}
	album_art_data_ptr query(const GUID& what, abort_callback& abort) override {
		abort.check();
		album_art_data_ptr out;
		if (!m_art.query(what, out)) throw exception_album_art_not_found();
		return out;
	}
	void set(const GUID& what, album_art_data_ptr data, abort_callback&) override {
		if (id_to_kind(what) == 0) throw exception_album_art_unsupported_entry();
		m_art.set(what, data);
	}
	void remove(const GUID& what) override { m_art.remove(what); }
	void remove_all() override { m_art.remove_all(); m_other.clear(); }
	void commit(abort_callback& abort) override {
		std::vector<FakPictureIn> pics;
		std::vector<std::string> mimes;
		mimes.reserve(m_art.get_count());
		m_art.enumerate([&](GUID const& id, album_art_data_ptr const& data) {
			const uint8_t* p = static_cast<const uint8_t*>(data->data());
			mimes.emplace_back(sniff_mime(p, data->size()));
			pics.push_back({ id_to_kind(id), mimes.back().c_str(), "", p, data->size() });
		});
		for (auto const& o : m_other) pics.push_back({ o.kind, o.mime.c_str(), o.description.c_str(), o.data.data(), o.data.size() });
		m_src.abort = &abort;
		rewrite(m_file, m_dec.get(), current_tags(m_dec.get()), true, pics, abort);
	}

private:
	struct other_picture { uint32_t kind; std::string mime, description; std::vector<uint8_t> data; };
	file_ptr m_file;
	file_source m_src;
	decoder_ptr m_dec;
	pfc::map_t<GUID, album_art_data_ptr> m_art;
	std::vector<other_picture> m_other;
};

class album_art_editor_fak : public album_art_editor_v2 {
public:
	bool is_our_path(const char*, const char* p_extension) override { return stricmp_utf8(p_extension, "fak") == 0; }
	album_art_editor_instance_ptr open(file_ptr p_filehint, const char* p_path, abort_callback& p_abort) override {
		file_ptr f = p_filehint;
		if (f.is_empty()) filesystem::g_open(f, p_path, filesystem::open_mode_write_existing, p_abort);
		return new service_impl_t<album_art_editor_instance_fak>(f, p_abort);
	}
	GUID get_guid() override { return guid_input; }
};

static service_factory_single_t<album_art_extractor_fak> g_album_art_extractor_fak;
static service_factory_single_t<album_art_editor_fak> g_album_art_editor_fak;

} // namespace

DECLARE_FILE_TYPE("FAK lossless audio", "*.FAK");
