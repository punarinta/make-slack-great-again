# msga_app_claude — the Claude Code workspace: every Claude Code session on
# this machine as a DM (read from Claude Code's own files), written to through
# background sessions the `claude` CLI starts and resumes.
add_library(msga_app_claude STATIC
    # shared
    common.cpp
    async.cpp
    # the CLI, terminals, typing into live sessions
    cli.cpp
    pty.cpp
    vt.cpp
    attach.cpp
    launcher.cpp
    approvals.cpp
    # what Claude Code writes: the roster, transcripts
    roster.cpp
    transcript.cpp
    # what msga keeps: the team, answers' files, worktrees, the session catalog
    roles.cpp
    avatar_glyphs.cpp
    outputs.cpp
    worktrees.cpp
    catalog.cpp
    # the workspace backend
    render.cpp
    backend.cpp
    backend_send.cpp
    backend_manage.cpp
    backend_branch.cpp
    # agent thread links: Slack threads answered by sessions
    links.cpp
)
target_link_libraries(msga_app_claude PUBLIC msga_app_core msga_flags PRIVATE msga_gfx)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    # openpty/forkpty live in libutil on glibc (in libc on macOS and musl).
    include(CheckLibraryExists)
    check_library_exists(util openpty "" MSGA_HAVE_LIBUTIL)
    if(MSGA_HAVE_LIBUTIL)
        target_link_libraries(msga_app_claude PRIVATE util)
    endif()
endif()
find_package(Threads REQUIRED)
target_link_libraries(msga_app_claude PUBLIC Threads::Threads)
