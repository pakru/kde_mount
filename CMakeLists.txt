cmake_minimum_required(VERSION 3.20)
project(nasmount VERSION 0.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# nasmount-core/-session are STATIC (deliberately: see below) but kcm_nasmount
# links them into a plugin (a shared object), which requires PIC code.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Concurrent Quick QuickControls2)
find_package(KF6 6.0 REQUIRED COMPONENTS Auth I18n WidgetsAddons Config CoreAddons KCMUtils)

set(CMAKE_AUTOMOC ON)

set(NASMOUNT_HELPER_ID "io.github.pakru.nasmount")

# ---------------------------------------------------------------------------
# core — validation, unit-value encoding, and the read-only state model
# (docs/credential-modes-design.md §§4-6). Linked into every binary,
# including the privileged helper, which is exactly why it stays a STATIC
# library rather than a shared one: the helper must have no runtime
# dependency on a .so a non-root user could replace.
# ---------------------------------------------------------------------------
add_library(nasmount-core STATIC
    src/core/unitspec.cpp
    src/core/unitvalue.cpp
    src/core/verify.cpp
)
target_include_directories(nasmount-core PUBLIC src/core)
target_link_libraries(nasmount-core PUBLIC Qt6::Core)

# ---------------------------------------------------------------------------
# session — Store (KConfig), the per-user lock, the KAuth call
# wrapper, the async operation controller, and the display model. Never
# linked into the helper (plan §8: "session -> dialog, kcm --
# never the helper").
# ---------------------------------------------------------------------------
add_library(nasmount-session STATIC
    src/session/cleanupvalidation.cpp
    src/session/store.cpp
    src/session/userlock.cpp
    src/session/helperinvoke.cpp
    src/session/mountactions.cpp
    src/session/mountmodel.cpp
)
target_include_directories(nasmount-session PUBLIC src/session)
target_link_libraries(nasmount-session PUBLIC
    nasmount-core
    Qt6::Core Qt6::Concurrent
    KF6::AuthCore KF6::ConfigCore
)

# ---------------------------------------------------------------------------
# root — the private, privileged library: descriptor-based durable filesystem
# primitives, the root lock, systemd command execution, and credential/
# runtime artifact stores (plan §2.1). Linked ONLY into privileged
# executables (nasmount-helper, nasmount-boot). NEVER linked into
# nasmount-session, the KCM, or the dialog — those are
# reachable by an unprivileged, potentially hostile local process, and must
# have no code path that can mutate root-owned state. Enforced below by
# nasmount_assert_no_root_link(), not just by this comment.
# ---------------------------------------------------------------------------
add_library(nasmount-root STATIC
    src/root/arming.cpp
    src/root/credentialstore.cpp
    src/root/durablefs.cpp
    src/root/inventory.cpp
    src/root/operations.cpp
    src/root/rootlock.cpp
    src/root/runtimefiles.cpp
    src/root/systemdops.cpp
)
target_include_directories(nasmount-root PUBLIC src/root)
target_link_libraries(nasmount-root PUBLIC nasmount-core Qt6::Core)

# --- privileged KAuth helper ------------------------------------------------
add_executable(nasmount-helper src/helper/helper.cpp)
# No Widgets, no I18n, no session library: the root helper produces no UI, no
# translated strings, and must not link anything that talks to KConfig
# on the caller's behalf. libQt6Gui still appears in ldd output as a transitive
# dependency of KF6CoreAddons, which KAuth requires — but nothing here
# constructs a QGuiApplication, so no GUI machinery is initialised.
target_link_libraries(nasmount-helper PRIVATE nasmount-core nasmount-root Qt6::Core KF6::AuthCore)
install(TARGETS nasmount-helper DESTINATION ${KAUTH_HELPER_INSTALL_DIR})

# Generates and installs the D-Bus policy + system service file that let the
# helper own its name on the system bus and be activated on demand.
kauth_install_helper_files(nasmount-helper ${NASMOUNT_HELPER_ID} root)

# Turns the .actions file into a polkit .policy via kauth-policy-gen.
kauth_install_actions(${NASMOUNT_HELPER_ID} ${CMAKE_CURRENT_SOURCE_DIR}/${NASMOUNT_HELPER_ID}.actions)

# --- privileged boot coordinator (phase 4) ----------------------------------
# Started directly by systemd as root (nasmount-boot.service) -- never D-Bus/
# KAuth activated, so no .actions entry and no polkit policy: there is no
# caller to authorize, only root arming triggers it already owns (design
# §6.3). Installed into the KDE libexec dir, matching the service template's
# ExecStart=.
add_executable(nasmount-boot src/boot/main.cpp)
target_link_libraries(nasmount-boot PRIVATE nasmount-core nasmount-root Qt6::Core)
install(TARGETS nasmount-boot DESTINATION ${KDE_INSTALL_LIBEXECDIR})

configure_file(systemd/nasmount-boot.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/nasmount-boot.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nasmount-boot.service
        DESTINATION ${KDE_INSTALL_SYSTEMDUNITDIR}/system)

# --- unprivileged GUI: Dolphin service-menu dialog --------------------------
# dialog.qrc embeds ShareForm.qml from src/kcm/ui -- one form definition
# serving both front ends, rather than a second copy that can drift.
qt_add_resources(NASMOUNT_DIALOG_QRC src/dialog/dialog.qrc)
add_executable(nasmount-dialog
    src/dialog/main.cpp
    src/dialog/dialogbackend.cpp
    src/dialog/smburl.cpp
    ${NASMOUNT_DIALOG_QRC}
)
target_link_libraries(nasmount-dialog
    PRIVATE nasmount-core nasmount-session
            Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2
            KF6::I18n
)
install(TARGETS nasmount-dialog DESTINATION ${KDE_INSTALL_BINDIR})

# --- authenticated uninstall coordinator -----------------------------------
# Runs as the user while the helper/policy still exist. It validates the
# install manifest, invokes the single auth_admin purge action, then removes
# config data only after the privileged purge is confirmed.
add_executable(nasmount-cleanup src/cleanup/main.cpp)
target_link_libraries(nasmount-cleanup PRIVATE nasmount-session Qt6::Core)
install(TARGETS nasmount-cleanup DESTINATION ${KDE_INSTALL_BINDIR})


# --- KCM: System Settings module --------------------------------------------
add_subdirectory(src/kcm)

# --- Dolphin service menu ---------------------------------------------------
# Exec= is substituted with the absolute install path rather than relying on
# PATH. Dolphin launches service-menu actions as transient systemd user units,
# whose PATH is the systemd user manager's — not a shell's.
configure_file(servicemenu/nasmount.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/nasmount.desktop @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nasmount.desktop
        DESTINATION ${KDE_INSTALL_DATADIR}/kio/servicemenus
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)

# --- linkage invariant: nasmount-root reaches only privileged executables ---
# (plan §2.1.2). Structural placement (never adding it to these
# target_link_libraries calls) is the primary defence; this is the check that
# catches a future accidental addition instead of relying on that forever.
function(nasmount_assert_no_root_link target)
    get_target_property(_nasmount_libs ${target} LINK_LIBRARIES)
    if(_nasmount_libs AND "nasmount-root" IN_LIST _nasmount_libs)
        message(FATAL_ERROR
            "${target} must never link nasmount-root (plan 2.1.2): privileged "
            "filesystem/systemd mutation code must not be reachable from an "
            "unprivileged, potentially hostile local process.")
    endif()
endfunction()

nasmount_assert_no_root_link(nasmount-session)
nasmount_assert_no_root_link(nasmount-dialog)
nasmount_assert_no_root_link(nasmount-cleanup)
nasmount_assert_no_root_link(kcm_nasmount)

# --- tests -------------------------------------------------------------------
# Not installed; built so `ctest` can exercise the same validation/encoding/
# verification code the privileged helper relies on.
enable_testing()

add_executable(unitspec_test tests/unitspec_test.cpp)
target_link_libraries(unitspec_test PRIVATE nasmount-core)
add_test(NAME unitspec COMMAND unitspec_test)

# smburl links the dialog's own translation unit directly rather than a
# library: it is the only non-GUI part of nasmount-dialog, and pulling it into
# a library purely for the test would be more structure than it earns.
add_executable(smburl_test tests/smburl_test.cpp src/dialog/smburl.cpp)
target_include_directories(smburl_test PRIVATE src/dialog)
target_link_libraries(smburl_test PRIVATE nasmount-core KF6::I18n)
add_test(NAME smburl COMMAND smburl_test)

add_executable(unitvalue_test tests/unitvalue_test.cpp)
target_link_libraries(unitvalue_test PRIVATE nasmount-core)
add_test(NAME unitvalue COMMAND unitvalue_test)

add_executable(verify_test tests/verify_test.cpp)
target_link_libraries(verify_test PRIVATE nasmount-core)
add_test(NAME verify COMMAND verify_test)

add_executable(helperinvoke_test tests/helperinvoke_test.cpp)
target_link_libraries(helperinvoke_test PRIVATE nasmount-session)
add_test(NAME helperinvoke COMMAND helperinvoke_test)

add_executable(store_test tests/store_test.cpp)
target_link_libraries(store_test PRIVATE nasmount-session KF6::ConfigCore)
add_test(NAME store COMMAND store_test)

add_executable(mountactions_test tests/mountactions_test.cpp)
target_link_libraries(mountactions_test PRIVATE nasmount-session)
add_test(NAME mountactions COMMAND mountactions_test)

add_executable(mountmodel_test tests/mountmodel_test.cpp)
target_link_libraries(mountmodel_test PRIVATE nasmount-session)
add_test(NAME mountmodel COMMAND mountmodel_test)

add_executable(durablefs_test tests/durablefs_test.cpp)
target_link_libraries(durablefs_test PRIVATE nasmount-root)
add_test(NAME durablefs COMMAND durablefs_test)

add_executable(inventory_test tests/inventory_test.cpp)
target_link_libraries(inventory_test PRIVATE nasmount-root)
add_test(NAME inventory COMMAND inventory_test)

add_executable(operations_test tests/operations_test.cpp)
target_link_libraries(operations_test PRIVATE nasmount-root)
add_test(NAME operations COMMAND operations_test)

add_executable(arming_test tests/arming_test.cpp)
target_link_libraries(arming_test PRIVATE nasmount-root)
add_test(NAME arming COMMAND arming_test)

add_executable(credentialstore_test tests/credentialstore_test.cpp)
target_link_libraries(credentialstore_test PRIVATE nasmount-root)
add_test(NAME credentialstore COMMAND credentialstore_test)

add_executable(cleanupvalidation_test tests/cleanupvalidation_test.cpp)
target_link_libraries(cleanupvalidation_test PRIVATE nasmount-session)
add_test(NAME cleanupvalidation COMMAND cleanupvalidation_test)

# The QML/C++ boundary is invisible to the compiler, qmllint and every ctest
# binary: QML resolves these names at runtime against a QObject exposed as
# `var`. This gate is the only thing that catches a C++ rename leaving a QML
# call site behind.
add_test(NAME qml_invokable_gate
         COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/qml_invokable_gate.sh)

add_test(NAME removed_api_gates
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/removed_api_gates.sh)
set_tests_properties(removed_api_gates PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
