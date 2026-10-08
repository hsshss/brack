cmake_minimum_required(VERSION 3.25)
# cmake -DDIR=<folder> -DIDENTITY=<identity> -DENTITLEMENTS=<file> -P codesign_hosts.cmake
# Signs every plugin host in DIR (brack-host-*) with the Hardened Runtime: those copied in from
# other architectures' builds too, which may carry no signature (the linker signs arm64 only).
file(GLOB hosts "${DIR}/brack-host-*")
foreach(host IN LISTS hosts)
  execute_process(COMMAND codesign --force --sign "${IDENTITY}" --options runtime --entitlements "${ENTITLEMENTS}" "${host}"
                  RESULT_VARIABLE result)
  if(result)
    message(FATAL_ERROR "codesign failed for ${host}")
  endif()
endforeach()
