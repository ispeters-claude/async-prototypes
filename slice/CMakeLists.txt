# SPDX-License-Identifier: MIT

# stdexec provides the completion_signatures machinery and stop tokens, as
# for typed-handle.
async_prototypes_stdexec(30acdb0)

# The checks in every configuration: TH_MODE (EAGER 0, ERASED 1, TYPED 2,
# VARIANT 3) by SLICE_DEST (CHAIN 0, ONCE 1, RECORD 2, OUT 3) by SLICE_ALLOC
# (at connect 0, at start 1).
foreach(mode 0 1 2 3)
    foreach(dest 0 1 2 3)
        foreach(alloc 0 1)
            set(config ${mode}.${dest}.${alloc})
            add_executable(async_prototypes.slice.checks.${config})
            target_sources(async_prototypes.slice.checks.${config} PRIVATE checks.cpp stores.cpp)
            target_compile_definitions(
                async_prototypes.slice.checks.${config}
                PRIVATE TH_MODE=${mode} SLICE_DEST=${dest} SLICE_ALLOC=${alloc} SLICE_COUNT
            )
            target_link_libraries(
                async_prototypes.slice.checks.${config}
                PRIVATE async_prototypes.stdexec.30acdb0
            )
            add_test(
                NAME slice.checks.${config}
                COMMAND async_prototypes.slice.checks.${config}
            )
        endforeach()
    endforeach()
endforeach()

# The bench in the default configuration (VARIANT, CHAIN, at connect), with
# a short run of every scenario; cg.sh builds the others.
add_executable(async_prototypes.slice.bench)
target_sources(async_prototypes.slice.bench PRIVATE bench.cpp stores.cpp)
target_compile_definitions(async_prototypes.slice.bench PRIVATE TH_MODE=3)
target_link_libraries(async_prototypes.slice.bench PRIVATE async_prototypes.stdexec.30acdb0)
add_test(NAME slice.bench COMMAND sh -c "for s in $(\"$<TARGET_FILE:async_prototypes.slice.bench>\" list); do \"$<TARGET_FILE:async_prototypes.slice.bench>\" $s 100 || exit 1; done")

foreach(mode 2 3)
    foreach(firewall 0 4)
        add_executable(async_prototypes.slice.build_time.${mode}.${firewall})
        target_sources(async_prototypes.slice.build_time.${mode}.${firewall} PRIVATE build_time.cpp)
        target_compile_definitions(
            async_prototypes.slice.build_time.${mode}.${firewall}
            PRIVATE TH_MODE=${mode} DEPTH=16 FIREWALL=${firewall}
        )
        target_link_libraries(
            async_prototypes.slice.build_time.${mode}.${firewall}
            PRIVATE async_prototypes.stdexec.30acdb0
        )
        add_test(
            NAME slice.build_time.${mode}.${firewall}
            COMMAND async_prototypes.slice.build_time.${mode}.${firewall}
        )
    endforeach()
endforeach()
