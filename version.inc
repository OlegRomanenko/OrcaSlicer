# Общие метаданные приложения для CMake, интерфейса и шаблонов упаковки.
# Отображаемая версия берётся из подходящей локальной ветки; без неё действует
# штатная строка апстрима. Git используется только для чтения, исходники не меняются.
# Внутренняя `SLIC3R_VERSION` не зависит от имени ветки и сохраняет совместимость.

set(SLIC3R_APP_NAME "OrcaSlicer")
set(SLIC3R_APP_KEY "OrcaSlicer")

if(NOT DEFINED BBL_INTERNAL_TESTING)
set(BBL_INTERNAL_TESTING "0")
endif()
set(SoftFever_VERSION "2.5.0-dev")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git")
    # Собственная `.git` нужна и для worktree, и для защиты архива от чужой ветки
    # родительского репозитория. Полный ref не зависит от одноимённого тега;
    # `detached HEAD` не является именем ветки.
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_LIST_DIR}" symbolic-ref --quiet HEAD
        RESULT_VARIABLE _orca_git_result OUTPUT_VARIABLE _orca_branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_orca_git_result EQUAL 0 AND
       _orca_branch MATCHES "^refs/heads/v[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z]+([.-][0-9A-Za-z]+)*)?$")
        string(REGEX REPLACE "^refs/heads/v" "" SoftFever_VERSION "${_orca_branch}")
    endif()

    # Смена ветки должна обновлять уже настроенную сборку. Путь к `HEAD` сообщает
    # Git: у worktree он находится не в `.git/HEAD` внутри каталога исходников.
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_LIST_DIR}" rev-parse --git-path HEAD
            RESULT_VARIABLE _orca_git_result OUTPUT_VARIABLE _orca_git_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_orca_git_result EQUAL 0)
            get_filename_component(_orca_git_head "${_orca_git_head}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_orca_git_head}")
        endif()
    endif()
endif()
unset(_orca_branch)
unset(_orca_git_head)
unset(_orca_git_result)

string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)"
       SoftFever_VERSION_MATCH ${SoftFever_VERSION})
set(ORCA_VERSION_MAJOR ${CMAKE_MATCH_1})
set(ORCA_VERSION_MINOR ${CMAKE_MATCH_2})
set(ORCA_VERSION_PATCH ${CMAKE_MATCH_3})

set(SLIC3R_VERSION "02.08.01.55")

if (NOT DEFINED ORCA_UPDATER_SIG_KEY)
    set(ORCA_UPDATER_SIG_KEY "" CACHE STRING "Base64url encoded updater signature key")
endif()
