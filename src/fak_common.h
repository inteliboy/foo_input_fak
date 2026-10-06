#pragma once
// Helpers shared by the input (input_fak.cpp) and the conversion menu (convert.cpp).
#include "stdafx.h"
#include "fak_capi.h"

namespace fak_common {

// FLAC/ID3 picture types <-> foobar2000 album art ids.
struct art_kind { uint32_t kind; GUID id; };
inline const art_kind* art_kinds(size_t& count) {
	static const art_kind kinds[] = {
		{ 3, album_art_ids::cover_front },
		{ 4, album_art_ids::cover_back },
		{ 6, album_art_ids::disc },
		{ 8, album_art_ids::artist },
		{ 1, album_art_ids::icon },
	};
	count = sizeof kinds / sizeof kinds[0];
	return kinds;
}
inline bool kind_to_id(uint32_t kind, GUID& out) {
	size_t n; const art_kind* k = art_kinds(n);
	for (size_t i = 0; i < n; ++i) if (k[i].kind == kind) { out = k[i].id; return true; }
	return false;
}
inline uint32_t id_to_kind(GUID const& id) {
	size_t n; const art_kind* k = art_kinds(n);
	for (size_t i = 0; i < n; ++i) if (k[i].id == id) return k[i].kind;
	return 0;
}
inline const char* sniff_mime(const uint8_t* p, size_t n) {
	if (n >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
	if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "image/jpeg";
	if (n >= 6 && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0)) return "image/gif";
	if (n >= 12 && memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0) return "image/webp";
	if (n >= 2 && p[0] == 'B' && p[1] == 'M') return "image/bmp";
	return "image/";
}

// What the library's read callback needs (`fak_read_fn`): the host file and the abort_callback of the
// operation in progress. The caller points `abort` at its own abort_callback before every call into
// the library that may read, so a cancelled operation stops its I/O.
struct file_source {
	file::ptr f;
	abort_callback* abort = nullptr;
};

inline int read_cb(void* user, uint64_t pos, uint8_t* buf, size_t len) {
	auto* s = static_cast<file_source*>(user);
	if (s->abort == nullptr) return 1;
	try {
		s->f->seek(pos, *s->abort);
		s->f->read_object(buf, len, *s->abort);
		return 0;
	} catch (...) {
		return 1;  // never let a C++ exception unwind through the Rust library
	}
}

// The output side, for `fak_write_fn`: the file being written and the operation's abort_callback.
struct file_sink {
	file::ptr f;
	abort_callback* abort = nullptr;
};

inline int write_cb(void* user, uint64_t pos, const uint8_t* buf, size_t len) {
	auto* s = static_cast<file_sink*>(user);
	if (s->abort == nullptr) return 1;
	try {
		s->f->seek(pos, *s->abort);
		s->f->write_object(buf, len, *s->abort);
		return 0;
	} catch (...) {
		return 1;  // never let a C++ exception unwind through the Rust library
	}
}

} // namespace fak_common

// User settings (settings.cpp). The conversion menu and the preferences page share them.
namespace fak_settings {

enum { level_count = 5, archival_level = 4, default_fec_group = 0, max_fec_group = 255 };
// The C ABI's fec_group: 0 = no FEC, 0xFFFFFFFF = one Reed-Solomon block over the whole file, else chunks per block.
inline uint32_t abi_fec_group(bool fec, int group) { return !fec ? 0u : group <= 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(group); }
extern const char* const level_names[level_count];        // "Fast" ... "Archival"
extern const char* const level_descriptions[level_count]; // one sentence each, shown by the slider

enum dest_mode { dest_source_folder = 0, dest_custom_folder = 1 };
enum exist_mode { exist_skip = 0, exist_overwrite = 1, exist_number = 2 };

struct values {
	int level;            // 0..4 (Archival = Insane effort; error-recovery data is the separate `fec` option)
	int dest;             // dest_mode
	pfc::string8 folder;  // native path, used with dest_custom_folder
	pfc::string8 pattern; // titleformat, used with dest_custom_folder (no extension)
	int exist;            // exist_mode
	bool album_art;       // copy album art from the source
	bool verify;          // read the written file back and check it against the source PCM hash
	bool fec;             // add error-recovery (FEC) parity chunks; the page turns it on when the level is Archival
	int fec_group;        // data chunks per Reed-Solomon parity block, 1..255; 0 = one block for the whole file
	int threads;          // 0 = automatic
	int chunk_secs;       // seconds per chunk, 0 = automatic
	bool cd_tags;         // single-file conversion: add CD identifiers and AccurateRip/CTDB checksums when the result is an exact CD image
	pfc::string8 image_pattern; // titleformat of the single-file (album image) name, no extension
};

values load();
void save(values const& v);
values defaults();

extern const GUID guid_prefs_page;

} // namespace fak_settings
