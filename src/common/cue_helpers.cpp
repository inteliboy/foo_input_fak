#include "stdafx.h"

// The SDK's embedded-cuesheet support (helpers/cue_*.cpp) without the rest of the helpers
// project, which needs ATL. Each helper file starts with #include "StdAfx.h" from its own folder
// (which pulls in ATL); defining that header's include guard first makes the include a no-op, so
// the files compile against this component's stdafx.h instead.
#define AFX_STDAFX_H__6356EC2B_6DD1_4BE8_935C_87ECBA8697E4__INCLUDED_
#include <helpers/cue_creator.cpp>
#include <helpers/cue_parser.cpp>
#include <helpers/cue_parser_embedding.cpp>
#include <helpers/cuesheet_index_list.cpp>
