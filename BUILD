cc_binary(
    name = "dtorrent",
    srcs = ['config.h'] + glob(["*.cpp"]) + glob(["*.c"]) + glob(["*.h"]),
    copts = [
        '-fpermissive', # code has some const violations
    ],
    linkopts = [
        '-lrt',
        '-lssl',
    ],
)

genrule(
    name = 'config',
    srcs = [
        'ctorrent.cpp',
        'config.h.in',
        'install-sh',
        'Makefile.in',
        'missing',
    ],
    outs = [
        'config.h',
    ],
    cmd = '$(location configure); cp config.h $@',
    tools = ['configure'],
)
