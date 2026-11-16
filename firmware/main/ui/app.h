// Reader application: file manager screen + paginated reader screen.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "board/board.h"
#include "input/gestures.h"
#include "text/doc.h"
#include "text/layout.h"
#include "text/types.h"

namespace reader {

class App : public GestureSink {
  public:
    bool begin();
    void run();
    void on_gesture(Gesture gesture) override;

    // Developer hooks used by the debug console (tools/devtool.py).
    void inject(Gesture gesture);
    bool open_path(const std::string& relative);
    bool open_index(int index);
    void reset_progress();
    // Forces a full GC16 repaint (clears ghosting on demand).
    void request_clean();
    // True when App::begin() has finished: debug commands that change state must
    // wait, otherwise they race the boot sequence.
    bool booted() const { return _booted; }
    // True while a frame is being drawn or a book is being opened.
    bool busy() const { return _busy; }
    // Debug hook: overrides the per-format open limits (0 restores defaults).
    void set_open_limit(uint64_t bytes);
    // Text currently drawn on screen (for end-to-end checks over serial).
    std::string dump_text();
    // Reference-mode geometry, read-only, for the debug console.
    bool link_mode() const { return _link_mode; }
    int link_focus() const { return _link_focus; }
    int link_count() const { return static_cast<int>(_page_links.size()); }
    int link_line_count() const { return static_cast<int>(_link_lines.size()); }
    int link_visible() const { return _link_visible; }
    int link_max_scroll() const { return _link_max_scroll; }
    int link_panel_height() const { return _link_panel_h; }
    int page_shift() const { return _page_shift; }

  private:
    struct FileEntry {
        std::string name;
        bool is_dir = false;
        bool parent = false;
        uint64_t size = 0;
    };

    enum class Screen { FileList, Reader, Message, Loading, Question, Booting };

    // file manager
    void load_directory(const std::string& relative, const std::string& select_name = "");
    void activate();
    void handle_file_list(Gesture gesture);

    // reader
    bool open_file(const std::string& relative);
    void close_reader();
    void handle_reader(Gesture gesture);
    void turn_page(int delta);
    // Single pass: page containing `position`, back history and page number.
    bool walk_to_position(const PagePos& position, PageView& containing);
    // Re-lays out around `position` after a font size change.
    void snap_to_layout(const PagePos& position);
    bool extend_history_backwards();
    void add_checkpoint(const PagePos& position);
    void change_font(int delta);
    void save_progress(bool force);
    // Signature of the current layout: a saved page number is only valid for it.
    uint32_t layout_signature() const;
    LayoutMetrics reader_metrics() const;

    // messages
    void set_message(const std::string& title, const std::string& body, Screen back_to);
    void handle_message(Gesture gesture);

    // drawing: only the parts that changed reach the E-Ink panel
    enum Part : uint8_t { kHeaderPart = 1, kContentPart = 2, kFooterPart = 4, kAllParts = 7 };
    void mark(uint8_t parts);
    void mark_all();
    void render();
    void render_header();
    // "75%" when the charge is known, "--" when the gauge is not readable yet.
    std::string battery_text() const;
    void draw_battery_icon(M5GFX& gfx, int x, int y);
    void render_content();
    void render_reader_content();
    void render_content_row(int row);
    void render_row_icon(int row, uint16_t ink);
    const char* icon_name_for(const FileEntry& entry, bool locked, bool selected) const;
    void render_scrollbar(bool content_area);
    int row_baseline(int row) const;
    void render_cursor_marks(int row);
    void render_footer();
    void render_message();
    void render_loading();
    void render_booting();
    // Footnote/reference navigation: enter with a long A+B, A/B move the focus,
    // a short A+B leaves the mode. The focused note is shown in a bottom panel.
    void enter_link_mode();
    void leave_link_mode();
    void move_link_focus(int delta);
    void render_link_marks();
    void render_link_panel();
    // Wraps the focused note and computes the panel geometry without drawing, so
    // scrolling works before the first frame of the panel is rendered.
    void prepare_link_text();
    struct PageLink {
        int line = 0;
        uint32_t note = 0;
        int16_t x = 0;
        int16_t width = 0;
        std::string label; // the reference as printed in the book, e.g. "[1]"
    };
    std::vector<PageLink> page_links() const;
    int focused_link_line() const;
    void render_question();
    // Shared card renderer for message/question screens (large type).
    void draw_dialog(const std::string& title, const std::string& body, const char* hint_left,
                     const char* hint_right);
    void show_question();
    void handle_question(Gesture gesture);
    // Tries to rescue a file whose encoding/structure the parser rejected: decodes
    // with the best-guess encoding, sanitises XML and writes a UTF-8 copy.
    bool repair_file(const std::string& relative, std::string* repaired);
    void finish_repair(bool replace_original);
  public:
    // Debug entry point: runs the repair on a path and shows the same dialog.
    bool force_repair(const std::string& relative);
  private:
    void show_loading(const std::string& name, const char* stage, int percent);
    Region header_region() const;
    Region content_region() const;
    Region footer_region() const;
    Region row_region(int row) const;
    Region cursor_region(int row) const;
    int list_row_height() const;
    int visible_rows() const;
    // Keeps _cursor inside the visible window, updating _scroll when it leaves.
    void ensure_cursor_visible();

    Screen _screen = Screen::FileList;
    Screen _message_back = Screen::FileList;
    uint8_t _dirty_parts = kAllParts;
    bool _full_repaint = true;
    bool _row_refresh = false;
    int _pages_since_clean = 0;
    int _prev_cursor = -1;
    int _prev_scroll = -1;

    std::string _dir; // relative to the SD root, "" = root
    std::vector<FileEntry> _entries;
    int _cursor = 0;
    int _scroll = 0;

    std::string _file;
    std::unique_ptr<ByteSource> _source;
    std::unique_ptr<Document> _doc;
    std::unique_ptr<Paginator> _paginator;
    PageView _page;
    std::vector<PagePos> _history;
    // Layout-dependent page positions with their page numbers.
    struct Checkpoint {
        PagePos position;
        int index = 0;
    };
    std::vector<Checkpoint> _checkpoints;
    int _page_index = 1;
    int _total_pages = 0;
    bool _counting = false;
    void start_counting();
    void step_counting();
    bool _force_clean = false;
    volatile bool _booted = false;
    volatile bool _busy = false;
    std::string _pending_resume;
    bool _link_mode = false;
    int _link_focus = -1;
    std::vector<PageLink> _page_links;
    std::vector<std::string> _link_lines; // wrapped note body for the current focus
    uint32_t _link_lines_note = 0;        // which note _link_lines belongs to
    int _link_scroll = 0;                 // first visible wrapped line
    int _link_visible = 0;                // lines that fit into the panel
    int _link_max_scroll = 0;
    int _page_shift = 0;    // temporary upward shift of the page while browsing notes
    int _link_panel_h = 0;  // panel height chosen for the current focus
    int _link_line = -1;    // line of the focused reference (for partial repaints)
    uint32_t _link_shown_note = 0;
    int _font_size = 1;
    uint32_t _last_save_ms = 0;
    int _pages_since_save = 0;
    bool _end_of_book = false;
    bool _restart_book = false;

    std::string _question_title;
    std::string _question_body;
    std::string _question_file;
    std::string _question_repaired;
    std::string _loading_name;
    const char* _loading_stage = "";
    int _loading_percent = 0;
    std::string _message_title;
    std::string _message_body;

    int _battery_percent = -1;
    uint32_t _battery_read_ms = 0;
    Gesture _pending_injected = Gesture::None;
    bool _has_injected = false;
};

App& app();

} // namespace reader
