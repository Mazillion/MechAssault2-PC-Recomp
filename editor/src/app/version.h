// The editor's version, shown in Help > About, the crash log header, the file properties and the saved scenarios (Editor = ...).
// res\editor.rc carries the same number (FILEVERSION / FileVersion / ProductVersion): change both.
#pragma once
#define MA2SE_VERSION "1.1.7"
#ifndef MA2SE_GIT
#define MA2SE_GIT "dev"  // build.cmd passes the short git id
#endif
