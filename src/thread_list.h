#pragma once
#include <wx/vlbox.h>
#include <string>
#include <vector>

// One row of the thread pane.
struct ThreadRow {
    // Unseen: finished while not shown, not viewed since.
    enum class State { Done, Running, Unseen, Failed };
    std::string id;
    wxString title;
    wxString detail;  // the current step while running, else when last used
    State state = State::Done;
    bool child = false;  // a subagent, indented under its parent
};

// The thread pane's list: a state mark and the title, with a dim detail line
// below, in the system list colours. The selected row is the active thread.
class ThreadList : public wxVListBox {
public:
    explicit ThreadList(wxWindow* parent);

    // Replace the rows and select the one with `activeId`.
    void SetRows(std::vector<ThreadRow> rows, const std::string& activeId);
    const std::vector<ThreadRow>& Rows() const { return rows_; }

private:
    std::vector<ThreadRow> rows_;

    void OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const override;
    wxCoord OnMeasureItem(size_t n) const override;
};
