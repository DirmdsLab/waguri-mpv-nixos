add_languages('cpp', native: false)

qt = import('qt6')
waguri_gl = dependency('gl')
qt_dependencies = dependency('qt6', modules: ['Core', 'Gui', 'Widgets',
    'OpenGL', 'OpenGLWidgets', 'Network', 'Concurrent', 'DBus'])
resources = qt.compile_resources(sources: 'assets.qrc')
mpris_moc = qt.compile_moc(headers: 'mpris.hpp', dependencies: qt_dependencies)

executable('waguri-mpv',
    files('main.cpp', 'player.cpp', 'library.cpp', 'lyrics.cpp', 'tracklist.cpp', 'youtube.cpp', 'mpris.cpp'),
    resources, mpris_moc,
    include_directories: incdir_public,
    dependencies: [qt_dependencies, waguri_gl, libavformat, libavcodec, libavutil, libswscale],
    link_with: libmpv,
    install_rpath: join_paths('$ORIGIN', '..', get_option('libdir')),
    install: true,
)

install_data('io.waguri.Player.desktop',
    install_dir: join_paths(get_option('datadir'), 'applications'))
install_data('assets/app-icon.png',
    install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', '192x192', 'apps'),
    rename: 'io.waguri.Player.png')
