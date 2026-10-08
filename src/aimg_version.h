#ifndef AIMG_VERSION_H_INCLUDED
#define AIMG_VERSION_H_INCLUDED

/* Single source of truth for the plugin's version number. Bump this file
 * when releasing; keep pluginst.inf's `version=` key and CMakeLists.txt's
 * project(VERSION) in sync by hand (they're plain text/CMake, not compiled,
 * so they can't include this header).
 */

#define AIMG_VERSION_MAJOR 0
#define AIMG_VERSION_MINOR 3
#define AIMG_VERSION_PATCH 1

#define AIMG_VERSION_STRING "0.3.1"

#endif /* AIMG_VERSION_H_INCLUDED */
