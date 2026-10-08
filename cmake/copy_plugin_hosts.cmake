cmake_minimum_required(VERSION 3.25)
# cmake -DDIRS=<folder|...> -DHOSTS=<file name|...> -DDST=<folder> -DOWN=<file name>
#       -DSUFFIX=<.exe or nothing> -P copy_plugin_hosts.cmake
# Copies each plugin host named in HOSTS from the first of DIRS that has it into DST (when it
# differs), warns about those none has, and removes from DST the plugin hosts
# (brack-host-*<SUFFIX>) neither in HOSTS nor OWN.
string(REPLACE "|" ";" DIRS "${DIRS}")
string(REPLACE "|" ";" HOSTS "${HOSTS}")
foreach(name IN LISTS HOSTS)
  set(found "")
  foreach(dir IN LISTS DIRS)
    if(NOT found AND EXISTS "${dir}/${name}")
      set(found "${dir}/${name}")
    endif()
  endforeach()
  if(found)
    file(COPY_FILE "${found}" "${DST}/${name}" ONLY_IF_DIFFERENT)
    file(CHMOD "${DST}/${name}" FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
         GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
  else()
    message(WARNING "${name}, which this build ships, is in none of ${DIRS}: build that architecture too "
                    "(docs/development.md, Build), or its plugins will not load")
  endif()
endforeach()
file(GLOB present "${DST}/brack-host-*${SUFFIX}")
foreach(host IN LISTS present)
  get_filename_component(name "${host}" NAME)
  if(NOT name STREQUAL OWN AND NOT name IN_LIST HOSTS)
    file(REMOVE "${host}")
  endif()
endforeach()
