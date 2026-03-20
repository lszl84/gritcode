#include "application.h"
#include "main_frame.h"
#include <wx/log.h>
#include <wx/file.h>

namespace fcn {

bool Application::OnInit() {
  if (!wxApp::OnInit()) {
    return false;
  }

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
