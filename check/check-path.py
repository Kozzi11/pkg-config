#/usr/bin/env python

import sys
from pkgchecker import PkgChecker

# tests = [(0, '-I/non-l/include -I/non-l-required/include', '', {}, ['--cflags', 'non-l-required', 'non-l']),
# 
#          RESULT=""
# PKG_CONFIG_PATH="$srcdir/sub" run_test --exists sub1
# 
# # default pkg-config path, making sure to resolve the variables fully
# eval pc_path="$pc_path"
# if [ "$native_win32" = yes ]; then
#     # This is pretty hacky. On native win32 (MSYS/MINGW), pkg-config
#     # builds the default path from the installation directory. It
#     # then adds lib/pkgconfig and share/pkgconfig to it. Normally,
#     # the autoconf build directory would be used, but that path is in
#     # Unix format.
#     if [ "$OSTYPE" = msys ]; then
#         # MSYS has "pwd -W" to get the current directory in Windows format
#         pcdir=$(cd $top_builddir/.libs && pwd -W)
#     else
#         # Assume we have cmd somewhere to get variable %cd%. Make sure
#         # to strip carriage returns.
#         pcdir=$(cd $top_builddir/.libs &&
#                 $WINE cmd /C echo %cd% | sed -r 's/\r//g')
#     fi
#     win_path="$pcdir/lib/pkgconfig;$pcdir/share/pkgconfig"
# 
#     # Convert from forward slashes to Windows backslashes
#     RESULT=$(echo $win_path | sed 's,/,\\,g')
# else
#     RESULT=$pc_path
# fi
# 
# unset PKG_CONFIG_LIBDIR
# run_test --variable=pc_path pkg-config

if __name__ == '__main__':
   print('This test requires manual work.')
   sys.exit(77)

