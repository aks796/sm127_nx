/* web_applet.h -- GodotIO.openURI via the system web applet. See web_applet.c.
 * MIT license; see LICENSE. */

#ifndef __WEB_APPLET_H__
#define __WEB_APPLET_H__

// --- engine thread -----------------------------------------------------------
// Queues a page and returns at once: 0 when queued, -1 when there is nowhere
// to open it (not http/https, or too long for the applet).
int  web_request(const char *url);
int  web_is_pending(void);

// --- main thread -------------------------------------------------------------
// Shows the page. BLOCKS until the user closes the browser, so this belongs
// on the appletMainLoop() thread, like swkbd_pump().
void web_pump(void);

#endif
