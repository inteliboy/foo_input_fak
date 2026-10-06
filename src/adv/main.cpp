#include "stdafx.h"

DECLARE_COMPONENT_VERSION(
	"FAK Lossless Audio (Advanced)",
	"1.1.1",
	"Everything in foo_input_fak, plus conversion and settings inside foobar2000.\n\n"
	"Playback, tagging and album art for FAK (.fak) lossless audio files. Decoding is bit-exact; tags and\n"
	"pictures are rewritten without touching the audio. The decoder reads exactly one format version (see the\n"
	"\"fak_version\" technical info field).\n\n"
	"Encoding: the \"FAK\" context menu converts tracks (levels Fast to Archival; settings in\n"
	"Preferences > Tools > FAK). \"FAK\" is also added to the Converter's output formats on first start (bundled fak.exe).\n"
	"To add it by hand: custom encoder fak.exe, Extension: fak, Parameters: encode - %d,\n"
	"Format is: lossless, Highest BPS mode supported: 24.\n\n"
	"This component replaces foo_input_fak: install one of the two, not both.");

VALIDATE_COMPONENT_FILENAME("foo_input_fak_adv.dll");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;

namespace {

// foo_input_fak_adv contains everything foo_input_fak does. With both installed every .fak file would be
// claimed twice, so say so once at startup instead of leaving the user to find out.
class component_conflict_check : public initquit {
public:
	void on_init() override {
		bool basic_installed = false;
		service_enum_t<componentversion> all;
		componentversion::ptr c;
		while (all.next(c)) {
			pfc::string8 file;
			c->get_file_name(file);
			// get_file_name() may or may not carry the ".dll" extension (the SDK's own filename validator
			// reads the full path instead), so accept both spellings.
			if (pfc::stringEqualsI_ascii(file.c_str(), "foo_input_fak.dll") ||
				pfc::stringEqualsI_ascii(file.c_str(), "foo_input_fak")) basic_installed = true;
		}
		if (basic_installed) {
			popup_message::g_complain("FAK Lossless Audio (Advanced)",
				"foo_input_fak and foo_input_fak_adv are both installed.\n\n"
				"foo_input_fak_adv already includes everything in foo_input_fak, so .fak files would be handled twice.\n"
				"Remove one of them in Preferences > Components (keep foo_input_fak_adv for the FAK menu) and restart foobar2000.");
		}
	}
};

initquit_factory_t<component_conflict_check> g_component_conflict_check;

} // namespace
