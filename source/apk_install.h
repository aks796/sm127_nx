/* apk_install.h -- installs the game from its Android APK on the console.
 * See apk_install.c.
 *
 * MIT license; see LICENSE. */

#ifndef __APK_INSTALL_H__
#define __APK_INSTALL_H__

// Main thread, before check_data() and before EGL takes the window. An APK in
// the app folder that is not the one installed is installed now, with progress
// on screen. Returns 1 when it installed, 0 when there was nothing to do. Ends
// in fatal_error() when there is neither an install nor an APK, or the install
// fails.
int apk_install_run(void);

#endif
