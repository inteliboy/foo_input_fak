#include "stdafx.h"

DECLARE_COMPONENT_VERSION(
	"FAK Lossless Audio",
	"1.1.1",
	"Playback, tagging and album art for FAK (.fak) lossless audio files.\n"
	"Decoding is bit-exact; tags and pictures are rewritten without touching the audio.\n\n"
	"The decoder reads exactly one format version (see the \"fak_version\" technical info field).\n\n"
	"Encoding: \"FAK\" is added to the Converter's output formats on first start (bundled fak.exe).\n"
	"To add it by hand: custom encoder fak.exe, Extension: fak, Parameters: encode - %d,\n"
	"Format is: lossless, Highest BPS mode supported: 24.\n\n"
	"The right-click \"FAK\" menu and the preferences page are in foo_input_fak_adv, which replaces this\n"
	"component (install one of the two, not both).");

VALIDATE_COMPONENT_FILENAME("foo_input_fak.dll");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
