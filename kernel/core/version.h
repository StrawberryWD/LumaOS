/* kernel/core/version.h - LumaOS 5.0
 *
 * DAY57: ONE place for every version / product string the system shows
 * (About window, `version`, boot banner, loader-independent banners).
 * Before this, "LumaOS 4.1" lived on in the About window and the kernel
 * banner while `version` already said 5.0.
 */
#ifndef LUMAOS_VERSION_H
#define LUMAOS_VERSION_H

#define LUMAOS_NAME      "LumaOS"
#define LUMAOS_VERSION   "5.0"
#define LUMAOS_FULLNAME  LUMAOS_NAME " " LUMAOS_VERSION     /* "LumaOS 5.0" */
#define LUMAOS_YEARS     "2025-2026"
#define LUMAOS_UI        "Material design"

#endif
