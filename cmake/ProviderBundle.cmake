# Bundled providers: the pinned packages in providers/lock.json, unpacked at
# configure time into a Qt resource at qrc:/providers/<id>/. Nothing is
# downloaded here; a digest mismatch stops the configure.
#
# For provider development, SPOOL_PROVIDER_OVERRIDES takes id=/path/to/checkout
# pairs and bundles those working trees instead of their pins.

set(SPOOL_PROVIDER_OVERRIDES "" CACHE STRING "id=/path pairs bundling provider checkouts instead of pinned packages")

function(spool_bundle_providers target)
    set(lock "${CMAKE_CURRENT_SOURCE_DIR}/providers/lock.json")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/bundled-providers")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lock}")
    file(READ "${lock}" json)
    string(JSON count LENGTH "${json}" providers)
    math(EXPR last "${count} - 1")
    set(qrc "<RCC>\n")
    foreach(index RANGE 0 ${last})
        string(JSON id GET "${json}" providers ${index} id)
        string(JSON archive GET "${json}" providers ${index} archive)
        string(JSON expected GET "${json}" providers ${index} sha256)
        set(root "${out}/${id}")
        set(override "")
        foreach(pair IN LISTS SPOOL_PROVIDER_OVERRIDES)
            if(pair MATCHES "^${id}=(.+)$")
                set(override "${CMAKE_MATCH_1}")
            endif()
        endforeach()
        file(REMOVE_RECURSE "${root}")
        if(override)
            file(GLOB_RECURSE sources CONFIGURE_DEPENDS RELATIVE "${override}"
                "${override}/manifest.json" "${override}/LICENSE" "${override}/NOTICE"
                "${override}/logic/*" "${override}/ui/*" "${override}/assets/*")
            foreach(source IN LISTS sources)
                get_filename_component(directory "${root}/${source}" DIRECTORY)
                file(COPY "${override}/${source}" DESTINATION "${directory}")
            endforeach()
        else()
            set(package "${CMAKE_CURRENT_SOURCE_DIR}/providers/${archive}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${package}")
            file(SHA256 "${package}" actual)
            if(NOT actual STREQUAL expected)
                message(FATAL_ERROR "${package} does not match its pin in providers/lock.json")
            endif()
            file(ARCHIVE_EXTRACT INPUT "${package}" DESTINATION "${root}")
            # Archive times are zeroed for reproducibility; restamp them so rcc
            # sees a changed pin as newer than the resource it built before.
            file(GLOB_RECURSE extracted "${root}/*")
            file(TOUCH_NOCREATE ${extracted})
        endif()
        file(GLOB_RECURSE files RELATIVE "${root}" "${root}/*")
        list(SORT files)
        string(APPEND qrc "  <qresource prefix=\"/providers/${id}\">\n")
        foreach(file IN LISTS files)
            string(APPEND qrc "    <file alias=\"${file}\">${root}/${file}</file>\n")
        endforeach()
        string(APPEND qrc "  </qresource>\n")
    endforeach()
    string(APPEND qrc "</RCC>\n")
    file(CONFIGURE OUTPUT "${out}/providers.qrc" CONTENT "${qrc}" @ONLY)
    qt_add_resources(resources "${out}/providers.qrc")
    target_sources(${target} PRIVATE ${resources})
    set(SPOOL_PROVIDER_BUNDLE_SOURCES ${resources} PARENT_SCOPE)
endfunction()
