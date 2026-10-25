/*
 * Vcast 2026: small POSIX type/function replacements for MSVC (MinGW already
 * provides them). Included from compat.h and raop_ntp.h on Windows only.
 * Licensed under the GNU Lesser General Public License 2.1 or later, like the
 * surrounding UxPlay library code.
 */

#ifndef VCAST_WIN_TYPES_H
#define VCAST_WIN_TYPES_H

#if defined(_MSC_VER)
#include <BaseTsd.h>
#include <string.h>
#ifndef VCAST_SSIZE_T_DEFINED
#define VCAST_SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#endif /* _MSC_VER */

#endif
