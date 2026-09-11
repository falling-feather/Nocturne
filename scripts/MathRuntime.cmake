# Pinned, source-built native renderer. No TeX installation or browser runtime.
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(nocturne_microtex
    URL https://codeload.github.com/NanoMichael/MicroTeX/tar.gz/0e3707f6dafebb121d98b53c64364d16fefe481d
    URL_HASH SHA256=47476269d29c41df322bce6bdd2daa7017cc50b9eda841b2bec1767dba28daa6
    SOURCE_SUBDIR nocturne-unused-upstream-build)
FetchContent_Declare(nocturne_tinyxml
    URL https://codeload.github.com/leethomason/tinyxml2/tar.gz/321ea883b7190d4e85cae5512a12e5eaa8f8731f
    URL_HASH SHA256=d5ae097578717b42b4f05f2c6b2d09f473a1c30d1552ca4e2dcb4eab5b34de84
    SOURCE_SUBDIR nocturne-unused-upstream-build)
FetchContent_MakeAvailable(nocturne_microtex nocturne_tinyxml)

set(math_generated "${CMAKE_CURRENT_BINARY_DIR}/math-runtime")
file(MAKE_DIRECTORY "${math_generated}")
# Upstream's Qt adapter still instantiates QFontDatabase, removed in Qt 6.
file(READ "${nocturne_microtex_SOURCE_DIR}/src/platform/qt/graphic_qt.cpp" math_qt)
string(REPLACE "QFontDatabase db;" "" math_qt "${math_qt}")
string(REPLACE "db.addApplicationFont" "QFontDatabase::addApplicationFont" math_qt "${math_qt}")
string(REPLACE "db.applicationFontFamilies" "QFontDatabase::applicationFontFamilies" math_qt "${math_qt}")
string(REPLACE "#include <QPainter>" "#include <QPainter>\n#include <QPainterPath>" math_qt "${math_qt}")
# MicroTeX draws unit-sized glyphs with a large painter transform. The FreeType
# raster backend can fail on these tiny cached glyphs. Use unhinted outlines at
# a stable size, then scale them to the same coordinates.
string(REPLACE "_painter->drawText(QPointF(x, y), text);" [=[
  QFont outlineFont = _font->getQFont();
  const qreal ratio = outlineFont.pointSizeF() / 64.0;
  outlineFont.setPixelSize(64); // TeX font metrics are pixel units, independent of screen DPI.
  outlineFont.setHintingPreference(QFont::PreferNoHinting);
  QPainterPath path;
  path.addText(QPointF(0, 0), outlineFont, text);
  _painter->save();
  _painter->translate(x, y);
  _painter->scale(ratio, ratio);
  _painter->fillPath(path, getQBrush());
  _painter->restore();
]=] math_qt "${math_qt}")
file(WRITE "${math_generated}/graphic_qt.cpp" "${math_qt}")
# Resources are embedded; never search the working directory or environment.
file(READ "${nocturne_microtex_SOURCE_DIR}/src/latex.cpp" math_latex)
string(REPLACE "auto path = queryResourceLocation(res_root_path);" "auto path = res_root_path;" math_latex "${math_latex}")
file(WRITE "${math_generated}/latex.cpp" "${math_latex}")
file(GLOB_RECURSE math_sources CONFIGURE_DEPENDS
    "${nocturne_microtex_SOURCE_DIR}/src/atom/*.cpp"
    "${nocturne_microtex_SOURCE_DIR}/src/box/*.cpp"
    "${nocturne_microtex_SOURCE_DIR}/src/core/*.cpp"
    "${nocturne_microtex_SOURCE_DIR}/src/fonts/*.cpp"
    "${nocturne_microtex_SOURCE_DIR}/src/utils/*.cpp"
    "${nocturne_microtex_SOURCE_DIR}/src/res/*.cpp")
add_library(NocturneMathRuntime STATIC ${math_sources}
    "${math_generated}/graphic_qt.cpp" "${math_generated}/latex.cpp"
    "${nocturne_microtex_SOURCE_DIR}/src/render.cpp"
    "${nocturne_tinyxml_SOURCE_DIR}/tinyxml2.cpp")
set_target_properties(NocturneMathRuntime PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(NocturneMathRuntime PUBLIC "${nocturne_microtex_SOURCE_DIR}/src"
    PRIVATE "${nocturne_tinyxml_SOURCE_DIR}" "${nocturne_microtex_SOURCE_DIR}/src/platform/qt")
target_compile_definitions(NocturneMathRuntime PUBLIC BUILD_QT PRIVATE NOMINMAX)
target_link_libraries(NocturneMathRuntime PRIVATE Qt6::Gui)
file(GLOB_RECURSE math_fonts "${nocturne_microtex_SOURCE_DIR}/res/fonts/*.ttf")
set(math_qrc "<RCC><qresource prefix=\"/nocturne-math\">\n")
foreach(font IN LISTS math_fonts)
    file(RELATIVE_PATH alias "${nocturne_microtex_SOURCE_DIR}/res" "${font}")
    string(APPEND math_qrc "<file alias=\"${alias}\">${font}</file>\n")
endforeach()
string(APPEND math_qrc "</qresource></RCC>\n")
file(WRITE "${math_generated}/MathFonts.qrc" "${math_qrc}")
target_sources(NocturneMathRuntime PRIVATE "${math_generated}/MathFonts.qrc")
file(MAKE_DIRECTORY "${math_generated}/licenses/MicroTeX" "${math_generated}/licenses/tinyxml2")
configure_file("${nocturne_microtex_SOURCE_DIR}/LICENSE" "${math_generated}/licenses/MicroTeX/LICENSE" COPYONLY)
file(COPY "${nocturne_microtex_SOURCE_DIR}/res/fonts/licences" DESTINATION "${math_generated}/licenses/MicroTeX")
configure_file("${nocturne_tinyxml_SOURCE_DIR}/LICENSE.txt" "${math_generated}/licenses/tinyxml2/LICENSE.txt" COPYONLY)
