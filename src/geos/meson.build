libgeos_sources = files(
    'geoshost.cpp',
    'geoshost_fs.cpp',
    'geoshost_mem.cpp',
    'geoshost_hostfs.cpp',
    'hostfs_volume.cpp',
    'tlse.c',
)

libgeos = static_library(
    'geos',
    libgeos_sources,
    include_directories: incdir,
    dependencies: [
        sdl2_dep,
        sdl2_net_dep,
    ],
)

libgeos_dep = declare_dependency(link_with: libgeos)

internal_deps += libgeos_dep