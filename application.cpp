#include "application.h"
#include "main_frame.h"
#include <wx/log.h>
#include <wx/file.h>

namespace fcn {

bool Application::OnInit() {
  if (!wxApp::OnInit()) {
    return false;
  }

  // Set WM_CLASS for dwm to identify all wxWidgets apps
  // Class = "wxapp" (generic for all wxWidgets applications)
  SetAppName("wxapp");  // This sets the CLASS in WM_CLASS

  // Enable logging to console for debugging
  wxLog::SetActiveTarget(new wxLogStderr());
  wxLog::SetLogLevel(wxLOG_Info);
  
  auto* frame = new ui::MainFrame();
  frame->Show(true);
  
  return true;
}

int Application::OnExit() {
  return wxApp::OnExit();
}

} // namespace fcn

wxIMPLEMENT_APP(fcn::Application);
