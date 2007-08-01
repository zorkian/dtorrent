# Process this file with autoconf to produce a configure script.
AC_INIT([CTorrent], [dnh2.2], [dholmes@ct.boxmail.com])
AM_INIT_AUTOMAKE(ctorrent,dnh2.2)
AC_CONFIG_HEADER([config.h])

AC_CONFIG_SRCDIR([ctorrent.cpp])

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC

# Checks for libraries.
AC_CHECK_LIB([ssl],[SHA1_Init],,
	[AC_CHECK_LIB([crypt],[SHA1_Init],,
	[AC_CHECK_LIB([crypto],[SHA1_Init],,
	[AC_CHECK_LIB([md],[SHA1_Init],,
	[AC_DEFINE([USE_STANDALONE_SHA1],,[Define to 1 to use the Steve Reid's public-domain SHA-1 code.])])])])])

# Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_CHECK_HEADERS([arpa/inet.h fcntl.h limits.h memory.h netdb.h netinet/in.h stdlib.h string.h sys/param.h sys/socket.h sys/time.h unistd.h ssl/sha.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_C_INLINE
AC_TYPE_SIZE_T
AC_HEADER_TIME

# Checks for library functions.
AC_PROG_GCC_TRADITIONAL
AC_FUNC_MEMCMP
AC_TYPE_SIGNAL
AC_FUNC_STAT
AC_CHECK_FUNCS([ftruncate gethostbyname gettimeofday getwd inet_ntoa memchr memmove memset mkdir select socket strchr strerror strncasecmp strstr strtol strtoll strnstr])

# Enable/check large file support
AC_SYS_LARGEFILE
AC_FUNC_FSEEKO

AC_OUTPUT(Makefile)
