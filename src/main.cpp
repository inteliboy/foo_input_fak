#include "stdafx.h"

DECLARE_COMPONENT_VERSION(
	"FAK Lossless Audio",
	"1.0.0",
	"Playback, tagging and album art for FAK (.fak) lossless audio files.\n"
	"Decoding is bit-exact; tags and pictures are rewritten without touching the audio.\n\n"
	"The format is frozen: this build reads exactly one format version (see the \"fak_version\"\n"
	"technical info field).\n\n"
	"Encoding: the \"FAK\" context menu converts tracks (levels Fast to Archival; settings in\n"
	"Preferences > Tools > FAK). \"FAK\" is also added to the Converter's output formats on first start (bundled fak.exe).\n"
	"To add it by hand: custom encoder fak.exe, Extension: fak, Parameters: encode - %d,\n"
	"Format is: lossless, Highest BPS mode supported: 24.");

VALIDATE_COMPONENT_FILENAME("foo_input_fak.dll");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
