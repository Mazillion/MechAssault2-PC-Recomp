/* ma2_release.h -- public release identity, in ONE place (W-06a).
 *
 * MA2_PUBLIC_VERSION is the number players see (game, editor and installer use the same one, D-6).
 * The game also reads recomp-install.json beside the exe for the installer's version and kit id
 * (written at install time); this define is the fallback and the exe's version resource (app.rc).
 *
 * MA2_SUPPORT_URL: PLACEHOLDER (D-11). The project has no support address yet. Put the one public
 * URL (issue tracker or forum thread) here; while it is empty the crash and hang boxes leave the
 * "Report at:" line out. Plain ASCII, no quotes.
 */
#pragma once

#define MA2_VERSION_MAJOR 1
#define MA2_VERSION_MINOR 1
#define MA2_VERSION_PATCH 2
#define MA2_PUBLIC_VERSION "1.1.2"

#define MA2_SUPPORT_URL "https://github.com/Mazillion/MechAssault2-PC-Recomp/issues"
