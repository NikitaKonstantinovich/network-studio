// ImGui's optional item hooks are enabled for the automated interaction test.
// The desktop application leaves TestEngineHookItems false and these hooks idle.
#include "imgui_internal.h"
void ImGuiTestEngineHook_ItemAdd(ImGuiContext*,ImGuiID,const ImRect&,const ImGuiLastItemData*) {}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*,ImGuiID,const char*,ImGuiItemStatusFlags) {}
void ImGuiTestEngineHook_Log(ImGuiContext*,const char*,...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*,ImGuiID) {return "";}
