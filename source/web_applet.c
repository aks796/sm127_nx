/* web_applet.c -- GodotIO.openURI on the Switch's own browser.
 *
 * SM127's login screen links to Level Share Square for "Sign up" and "Forgot
 * password?" (OS.shell_open). On Android that is a browser intent; here it was
 * a stub that returned an error, so both buttons did nothing.
 *
 * Two applets can show a page:
 *
 *   WebApplet        the full browser. Only available when the process runs
 *                    as an Application (title takeover / forwarder).
 *   WifiWebAuthApplet the captive-portal browser. Available from applet mode
 *                    (the album), which is how homebrew is often launched.
 *
 * The first is tried when it can work, the second otherwise or if it fails.
 * Either one blocks until closed and suspends the game while up, so the page
 * is queued from the engine thread and shown from the main thread, the same
 * arrangement as the software keyboard (swkbd_shim.c).
 *
 * MIT license; see LICENSE. */

#include <string.h>
#include <strings.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "web_applet.h"

#if DEBUG_LOG
#define webLog(...) debugPrintf("[web] " __VA_ARGS__)
#else
#define webLog(...) do {} while (0)
#endif

// WifiWebAuthApplet's initial_url field is 0x400 bytes, the smaller of the two.
#define URL_MAX 0x400

static Mutex s_lock;
static int   s_pending;
static char  s_url[URL_MAX];

// SM127 links to its Discord server from the main menu's Discord button and
// the editor's help pages. Discord cannot be used in the Switch's browser, so
// those links are not opened; the helper also hides the menu button.
static int is_discord_link(const char *url) {
  const char *host = strstr(url, "://");
  if (!host) return 0;
  host += 3;
  const size_t n = strcspn(host, "/:?#");
  static const char *const domains[] = { "discord.gg", "discord.com", "discordapp.com" };
  for (unsigned i = 0; i < sizeof(domains) / sizeof(*domains); i++) {
    const size_t k = strlen(domains[i]);
    if (n >= k && !strncasecmp(host + n - k, domains[i], k) && (n == k || host[n - k - 1] == '.'))
      return 1;
  }
  return 0;
}

int web_request(const char *url) {
  if (!url || (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0)) {
    webLog("not opened: '%s' is not a web address\n", url ? url : "(null)");
    return -1;
  }
  if (is_discord_link(url)) {
    webLog("not opened: %s is a Discord link, which the Switch browser cannot use\n", url);
    return -1;
  }
  if (strlen(url) >= URL_MAX) {
    webLog("not opened: address is %zu bytes, the browser takes %d\n", strlen(url), URL_MAX - 1);
    return -1;
  }

  mutexLock(&s_lock);
  const int busy = s_pending;
  if (!busy) {
    strcpy(s_url, url);
    s_pending = 1;
  }
  mutexUnlock(&s_lock);

  if (busy) webLog("a page is already opening; ignored %s\n", url);
  else      webLog("queued %s\n", url);
  return 0;
}

int web_is_pending(void) {
  mutexLock(&s_lock);
  const int p = s_pending;
  mutexUnlock(&s_lock);
  return p;
}

void web_pump(void) {
  char url[URL_MAX];
  mutexLock(&s_lock);
  if (!s_pending) { mutexUnlock(&s_lock); return; }
  strcpy(url, s_url);
  mutexUnlock(&s_lock);

  const AppletType type = appletGetAppletType();
  const int application = type == AppletType_Application || type == AppletType_SystemApplication;
  Result rc = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

  if (application) {
    WebCommonConfig cfg;
    rc = webPageCreate(&cfg, url);
    if (R_SUCCEEDED(rc)) rc = webConfigSetWhitelist(&cfg, "^http");
    if (R_SUCCEEDED(rc)) rc = webConfigShow(&cfg, NULL);
    webLog("browser (WebApplet): 0x%x\n", rc);
  }
  if (!application || R_FAILED(rc)) {
    WebWifiConfig cfg;
    Uuid uuid;
    memset(&uuid, 0, sizeof(uuid));
    webWifiCreate(&cfg, NULL, url, uuid, 0);
    rc = webWifiShow(&cfg, NULL);
    webLog("browser (WifiWebAuthApplet): 0x%x\n", rc);
  }

  mutexLock(&s_lock);
  s_pending = 0;
  mutexUnlock(&s_lock);
}
