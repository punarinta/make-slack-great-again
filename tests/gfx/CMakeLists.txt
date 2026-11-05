# gfx: one binary, one ctest test per case group. Pictures from ../assets
# (avatars, GIFs, svg/), plus the repository's own art in ../../gfx.
add_executable(gfx_tests gfx_tests.cpp)
target_link_libraries(gfx_tests PRIVATE msga_gfx)
target_compile_definitions(gfx_tests PRIVATE
    MSGA_TEST_ASSETS="${MSGA_TEST_ASSETS}"
    GFX_REPO_ART="${MSGA_ROOT}/gfx")
foreach(t blend fill clip roundrect path stroke bitmap blit gradient shadow decode anim gifrestore animbudget snapped icons
        paint2 svg svgsize svgfuzz cover resizeaxes scratch svglayers)
    add_test(NAME gfx_${t} COMMAND gfx_tests ${t})
endforeach()
