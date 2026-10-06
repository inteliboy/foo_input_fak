#include "fak_common.h"

// Persistent settings of the conversion menu. Stored with foobar2000's
// modern cfg_var types, so they follow the profile.

namespace fak_settings {

const char* const level_names[level_count] = { "Fast", "Normal", "Max", "Insane", "Archival" };

// Statements taken from the encoder's own documentation (src/encoder.rs `Effort`); no
// figures are quoted here because they depend on the material and the machine.
const char* const level_descriptions[level_count] = {
	"Fastest encoding; fixed block size and no second-opinion search. Larger files.",
	"The default: variable block size and cross-channel prediction. A good balance of speed and size.",
	"Encodes every candidate block for real. Several times slower than Normal, smaller files.",
	"Adds the stage-2 adaptive filter: the smallest files, but decoding takes about twice as long as Max.",
	"Insane effort, for archives. Turns on error-recovery data (FEC) below, so a damaged chunk can be repaired.",
};

const GUID guid_prefs_page = { 0xade0e96f, 0x9b9b, 0x4f78, { 0xb1, 0x70, 0x5f, 0x5a, 0x24, 0x14, 0x34, 0xb8 } };

namespace {
cfg_int cfg_level({ 0x45828f80, 0xaa36, 0x432a, { 0x8e, 0xba, 0xa1, 0x09, 0x63, 0x47, 0x5c, 0x87 } }, 1);
cfg_int cfg_dest({ 0x620cd821, 0xbdbf, 0x41a8, { 0x8e, 0x95, 0x39, 0x94, 0x15, 0x3e, 0x8e, 0x0f } }, dest_source_folder);
cfg_string cfg_folder({ 0x661f8789, 0x96e2, 0x4fe5, { 0x81, 0x70, 0x5c, 0xae, 0xd9, 0xfc, 0x38, 0xdd } }, "");
cfg_string cfg_pattern({ 0xeee4bade, 0xf5e9, 0x4776, { 0xbf, 0x3d, 0x96, 0x8b, 0xe6, 0x9f, 0xce, 0x6d } }, "%album artist%\\%album%\\%tracknumber%. %title%");
cfg_int cfg_exist({ 0xcac996d3, 0x4c6e, 0x4bdd, { 0xb8, 0x66, 0x85, 0x31, 0x6f, 0xde, 0x1c, 0x4d } }, exist_number);
cfg_bool cfg_art({ 0xce1d24e6, 0x1073, 0x4978, { 0x9c, 0x56, 0x0e, 0x0c, 0x98, 0x71, 0x79, 0xba } }, true);
cfg_bool cfg_fec({ 0x8ceec5c7, 0x2d1b, 0x4829, { 0xa4, 0x25, 0xa9, 0x6d, 0x26, 0x9c, 0xe2, 0x1c } }, false);
cfg_int cfg_fec_group({ 0x03df84ac, 0xb106, 0x48a2, { 0xad, 0x11, 0xbd, 0x1b, 0xc6, 0xae, 0xd0, 0xa3 } }, default_fec_group);
cfg_int cfg_threads({ 0x20509f8e, 0x2c19, 0x41da, { 0x80, 0x78, 0x2c, 0x7d, 0x65, 0x0e, 0x80, 0x7d } }, 0);
cfg_int cfg_chunk({ 0x8e325820, 0x8ac8, 0x4c8d, { 0x85, 0xfe, 0x1d, 0xd0, 0xbb, 0x96, 0xf2, 0x24 } }, 0);
cfg_bool cfg_cdtags({ 0x5a7d10c2, 0x6b3e, 0x4f0a, { 0x9e, 0x21, 0x3c, 0x88, 0x0d, 0x54, 0xa1, 0x77 } }, false);
// The album name, with the album artist before it when there is one; the first track's file name when the tracks have no album.
cfg_string cfg_image({ 0x91c4e6f3, 0x27d8, 0x4b1c, { 0xa6, 0x0f, 0x52, 0xe9, 0x13, 0xbb, 0x4c, 0x08 } }, "[%album artist% - ]$if2(%album%,%filename%)");
cfg_bool cfg_verify({ 0xd2c3e9db, 0xdb87, 0x44d4, { 0x8c, 0x12, 0xda, 0xb9, 0x98, 0x2c, 0x35, 0x9e } }, true);

int clamp(int64_t v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : static_cast<int>(v); }
}

values defaults() {
	values v;
	v.level = 1;
	v.dest = dest_source_folder;
	v.pattern = "%album artist%\\%album%\\%tracknumber%. %title%";
	v.exist = exist_number;
	v.album_art = true;
	v.verify = true;
	v.fec = false;
	v.fec_group = default_fec_group;
	v.threads = 0;
	v.chunk_secs = 0;
	v.cd_tags = false;
	v.image_pattern = "[%album artist% - ]$if2(%album%,%filename%)";
	return v;
}

values load() {
	values v;
	v.level = clamp(cfg_level.get(), 0, level_count - 1);
	v.dest = clamp(cfg_dest.get(), dest_source_folder, dest_custom_folder);
	v.folder = cfg_folder.get();
	v.pattern = cfg_pattern.get();
	v.exist = clamp(cfg_exist.get(), exist_skip, exist_number);
	v.album_art = cfg_art.get();
	v.verify = cfg_verify.get();
	v.fec = cfg_fec.get();
	v.fec_group = clamp(cfg_fec_group.get(), 0, max_fec_group);
	v.threads = clamp(cfg_threads.get(), 0, 256);
	v.chunk_secs = clamp(cfg_chunk.get(), 0, 600);
	v.cd_tags = cfg_cdtags.get();
	v.image_pattern = cfg_image.get();
	return v;
}

void save(values const& v) {
	cfg_level = clamp(v.level, 0, level_count - 1);
	cfg_dest = clamp(v.dest, dest_source_folder, dest_custom_folder);
	cfg_folder = v.folder.c_str();
	cfg_pattern = v.pattern.c_str();
	cfg_exist = clamp(v.exist, exist_skip, exist_number);
	cfg_art = v.album_art;
	cfg_verify = v.verify;
	cfg_fec = v.fec;
	cfg_fec_group = clamp(v.fec_group, 0, max_fec_group);
	cfg_threads = clamp(v.threads, 0, 256);
	cfg_chunk = clamp(v.chunk_secs, 0, 600);
	cfg_cdtags = v.cd_tags;
	cfg_image = v.image_pattern.c_str();
}

} // namespace fak_settings
