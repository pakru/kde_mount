kcmutils_add_qml_kcm(kcm_nasmount
    SOURCES
        kcmnasmount.cpp
        kcmnasmount.h
)
target_link_libraries(kcm_nasmount PRIVATE
    nasmount-core
    nasmount-session
    Qt6::Core
    Qt6::Quick
    KF6::KCMUtilsQuick
    KF6::CoreAddons
)
