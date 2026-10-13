// 40_AutoBattler: top-down auto battler. Gameplay lives in AutoBattlerSim (Flecs); this file owns
// rendering, camera, input and UI.

#include "AutoBattlerSim.h"

#include <stdlib.h>
#include <string.h>

#include "../../../../Common_3/Application/Interfaces/IApp.h"
#include "../../../../Common_3/Graphics/Interfaces/IGraphics.h"

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

class AutoBattler: public IApp
{
public:
    bool Init()
    {
        for (int i = 1; i < argc; ++i)
        {
            if (strcmp(argv[i], "--selftest") == 0)
                exit(runAutoBattlerSelfTest("40_AutoBattler_selftest.txt") == 0 ? 0 : 1);
        }
        ShowUnsupportedMessage("Rendering is not implemented yet; run with --selftest");
        return false;
    }

    void Exit() {}

    bool Load(ReloadDesc*) { return true; }

    void Unload(ReloadDesc*) {}

    void Update(float) {}

    void Draw() {}

    const char* GetName() { return "40_AutoBattler"; }
};

DEFINE_APPLICATION_MAIN(AutoBattler)
