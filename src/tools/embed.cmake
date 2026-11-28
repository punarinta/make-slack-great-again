# Writes a binary file as a C++ byte array (cmake -P, no Python needed):
#   cmake -DIN=<file> -DOUT=<file.cpp> -DNAMESPACE=<ns> -DNAME=<symbol> -P embed.cmake
# OUT defines `extern const unsigned char NAME[]` and `extern const unsigned
# NAME_size` in namespace NAMESPACE.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" chars)
math(EXPR size "${chars} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
get_filename_component(inName "${IN}" NAME)
file(WRITE "${OUT}" "// Generated from ${inName} by src/tools/embed.cmake. Do not edit.\n"
    "namespace ${NAMESPACE} {\n"
    "extern const unsigned char ${NAME}[];\n"
    "extern const unsigned      ${NAME}_size;\n"
    "const unsigned char ${NAME}[] = {\n${bytes}};\n"
    "const unsigned ${NAME}_size = ${size};\n"
    "} // namespace ${NAMESPACE}\n")
