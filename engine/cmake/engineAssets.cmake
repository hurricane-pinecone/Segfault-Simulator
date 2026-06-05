# Sets ENGINE_ASSET_DIR to the engine's installed runtime assets (default font,
# etc.), resolved relative to this module so it holds from any install prefix or
# Conan package folder. Consuming projects copy these next to their executable.
#
# Installed to <prefix>/<datadir>/sfs-engine/cmake/ alongside ../assets, and
# registered as a Conan build module (see conanfile.py package_info) so it is
# auto-included by both raw find_package and Conan consumers.
get_filename_component(ENGINE_ASSET_DIR "${CMAKE_CURRENT_LIST_DIR}/../assets" ABSOLUTE)
