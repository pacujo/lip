env = Environment()
env["ENV"]["PKG_CONFIG_PATH"] = "/usr/local/lib/pkgconfig"

packages = [
    "gtk+-3.0",
    "asynctls",
    "openssl",
    "async",
    "fstrace",
    "rotatable",
    "unixkit",
    "encjson",
    "fsdyn"
]

env.MergeFlags([
    f"!pkg-config {pkg} --cflags --libs"
    for pkg in packages
])

env.MergeFlags("-lm")

env.Program(
    "irc",
    ["irc.c"],
    CCFLAGS="-g -Wall -Werror",
)
