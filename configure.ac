# Process this file with autoconf to produce a configure script.
m4_include([version.m4])
AC_INIT([m4_PACKAGE_NAME], [m4_PACKAGE_VERSION], [m4_PACKAGE_BUGREPORT],
	[m4_PACKAGE_TARNAME])
AM_INIT_AUTOMAKE([AC_PACKAGE_TARNAME], [AC_PACKAGE_VERSION])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])

AC_CONFIG_SRCDIR([ctorrent.cpp])

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC

AC_LANG([C++])

# Checks for libaries.
AC_SEARCH_LIBS([clock_gettime],[c rt posix4],,
	[AC_MSG_ERROR([cannot find library for clock_gettime()])])

# Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_HEADER_TIME
AC_CHECK_HEADERS([arpa/inet.h fcntl.h limits.h memory.h netdb.h netinet/in.h sys/param.h sys/socket.h sys/time.h unistd.h])
AC_CHECK_HEADERS([termios.h termio.h sgtty.h ioctl.h sys/ioctl.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_C_INLINE
AC_PROG_GCC_TRADITIONAL
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_INT8_T
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_INT64_T
AC_TYPE_UINT8_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T

# Checks for library functions.
AC_FUNC_MEMCMP
AC_TYPE_SIGNAL
AC_FUNC_STAT
AC_FUNC_STRTOD
AC_CHECK_FUNCS([ftruncate gethostbyname gettimeofday getwd inet_ntoa memchr memmove memset mkdir select socket strchr strerror strncasecmp strstr strtol strtoll strnstr])

# Enable/check large file support
AC_SYS_LARGEFILE
AC_FUNC_FSEEKO

# Check for termios support
AC_SYS_POSIX_TERMIOS
if test "$ac_cv_sys_posix_termios" = yes; then
	AC_DEFINE([USE_TERMIOS],,
		[Define to use Posix termios for terminal control.])
	AC_HEADER_TIOCGWINSZ
else
	AC_CHECK_HEADER([termio.h],
		[AC_DEFINE([USE_TERMIO],,[Define to use termio for terminal control.])],
		[AC_SEARCH_LIBS([gtty],[compat],
			[AC_DEFINE([USE_SGTTY],,
				[Define to use sgtty.h (gtty/stty) for terminal control.])])])
fi

# Check for SSL support, letting the user give a location hint.
AC_ARG_WITH([ssl],
	[AS_HELP_STRING([--with-ssl=DIR],
		[use SSL libraries in DIR for SHA1 support])],
	[with_ssl=$withval],
	[])
AC_ARG_WITH([ssl],
	[AS_HELP_STRING([--with-ssl=no],
		[use builtin (Steve Reid's public-domain) SHA-1 code])])
if test "$with_ssl" = "no"; then
	echo "Forcing use of Steve Reid's public-domain SHA-1 code."
	AC_DEFINE([USE_STANDALONE_SHA1],,
		[Define to 1 to use Steve Reid's public-domain SHA-1 code.])
else
	if test -n "$with_ssl" -a "$with_ssl" != "yes"; then
		# a directory was specified
		echo "Trying SSL support in $with_ssl or system default locations."
		CPPFLAGS="$CPPFLAGS -I$with_ssl/include -I$with_ssl/include/openssl"
		CPPFLAGS="$CPPFLAGS -I$with_ssl/include/ssl -I$with_ssl"
		LDFLAGS="$LDFLAGS -L$with_ssl/lib -L$with_ssl/lib/openssl"
		LDFLAGS="$LDFLAGS -L$with_ssl/lib/ssl -L$with_ssl"
		AC_SEARCH_LIBS([SHA1_Init],[ssl crypto crypt md],,
			[AC_MSG_ERROR([cannot find library for SHA1_Init() under $with_ssl])])
	fi
	if test -z "$with_ssl" -o "$with_ssl" = "yes"; then
		# no dir given--fall back to builtin code if necessary
		AC_SEARCH_LIBS([SHA1_Init],[ssl crypto crypt md],,
			[AC_DEFINE([USE_STANDALONE_SHA1],,
				[Define to 1 to use Steve Reid's public-domain SHA-1 code.])])
	fi
	AC_CHECK_HEADERS([openssl/sha.h ssl/sha.h sha.h])
fi

AC_ARG_ENABLE([profile],
	[AS_HELP_STRING([--enable-profile], [enable code profiling])],
	[CXXFLAGS="$CXXFLAGS -pg"
	 CFLAGS="$CFLAGS -pg"
	],
	[])

AC_OUTPUT()

