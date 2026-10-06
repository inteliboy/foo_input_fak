#include "fak_common.h"
#include <commctrl.h>
#include <shobjidl.h>
#include <SDK/coreDarkMode.h>

// Preferences > Tools > FAK: the settings behind the "FAK" context menu (level slider, error-recovery
// data, output location, file name pattern, ...). Plain Win32 (the component is built without
// ATL/WTL): a child dialog created from an in-memory template, its controls made in WM_INITDIALOG.

namespace {

using namespace fak_settings;

enum {
	id_level_label = 1000, id_slider = 1001, id_level_name, id_level_desc,
	id_fec, id_fec_label, id_fec_group, id_fec_chunks, id_fec_hint,
	id_output_label,
	id_dest_source, id_dest_custom, id_folder, id_browse,
	id_pattern_label, id_pattern, id_image_label, id_image, id_pattern_hint,
	id_exist_label, id_exist, id_threads_label, id_threads, id_chunk_label, id_chunk, id_chunk_hint,
	id_art, id_verify, id_cdtags,
};

const int chunk_choices[] = { 0, 1, 2, 5, 10, 20 };  // seconds; 0 = automatic

class prefs_instance : public preferences_page_instance {
public:
	prefs_instance(HWND parent, preferences_page_callback::ptr callback) : m_callback(callback) {
		INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES };
		InitCommonControlsEx(&icc);

		// A child dialog template with no controls of its own: WS_CHILD | DS_CONTROL so it themes and
		// tabs like a page; Segoe UI 9 so dialog units match the rest of the preferences dialog.
		alignas(4) static const struct {
			DLGTEMPLATE t; WORD menu, cls, title; WORD pt; WCHAR face[10];
		} tmpl = {
			{ WS_CHILD | DS_CONTROL | DS_SETFONT, WS_EX_CONTROLPARENT, 0, 0, 0, 280, 254 },
			0, 0, 0, 9, L"Segoe UI",
		};
		m_wnd = CreateDialogIndirectParamW(core_api::get_my_instance(), &tmpl.t, parent, &prefs_instance::dlg_proc, reinterpret_cast<LPARAM>(this));
	}
	~prefs_instance() {
		if (m_wnd != nullptr && IsWindow(m_wnd)) DestroyWindow(m_wnd);
	}

	t_uint32 get_state() override {
		t_uint32 s = preferences_state::resettable | preferences_state::dark_mode_supported;
		if (differs(read_controls(), load())) s |= preferences_state::changed;
		return s;
	}
	fb2k::hwnd_t get_wnd() override { return m_wnd; }
	void apply() override {
		save(read_controls());
		m_callback->on_state_changed();
	}
	void reset() override {
		m_fec_auto = false;
		write_controls(defaults());
		m_callback->on_state_changed();
	}

private:
	static bool differs(values const& a, values const& b) {
		return a.level != b.level || a.dest != b.dest || a.folder != b.folder || a.pattern != b.pattern
			|| a.exist != b.exist || a.album_art != b.album_art || a.verify != b.verify
			|| a.fec != b.fec || a.fec_group != b.fec_group || a.threads != b.threads || a.chunk_secs != b.chunk_secs
			|| a.cd_tags != b.cd_tags || a.image_pattern != b.image_pattern;
	}

	static INT_PTR CALLBACK dlg_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
		prefs_instance* self;
		if (msg == WM_INITDIALOG) {
			self = reinterpret_cast<prefs_instance*>(lp);
			SetWindowLongPtrW(wnd, GWLP_USERDATA, lp);
			self->m_wnd = wnd;
			self->m_font = reinterpret_cast<HFONT>(SendMessageW(wnd, WM_GETFONT, 0, 0));
			if (self->m_font == nullptr) self->m_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
			self->create_controls();
			self->write_controls(load());
			self->m_dark.AddDialogWithControls(wnd);
			return TRUE;
		}
		self = reinterpret_cast<prefs_instance*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
		if (self == nullptr) return FALSE;
		switch (msg) {
		case WM_HSCROLL:
			if (reinterpret_cast<HWND>(lp) == GetDlgItem(wnd, id_slider)) self->on_level_moved();
			return TRUE;
		case WM_SIZE: self->layout(); return TRUE;
		case WM_COMMAND: self->on_command(LOWORD(wp), HIWORD(wp)); return TRUE;
		}
		return FALSE;
	}

	HWND make(LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y, int w, int h, int id) {
		RECT r = { x, y, x + w, y + h };
		MapDialogRect(m_wnd, &r);
		HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left, r.bottom - r.top,
			m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), core_api::get_my_instance(), nullptr);
		if (c != nullptr && m_font != nullptr) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), FALSE);
		return c;
	}

	// Edit box with the standard sunken border (the plain WS_BORDER look is too thin next to the rest of the dialog).
	HWND make_edit(int x, int y, int w, int h, int id, DWORD extra = 0) {
		RECT r = { x, y, x + w, y + h };
		MapDialogRect(m_wnd, &r);
		HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra,
			r.left, r.top, r.right - r.left, r.bottom - r.top, m_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), core_api::get_my_instance(), nullptr);
		if (c != nullptr && m_font != nullptr) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), FALSE);
		return c;
	}

	void create_controls() {
		// -- level
		make(L"STATIC", L"Compression level", 0, 0, 0, 300, 9, id_level_label);
		HWND slider = make(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_AUTOTICKS | TBS_HORZ, 0, 10, 215, 18, id_slider);
		SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELONG(0, level_count - 1));
		SendMessageW(slider, TBM_SETPAGESIZE, 0, 1);
		make(L"STATIC", L"", SS_LEFT, 222, 13, 78, 9, id_level_name);
		make(L"STATIC", L"", SS_LEFT, 0, 30, 300, 25, id_level_desc);

		// -- error-recovery data
		make(L"BUTTON", L"Add error-recovery data (FEC)", WS_TABSTOP | BS_AUTOCHECKBOX, 0, 58, 120, 10, id_fec);
		make(L"STATIC", L"one parity block per", SS_LEFT, 124, 59, 66, 9, id_fec_label);
		make_edit(192, 57, 26, 12, id_fec_group, ES_NUMBER);
		make(L"STATIC", L"chunks (0 = whole file)", SS_LEFT, 222, 59, 78, 9, id_fec_chunks);
		make(L"STATIC", L"Lets a damaged chunk be repaired when the file is decoded. Adds about 1/N of the file size. Turned on for the Archival level.",
			SS_LEFT, 12, 72, 288, 18, id_fec_hint);

		// -- output
		make(L"STATIC", L"Output", 0, 0, 94, 300, 9, id_output_label);
		make(L"BUTTON", L"Same folder as the source file, same file name", WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP, 0, 105, 300, 10, id_dest_source);
		make(L"BUTTON", L"Folder:", WS_TABSTOP | BS_AUTORADIOBUTTON, 0, 118, 46, 10, id_dest_custom);
		make_edit(48, 117, 196, 12, id_folder);
		make(L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, 248, 116, 52, 14, id_browse);
		make(L"STATIC", L"Track file:", SS_LEFT, 12, 129, 34, 9, id_pattern_label);
		make_edit(48, 128, 252, 12, id_pattern);
		make(L"STATIC", L"Album file:", SS_LEFT, 12, 143, 34, 9, id_image_label);
		make_edit(48, 142, 252, 12, id_image);
		make(L"STATIC", L"Title formatting; folders may be included; .fak is added. \"Album file\" names a single-file conversion.", SS_LEFT, 48, 156, 252, 18, id_pattern_hint);

		// -- behaviour
		make(L"STATIC", L"If the file exists:", SS_LEFT, 0, 179, 70, 9, id_exist_label);
		HWND exist = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 72, 177, 84, 60, id_exist);
		SendMessageW(exist, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Skip it"));
		SendMessageW(exist, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Overwrite it"));
		SendMessageW(exist, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Add a number"));

		make(L"STATIC", L"Encoder threads:", SS_LEFT, 166, 179, 62, 9, id_threads_label);
		HWND threads = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 230, 177, 70, 90, id_threads);
		SendMessageW(threads, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic"));
		const DWORD cpus = (std::min)(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), DWORD(64));
		for (DWORD n = 1; n <= cpus; ++n) {
			wchar_t text[16];
			swprintf_s(text, L"%lu", n);
			SendMessageW(threads, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
		}

		make(L"STATIC", L"Chunk length:", SS_LEFT, 0, 196, 70, 9, id_chunk_label);
		HWND chunk = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 72, 194, 84, 90, id_chunk);
		for (int secs : chunk_choices) {
			wchar_t text[24];
			if (secs == 0) wcscpy_s(text, L"Automatic"); else swprintf_s(text, secs == 1 ? L"%d second" : L"%d seconds", secs);
			SendMessageW(chunk, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
		}
		make(L"STATIC", L"A chunk is the unit of seeking and repair.", SS_LEFT, 166, 196, 134, 18, id_chunk_hint);

		make(L"BUTTON", L"Copy album art", WS_TABSTOP | BS_AUTOCHECKBOX, 0, 214, 140, 10, id_art);
		make(L"BUTTON", L"Verify after writing (read back)", WS_TABSTOP | BS_AUTOCHECKBOX, 144, 214, 156, 10, id_verify);
		make(L"BUTTON", L"Single file: write CD tags (disc IDs, AccurateRip, CTDB) for an exact CD image", WS_TABSTOP | BS_AUTOCHECKBOX, 0, 227, 300, 10, id_cdtags);

		layout();
	}

	void place(int id, int x, int y, int w, int h) {
		HWND c = GetDlgItem(m_wnd, id);
		if (c == nullptr) return;
		if (w < 1) w = 1;
		RECT r = { x, y, x + w, y + h };
		MapDialogRect(m_wnd, &r);
		SetWindowPos(c, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
	}

	// Positions every control for the page's current width (the preferences dialog sizes the page to its
	// pane, and the user can resize the dialog), in dialog units. Wide controls stretch, so nothing is
	// cut off at the right edge; multi-line texts wrap inside their boxes.
	void layout() {
		if (m_wnd == nullptr || GetDlgItem(m_wnd, id_slider) == nullptr) return;
		RECT cr, unit = { 0, 0, 100, 100 };
		GetClientRect(m_wnd, &cr);
		MapDialogRect(m_wnd, &unit);
		int W = unit.right > 0 ? cr.right * 100 / unit.right : 280;
		if (W < 250) W = 250;  // narrower than this and the page would need a scroll bar
		const int half = W / 2 + 4;

		place(id_level_label, 0, 0, W, 9);
		place(id_slider, 0, 10, W - 84, 18);
		place(id_level_name, W - 78, 13, 78, 9);
		place(id_level_desc, 0, 30, W, 26);

		place(id_fec, 0, 58, 124, 10);
		place(id_fec_label, 128, 59, 66, 9);
		place(id_fec_group, 196, 57, 26, 12);
		place(id_fec_chunks, 226, 59, W - 226, 9);
		place(id_fec_hint, 12, 72, W - 12, 18);

		place(id_output_label, 0, 94, W, 9);
		place(id_dest_source, 0, 105, W, 10);
		place(id_dest_custom, 0, 118, 46, 10);
		place(id_folder, 48, 117, W - 48 - 56, 12);
		place(id_browse, W - 52, 116, 52, 14);
		place(id_pattern_label, 12, 129, 34, 9);
		place(id_pattern, 48, 128, W - 48, 12);
		place(id_image_label, 12, 143, 34, 9);
		place(id_image, 48, 142, W - 48, 12);
		place(id_pattern_hint, 48, 156, W - 48, 18);

		place(id_exist_label, 0, 179, 66, 9);
		place(id_exist, 68, 177, half - 74, 60);
		place(id_threads_label, half, 179, 62, 9);
		place(id_threads, half + 64, 177, W - half - 64, 90);
		place(id_chunk_label, 0, 196, 66, 9);
		place(id_chunk, 68, 194, half - 74, 90);
		place(id_chunk_hint, half, 196, W - half, 18);

		place(id_art, 0, 214, half - 6, 10);
		place(id_verify, half - 2, 214, W - half + 2, 10);
		place(id_cdtags, 0, 227, W, 10);
		InvalidateRect(m_wnd, nullptr, TRUE);
	}

	values read_controls() const {
		values v = defaults();
		v.level = static_cast<int>(SendDlgItemMessageW(m_wnd, id_slider, TBM_GETPOS, 0, 0));
		v.dest = IsDlgButtonChecked(m_wnd, id_dest_custom) == BST_CHECKED ? dest_custom_folder : dest_source_folder;
		uGetDlgItemText(m_wnd, id_folder, v.folder);
		uGetDlgItemText(m_wnd, id_pattern, v.pattern);
		uGetDlgItemText(m_wnd, id_image, v.image_pattern);
		v.cd_tags = IsDlgButtonChecked(m_wnd, id_cdtags) == BST_CHECKED;
		const LRESULT e = SendDlgItemMessageW(m_wnd, id_exist, CB_GETCURSEL, 0, 0);
		v.exist = e == CB_ERR ? exist_number : static_cast<int>(e);
		v.album_art = IsDlgButtonChecked(m_wnd, id_art) == BST_CHECKED;
		v.verify = IsDlgButtonChecked(m_wnd, id_verify) == BST_CHECKED;
		v.fec = IsDlgButtonChecked(m_wnd, id_fec) == BST_CHECKED;
		BOOL ok = FALSE;
		const UINT g = GetDlgItemInt(m_wnd, id_fec_group, &ok, FALSE);
		v.fec_group = ok ? static_cast<int>((std::min)(g, UINT(max_fec_group))) : default_fec_group;
		const LRESULT t = SendDlgItemMessageW(m_wnd, id_threads, CB_GETCURSEL, 0, 0);
		v.threads = t == CB_ERR ? 0 : static_cast<int>(t);  //  is Automatic, item n is n threads
		const LRESULT c = SendDlgItemMessageW(m_wnd, id_chunk, CB_GETCURSEL, 0, 0);
		v.chunk_secs = (c == CB_ERR || c < 0 || c >= static_cast<LRESULT>(std::size(chunk_choices))) ? 0 : chunk_choices[c];
		return v;
	}

	void write_controls(values const& v) {
		SendDlgItemMessageW(m_wnd, id_slider, TBM_SETPOS, TRUE, v.level);
		CheckRadioButton(m_wnd, id_dest_source, id_dest_custom, v.dest == dest_custom_folder ? id_dest_custom : id_dest_source);
		uSetDlgItemText(m_wnd, id_folder, v.folder.c_str());
		uSetDlgItemText(m_wnd, id_pattern, v.pattern.c_str());
		uSetDlgItemText(m_wnd, id_image, v.image_pattern.c_str());
		CheckDlgButton(m_wnd, id_cdtags, v.cd_tags ? BST_CHECKED : BST_UNCHECKED);
		SendDlgItemMessageW(m_wnd, id_exist, CB_SETCURSEL, v.exist, 0);
		CheckDlgButton(m_wnd, id_art, v.album_art ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(m_wnd, id_verify, v.verify ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(m_wnd, id_fec, v.fec ? BST_CHECKED : BST_UNCHECKED);
		SetDlgItemInt(m_wnd, id_fec_group, static_cast<UINT>(v.fec_group), FALSE);
		const LRESULT threads_items = SendDlgItemMessageW(m_wnd, id_threads, CB_GETCOUNT, 0, 0);
		SendDlgItemMessageW(m_wnd, id_threads, CB_SETCURSEL, (std::min)(static_cast<LRESULT>(v.threads), threads_items - 1), 0);
		int chunk_index = 0;
		for (size_t i = 0; i < std::size(chunk_choices); ++i) if (chunk_choices[i] == v.chunk_secs) chunk_index = static_cast<int>(i);
		SendDlgItemMessageW(m_wnd, id_chunk, CB_SETCURSEL, chunk_index, 0);
		refresh_level();
		refresh_enabled();
	}

	void refresh_level() {
		int lv = static_cast<int>(SendDlgItemMessageW(m_wnd, id_slider, TBM_GETPOS, 0, 0));
		if (lv < 0) lv = 0;
		if (lv >= level_count) lv = level_count - 1;
		uSetDlgItemText(m_wnd, id_level_name, level_names[lv]);
		uSetDlgItemText(m_wnd, id_level_desc, level_descriptions[lv]);
	}

	void refresh_enabled() {
		const BOOL custom = IsDlgButtonChecked(m_wnd, id_dest_custom) == BST_CHECKED;
		for (int id : { id_folder, id_browse }) EnableWindow(GetDlgItem(m_wnd, id), custom);
		// The track pattern only applies with a custom folder; the album pattern names a single file in either mode.
		for (int id : { id_pattern_label, id_pattern }) EnableWindow(GetDlgItem(m_wnd, id), custom);
		const BOOL fec = IsDlgButtonChecked(m_wnd, id_fec) == BST_CHECKED;
		for (int id : { id_fec_label, id_fec_group, id_fec_chunks }) EnableWindow(GetDlgItem(m_wnd, id), fec);
	}

	// Archival turns FEC on; moving away from Archival turns it off again unless the user chose it
	// (touched the checkbox) in the meantime.
	void on_level_moved() {
		refresh_level();
		const int lv = static_cast<int>(SendDlgItemMessageW(m_wnd, id_slider, TBM_GETPOS, 0, 0));
		const bool fec = IsDlgButtonChecked(m_wnd, id_fec) == BST_CHECKED;
		if (lv == archival_level && !fec) {
			CheckDlgButton(m_wnd, id_fec, BST_CHECKED);
			m_fec_auto = true;
		} else if (lv != archival_level && fec && m_fec_auto) {
			CheckDlgButton(m_wnd, id_fec, BST_UNCHECKED);
			m_fec_auto = false;
		}
		refresh_enabled();
		changed();
	}

	void changed() { m_callback->on_state_changed(); }

	void on_command(int id, int code) {
		switch (id) {
		case id_dest_source:
		case id_dest_custom: refresh_enabled(); changed(); break;
		case id_folder:
		case id_pattern:
		case id_image:
		case id_fec_group: if (code == EN_CHANGE) changed(); break;
		case id_exist:
		case id_threads:
		case id_chunk: if (code == CBN_SELCHANGE) changed(); break;
		case id_fec: m_fec_auto = false; refresh_enabled(); changed(); break;
		case id_art:
		case id_verify:
		case id_cdtags: changed(); break;
		case id_browse: browse(); break;
		}
	}

	// Folder picker. It runs a modal loop, during which the preferences dialog could be closed and
	// this object released; hold a reference until it returns.
	void browse() {
		service_ptr_t<prefs_instance> keep(this);
		HWND owner = m_wnd;
		IFileOpenDialog* dlg = nullptr;
		if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
		DWORD opts = 0;
		dlg->GetOptions(&opts);
		dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
		if (SUCCEEDED(dlg->Show(owner)) && IsWindow(m_wnd)) {
			IShellItem* item = nullptr;
			if (SUCCEEDED(dlg->GetResult(&item))) {
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
					uSetDlgItemText(m_wnd, id_folder, pfc::stringcvt::string_utf8_from_wide(path).get_ptr());
					CoTaskMemFree(path);
					CheckRadioButton(m_wnd, id_dest_source, id_dest_custom, id_dest_custom);
					refresh_enabled();
					changed();
				}
				item->Release();
			}
		}
		dlg->Release();
	}

	HWND m_wnd = nullptr;
	HFONT m_font = nullptr;
	fb2k::CCoreDarkModeHooks m_dark;  // follows foobar2000's dark mode setting
	bool m_fec_auto = false;  // FEC was switched on by choosing Archival, not by the user
	preferences_page_callback::ptr m_callback;
};

class prefs_page : public preferences_page_v3 {
public:
	const char* get_name() override { return "FAK"; }
	GUID get_guid() override { return guid_prefs_page; }
	GUID get_parent_guid() override { return preferences_page::guid_tools; }
	preferences_page_instance::ptr instantiate(fb2k::hwnd_t parent, preferences_page_callback::ptr callback) override {
		return fb2k::service_new<prefs_instance>(parent, callback);
	}
};

preferences_page_factory_t<prefs_page> g_prefs_page_factory;

} // namespace
