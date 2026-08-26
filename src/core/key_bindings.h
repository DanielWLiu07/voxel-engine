#pragma once

#include <GLFW/glfw3.h>

namespace core {

// Every key the player can press, in one table.
//
// The help text and the handler used to be forty lines apart and share
// nothing, so they could disagree - and did: C copies a performance
// snapshot to the clipboard and appeared in no printed line, which made it
// a feature only readable in the source. A table the help is printed FROM
// and the handler indexes INTO cannot drift, because there is only one
// place a key appears.
//
// Movement and mouse bindings are not here: they are continuous rather
// than a toggle, and the loop reads them positionally. print_bindings()
// still lists them, and they are marked in the source as the one part of
// this that is duplicated on purpose.
enum class Bind {
    Quit, Cursor, Hud, Screenshot,
    PauseTime, StepTimeForward, StepTimeBack,
    Occlusion, Wireframe, Vsync,
    Save, Load, WalkFly, CopyPerf,
    Count,
};

struct KeyBinding {
    int         key;     // GLFW_KEY_*
    const char* shown;   // how it is printed
    const char* action;  // what it does
};

inline constexpr KeyBinding kBindings[static_cast<int>(Bind::Count)] = {
    {GLFW_KEY_ESCAPE,        "ESC",   "quit"},
    {GLFW_KEY_TAB,           "Tab",   "toggle mouse capture"},
    {GLFW_KEY_F2,            "F2",    "toggle the debug HUD"},
    {GLFW_KEY_F12,           "F12",   "screenshot to ./screenshots"},
    {GLFW_KEY_T,             "T",     "pause time of day"},
    {GLFW_KEY_RIGHT_BRACKET, "]",     "step time of day forward"},
    {GLFW_KEY_LEFT_BRACKET,  "[",     "step time of day back"},
    {GLFW_KEY_O,             "O",     "toggle section-occlusion culling"},
    {GLFW_KEY_G,             "G",     "toggle wireframe terrain"},
    {GLFW_KEY_V,             "V",     "toggle vsync"},
    {GLFW_KEY_F5,            "F5",    "save the world to ./saves/world1"},
    {GLFW_KEY_F6,            "F6",    "load the world from ./saves/world1"},
    {GLFW_KEY_F,             "F",     "toggle walk / fly"},
    {GLFW_KEY_C,             "C",     "copy a perf snapshot to the clipboard"},
};

constexpr int key_of(Bind b) { return kBindings[static_cast<int>(b)].key; }

// Prints the table, then the continuous bindings the table cannot hold.
void print_bindings();

}  // namespace core
