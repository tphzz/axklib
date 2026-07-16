file(GLOB_RECURSE AXK_CPP_FILES
  LIST_DIRECTORIES false
  "${AXK_SOURCE_ROOT}/library/*.cpp"
  "${AXK_SOURCE_ROOT}/library/*.hpp"
  "${AXK_SOURCE_ROOT}/apps/application/*.cpp"
  "${AXK_SOURCE_ROOT}/apps/application/*.hpp"
  "${AXK_SOURCE_ROOT}/apps/cli/*.cpp"
  "${AXK_SOURCE_ROOT}/apps/cli/*.hpp")

foreach(AXK_CPP_FILE IN LISTS AXK_CPP_FILES)
  file(READ "${AXK_CPP_FILE}" AXK_CPP_CONTENT)
  if(AXK_CPP_CONTENT MATCHES "[#]include[ \t]*[<\"]crow")
    message(FATAL_ERROR "Crow include escaped the server adapter: ${AXK_CPP_FILE}")
  endif()
endforeach()

file(GLOB_RECURSE AXK_APPLICATION_CPP_FILES
  LIST_DIRECTORIES false
  "${AXK_SOURCE_ROOT}/library/*.cpp"
  "${AXK_SOURCE_ROOT}/library/*.hpp"
  "${AXK_SOURCE_ROOT}/apps/application/*.cpp"
  "${AXK_SOURCE_ROOT}/apps/application/*.hpp")

foreach(AXK_APPLICATION_CPP_FILE IN LISTS AXK_APPLICATION_CPP_FILES)
  file(READ "${AXK_APPLICATION_CPP_FILE}" AXK_APPLICATION_CPP_CONTENT)
  if(AXK_APPLICATION_CPP_CONTENT MATCHES "[#]include[ \t]*[<\"]CLI/")
    message(FATAL_ERROR "CLI11 include escaped the CLI adapter: ${AXK_APPLICATION_CPP_FILE}")
  endif()
endforeach()

file(GLOB_RECURSE AXK_INSTALLED_SDK_HEADERS
  LIST_DIRECTORIES false
  "${AXK_SOURCE_ROOT}/library/include/*.h"
  "${AXK_SOURCE_ROOT}/library/include/*.hpp")

foreach(AXK_INSTALLED_SDK_HEADER IN LISTS AXK_INSTALLED_SDK_HEADERS)
  file(READ "${AXK_INSTALLED_SDK_HEADER}" AXK_INSTALLED_SDK_CONTENT)
  if(AXK_INSTALLED_SDK_CONTENT MATCHES "axklib/application")
    message(FATAL_ERROR "Internal application service escaped the installed SDK: ${AXK_INSTALLED_SDK_HEADER}")
  endif()
endforeach()

file(READ "${AXK_SOURCE_ROOT}/apps/cli/local_operations.cpp" AXK_CLI_LOCAL_OPERATIONS)
if(NOT AXK_CLI_LOCAL_OPERATIONS MATCHES "make_application_registry")
  message(FATAL_ERROR "The CLI adapter does not consume the shared application-registry factory")
endif()
if(AXK_CLI_LOCAL_OPERATIONS MATCHES "bind_application_operations")
  message(FATAL_ERROR "The CLI adapter binds application operations independently")
endif()

file(GLOB_RECURSE AXK_SERVER_SOURCE_FILES
  LIST_DIRECTORIES false
  "${AXK_SOURCE_ROOT}/apps/server/src/*.cpp"
  "${AXK_SOURCE_ROOT}/apps/server/include/*.hpp")

set(AXK_SERVER_USES_CROW FALSE)
foreach(AXK_SERVER_SOURCE_FILE IN LISTS AXK_SERVER_SOURCE_FILES)
  file(READ "${AXK_SERVER_SOURCE_FILE}" AXK_SERVER_SOURCE)
  if(AXK_SERVER_SOURCE MATCHES "[#]include[ \t]*[<\"]crow")
    set(AXK_SERVER_USES_CROW TRUE)
  endif()
  if(AXK_SERVER_SOURCE MATCHES "boost/beast|cpp-httplib|httplib[.]h|websocketpp|pistache|restinio")
    message(FATAL_ERROR "A second HTTP or WebSocket framework entered the Crow adapter: ${AXK_SERVER_SOURCE_FILE}")
  endif()
  if(AXK_SERVER_SOURCE MATCHES
     "report[.](info|objects|relationships|inventory|coverage|orphans|validate)|extract[.](wav|sfz)|package[.](export|inspect|verify|plan_import|import)|create[.](hds|floppy|iso|manifest)|alter[.](hds|manifest)")
    message(FATAL_ERROR "A domain operation ID was hard-coded in the Crow adapter: ${AXK_SERVER_SOURCE_FILE}")
  endif()
  if(AXK_SERVER_SOURCE MATCHES
     "bind_(file|extraction|package|write)_operations|application/(file|extraction|package|write)_operations[.]hpp")
    message(FATAL_ERROR "The Crow adapter knows an individual application-operation family: ${AXK_SERVER_SOURCE_FILE}")
  endif()
endforeach()

file(READ "${AXK_SOURCE_ROOT}/apps/server/src/server.cpp" AXK_SERVER_IMPLEMENTATION)
if(NOT AXK_SERVER_IMPLEMENTATION MATCHES "make_application_registry")
  message(FATAL_ERROR "The Crow adapter does not consume the shared application-registry factory")
endif()
if(AXK_SERVER_IMPLEMENTATION MATCHES "bind_application_operations")
  message(FATAL_ERROR "The Crow adapter binds application operations independently")
endif()

if(NOT AXK_SERVER_USES_CROW)
  message(FATAL_ERROR "The server adapter does not include upstream Crow")
endif()

file(READ "${AXK_SOURCE_ROOT}/apps/server/CMakeLists.txt" AXK_SERVER_CMAKE)
if(NOT AXK_SERVER_CMAKE MATCHES "find_package[(]Crow CONFIG REQUIRED[)]" OR
   NOT AXK_SERVER_CMAKE MATCHES "Crow::Crow")
  message(FATAL_ERROR "The server target is not linked to the upstream Crow package")
endif()
