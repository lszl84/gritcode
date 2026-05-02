#include <wx/wx.h>
#include "chat_frame.h"
#include "shell_env.h"
#include "preferences.h"

class App : public wxApp {
public:
    bool OnInit() override {
        SetAppName("wx_gritcode");
        Preferences::Init();

        // Pull PATH and friends from the user's login shell so tool subprocesses
        // see the same env they'd see in a terminal — matters when launched
        // from a .desktop file or DE menu where rc-files never ran.
        ImportShellEnv();

        auto* frame = new ChatFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(App);
