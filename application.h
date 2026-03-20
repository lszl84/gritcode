#pragma once

#include <wx/wx.h>
#include <string>

namespace fcn {

class Application : public wxApp {
public:
  bool OnInit() override;
  int OnExit() override;
};

} // namespace fcn
