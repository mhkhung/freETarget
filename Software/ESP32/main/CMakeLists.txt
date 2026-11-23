# Compile every C file under main/.  Paths are relative to this file and use the
# exact on-disk case so the project builds on case-sensitive file systems
# (Linux, Docker, CI) as well as on Windows and macOS.
# (Run "idf.py reconfigure" after adding or removing a .c file.)
file(GLOB_RECURSE app_sources "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                                    "TCPIP"
                                    "drivers"
                                    "HTML"
                       EMBED_FILES "HTML/FreeETarget.html"
                                   "HTML/FreeETargetIcon.png"
                                   "HTML/help.html"
                                   "HTML/menu.html"
                                   "HTML/live.html"
                                   "HTML/app.html"
                                   "TCPIP/ca_cert.pem"
                       )
