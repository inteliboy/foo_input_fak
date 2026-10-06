#pragma once
// Settings of the advanced component (foo_input_fak_adv): shared by the conversion menu and the preferences page.
#include "stdafx.h"

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
