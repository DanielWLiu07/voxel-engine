#pragma once

// Tracy zone macros. When the project is built without VOXEL_USE_TRACY, all
// of these compile to nothing - sources can sprinkle ZoneScoped freely and
// pay zero cost in release builds.
#if defined(VOXEL_USE_TRACY)
  #include <tracy/Tracy.hpp>
#else
  #define ZoneScoped         do {} while (0)
  #define ZoneScopedN(name)  do {} while (0)
  #define FrameMark          do {} while (0)
  #define FrameMarkNamed(n)  do {} while (0)
#endif
