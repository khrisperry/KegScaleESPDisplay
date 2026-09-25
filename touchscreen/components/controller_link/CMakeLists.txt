# Canonical controller_link component. The Touch repository keeps a vendored
# byte-for-byte mirror for standalone/offline builds. Update the Scale copy
# first, then run tools/sync_controller_link.py --write from the Scale repo or
# tools/sync_controller_link.py from the Display repo.
idf_component_register(SRCS "controller_link.c" INCLUDE_DIRS "include" REQUIRES mbedtls)
